// ScreenDriver coprocessor, milestone 4: link + drawing + sprites.
// Receives frames from the Cardputer, renders drawing commands into a framebuffer and flushes the changed
// areas to the ILI9341 at END_FRAME. Until the first INIT or drawing command it shows live link stats.
#include <Arduino.h>
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "panel_config.h"
#include "sd_link.h"
#include "renderer.h"
#include "soc/usb_serial_jtag_struct.h"
#ifdef SD_SELFTEST
#include "selftest.h"
static SelfTestResult selfTestResult;
#endif

#define FW_VERSION 0x0600  // M6: palette images, tilemaps, sprite batches
#define WATCHDOG_MS 3000   // the main loop must come round within this time, or the C6 reboots
#ifndef ASSET_POOL_BYTES
#define ASSET_POOL_BYTES (160 * 1024)
#endif
#define HEAP_RESERVE (40 * 1024)  // left free for the UART driver, stacks and LovyanGFX

// XIAO ESP32-C6 pins -> Cardputer-ADV EXT header
#define PIN_LINK_TX   16  // D6 -> Cardputer G15 (RX)
#define PIN_LINK_RX   17  // D7 <- Cardputer G13 (TX)
#define PIN_LINK_RTS  20  // D9 -> Cardputer G6 (CTS)
#define PIN_LINK_CTS  23  // D5 <- Cardputer G4 (RTS)
#define LINK_UART     UART_NUM_1

static LGFX_ILI9341 lcd;
static LGFX_Sprite fb(&lcd);  // 320x240 RGB565 framebuffer: all drawing goes here
static Renderer renderer;
static AssetStore assets;
static bool graphicsActive = false;  // set by INIT or the first drawing command: stops the status screen
static uint16_t frameNo = 0;
static uint16_t fpsX10 = 0;
static SdUartPort sdLink;
static SdParser parser;
static uint8_t txSeq = 0;
static uint8_t rxBuf[2048];

static struct {
  uint32_t rxBytes = 0, rxFrames = 0, crcErrors = 0, lenErrors = 0, unknownCmds = 0;
  uint32_t waitUs = 0;  // time the main loop spent waiting for link data (since the last log line)
} st;

// ---- replies ----------------------------------------------------------------
static void reply(uint8_t cmd, const uint8_t* payload = nullptr, uint16_t len = 0) {
  sdLink.sendFrame(txSeq++, cmd, payload, len);
}

static void sendHello() {
  uint8_t p[11], *w = p;
  *w++ = SD_PROTO_VERSION;
  w = sd_wr_u16(w, FW_VERSION);
  w = sd_wr_u16(w, SD_SCREEN_W);
  w = sd_wr_u16(w, SD_SCREEN_H);
  w = sd_wr_u32(w, assets.freeBytes());
  reply(SD_HELLO, p, w - p);
}

static void sendNack(uint8_t seq, uint8_t cmd, uint8_t err) {
  uint8_t p[3] = {seq, cmd, err};
  reply(SD_NACK, p, 3);
}

static void sendAck(uint8_t seq, uint8_t cmd) {
  uint8_t p[2] = {seq, cmd};
  reply(SD_ACK, p, 2);
}

static void sendStatus() {
  sdLink.pollEvents();
  uint8_t p[34], *w = p;
  w = sd_wr_u32(w, ESP.getFreeHeap());
  w = sd_wr_u32(w, assets.freeBytes());
  w = sd_wr_u16(w, fpsX10);
  w = sd_wr_u32(w, st.rxBytes);
  w = sd_wr_u32(w, st.rxFrames);
  w = sd_wr_u32(w, st.crcErrors);
  w = sd_wr_u32(w, st.lenErrors);
  w = sd_wr_u32(w, sdLink.fifoOverflows);
  w = sd_wr_u32(w, sdLink.backpressure);
  reply(SD_STATUS, p, w - p);
}

// END_FRAME: flush the dirty areas, reply FRAME_DONE, update the fps figure.
static void endFrame(bool forceFull) {
  uint32_t flushUs = renderer.flush(forceFull);
  uint8_t pl[10], *w = pl;
  w = sd_wr_u16(w, ++frameNo);
  w = sd_wr_u32(w, renderer.renderUs);
  w = sd_wr_u32(w, flushUs);
  reply(SD_FRAME_DONE, pl, w - pl);
  renderer.renderUs = 0;

  static uint32_t winStart = 0, winFrames = 0;
  uint32_t now = millis();
  winFrames++;
  if (now - winStart >= 1000) {
    fpsX10 = (uint16_t)(winFrames * 10000UL / (now - winStart));
    winStart = now;
    winFrames = 0;
  }
}

// ---- command dispatch -------------------------------------------------------
static void handleFrame() {
  const uint8_t* p = parser.payload();
  const uint16_t n = parser.payloadLen;
  st.rxFrames++;
  if (Renderer::handles(parser.cmd)) {
    graphicsActive = true;
    uint8_t err = renderer.handle(parser.cmd, p, n);
    if (err) sendNack(parser.seq, parser.cmd, err);
    return;
  }
  switch (parser.cmd) {
    case SD_NOP:
      break;
    case SD_PING:
      sdLink.sendFrame(parser.seq, SD_PONG, p, n);  // PONG echoes the PING's seq
      break;
    case SD_RESET:
      sdLink.waitTxDone();
      esp_restart();
      break;
    case SD_INIT: {
      if (n < 2 || (p[0] != 1 && p[0] != 3)) { sendNack(parser.seq, parser.cmd, SD_ERR_BAD_ARG); break; }
      lcd.setRotation(p[0]);  // landscape only (1 or 3): the framebuffer is 320x240
      graphicsActive = true;
      if (p[1] & SD_INIT_CLEAR) {
        renderer.clearAll();
        renderer.flush(true);
      }
      break;
    }
    case SD_BEGIN_FRAME:
      graphicsActive = true;
      break;
    case SD_END_FRAME:
      graphicsActive = true;
      endFrame(n >= 1 && (p[0] & SD_END_FULL_FLUSH));
      break;
    case SD_SET_BACKLIGHT:
      if (n < 1) { sendNack(parser.seq, parser.cmd, SD_ERR_BAD_LEN); break; }
      lcd.setBrightness(p[0]);
      break;
    case SD_GET_STATUS:
      sendStatus();
      break;
    // ---- assets: BEGIN and END are always answered (ACK/NACK); DATA and FREE only on error
    case SD_ASSET_BEGIN:
    case SD_ASSET_END: {
      uint8_t err = parser.cmd == SD_ASSET_BEGIN ? assets.beginAsset(p, n) : assets.endAsset(p, n);
      if (err) sendNack(parser.seq, parser.cmd, err);
      else sendAck(parser.seq, parser.cmd);
      if (parser.cmd == SD_ASSET_END)
        Serial.printf("[c6] asset %u %s, pool %lu/%lu used\n", n ? p[0] : 0, err ? "FAILED" : "ready",
                      (unsigned long)assets.used(), (unsigned long)assets.capacity());
      break;
    }
    case SD_ASSET_DATA: {
      uint8_t err = assets.data(p, n);
      if (err) sendNack(parser.seq, parser.cmd, err);
      break;
    }
    case SD_TILE_SET: {
      uint8_t err = assets.setTiles(p, n);
      if (err) sendNack(parser.seq, parser.cmd, err);
      break;
    }
    case SD_ASSET_FREE: {
      uint8_t err = assets.freeCmd(p, n);
      if (err) sendNack(parser.seq, parser.cmd, err);
      break;
    }
    case SD_DEBUG_PINS: {
      // Wire test: the host drives G4 and reads G6; we drive D9 (RTS) and read D5. Flow control is off meanwhile,
      // so the reply can always leave whatever the wires do.
      if (n < 1 || p[0] > SD_PINS_RELEASE_FC_ON) { sendNack(parser.seq, parser.cmd, SD_ERR_BAD_ARG); break; }
      uint8_t a = p[0];
      if (a == SD_PINS_RELEASE_FC_OFF || a == SD_PINS_RELEASE_FC_ON) {
        sdLink.forceRts(-1);
        sdLink.setFlowControl(a == SD_PINS_RELEASE_FC_ON);
      } else {
        sdLink.setFlowControl(false);
        if (a != SD_PINS_READ) sdLink.forceRts(a == SD_PINS_RTS_HIGH ? 1 : 0);
      }
      delay(2);  // let the levels settle
      uint8_t r[3] = {(uint8_t)sdLink.ctsLevel(), (uint8_t)sdLink.rtsLevel(), (uint8_t)sdLink.flowControl()};
      Serial.printf("[c6] pins action %u: D5=%u D9=%u fc=%u\n", a, r[0], r[1], r[2]);
      reply(SD_PINS, r, 3);
      break;
    }
    case SD_DEBUG_HANG:
      Serial.println("[c6] DEBUG_HANG: looping until the watchdog reboots us");
      for (;;) {
      }
    case SD_DEBUG_STALL: {
      if (n < 2) { sendNack(parser.seq, parser.cmd, SD_ERR_BAD_LEN); break; }
      uint16_t ms = sd_rd_u16(p);
      delay(ms > 2500 ? 2500 : ms);  // stop reading: RTS deasserts once the RX buffer fills (< watchdog)
      break;
    }
    default:
      st.unknownCmds++;
      sendNack(parser.seq, parser.cmd, SD_ERR_UNKNOWN_CMD);
      break;
  }
}

static void processBytes(const uint8_t* p, const uint8_t* end) {
  while (p < end) {
    switch (parser.push(p, end)) {
      case SdParser::FRAME: handleFrame(); break;
      case SdParser::BAD_CRC:
        st.crcErrors++;
        Serial.printf("[c6] CRC error (cmd 0x%02X seq %u)\n", parser.cmd, parser.seq);
        sendNack(parser.seq, parser.cmd, SD_ERR_CRC);
        break;
      case SdParser::BAD_LEN:
        st.lenErrors++;
        Serial.println("[c6] bad length, resyncing");
        break;
      case SdParser::NONE: break;
    }
  }
}

// ---- status screen ----------------------------------------------------------
static void drawStatus() {
  // Shown until the Cardputer takes over (INIT or the first drawing command)
  if (graphicsActive) return;
  static uint32_t lastMs = 0;
  static bool first = true;
  if (!first && millis() - lastMs < 1000) return;
  lastMs = millis();
  if (first) {
    lcd.fillScreen(TFT_BLACK);
    lcd.setFont(&fonts::Font4);
    lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    lcd.drawString("ScreenDriver", 8, 8);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    char s[64];
    snprintf(s, sizeof s, "fw %04X  proto v%d  link %lu baud", FW_VERSION, SD_PROTO_VERSION, (unsigned long)SD_LINK_BAUD);
    lcd.drawString(s, 8, 40);
    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.drawString("waiting for the Cardputer...", 8, 64);
    first = false;
  }
  lcd.setFont(&fonts::Font2);
  lcd.setTextPadding(300);
  int y = 100;
  auto line = [&](uint16_t color, const char* fmt, auto... args) {
    char s[64];
    snprintf(s, sizeof s, fmt, args...);
    lcd.setTextColor(color, TFT_BLACK);
    lcd.drawString(s, 8, y);
    y += 18;
  };
  line(TFT_WHITE, "received: %lu bytes, %lu commands", (unsigned long)st.rxBytes, (unsigned long)st.rxFrames);
  bool errs = st.crcErrors || st.lenErrors || sdLink.fifoOverflows;
  line(errs ? TFT_ORANGE : TFT_WHITE, "errors: crc %lu  length %lu  overflow %lu", (unsigned long)st.crcErrors,
       (unsigned long)st.lenErrors, (unsigned long)sdLink.fifoOverflows);
  int cts = sdLink.ctsLevel();
  line(cts == 0 ? TFT_GREEN : TFT_RED, "Cardputer ready (D5): %s", cts == 0 ? "yes" : "no (not running / wire?)");
  line(sdLink.flowControl() ? TFT_GREEN : TFT_RED, "flow control: %s", sdLink.flowControl() ? "on" : "OFF (see link_test)");
  line(TFT_WHITE, "image pool: %lu KB", (unsigned long)(assets.capacity() / 1024));
  lcd.setTextPadding(0);
}

// Debug log over USB, every 2 s
static void logStats() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 2000) return;
  // link wait per frame: high = the C6 waits for the Cardputer, low = the C6 is the bottleneck
  static uint16_t lastFrameNo = 0;
  uint16_t frames = frameNo - lastFrameNo;
  lastFrameNo = frameNo;
  if (frames) Serial.printf("[c6] %u frames in %lu ms, link wait %lu us/frame\n", frames, (unsigned long)(now - last),
                            (unsigned long)(st.waitUs / frames));
  st.waitUs = 0;
  last = now;
  Serial.printf("[c6] rx=%lu frames=%lu crc=%lu len=%lu line=%lu ovf=%lu bp=%lu cts=%d fc=%s baud=%lu pool=%lu/%lu heap=%lu\n",
                (unsigned long)st.rxBytes, (unsigned long)st.rxFrames, (unsigned long)st.crcErrors,
                (unsigned long)st.lenErrors, (unsigned long)sdLink.lineErrors, (unsigned long)sdLink.fifoOverflows,
                (unsigned long)sdLink.backpressure, sdLink.ctsLevel(), sdLink.flowControl() ? "on" : "OFF",
                (unsigned long)sdLink.actualBaud(), (unsigned long)assets.used(), (unsigned long)assets.capacity(),
                (unsigned long)ESP.getFreeHeap());
}

// ---- main -------------------------------------------------------------------
void setup() {
  Serial.begin(115200);  // USB CDC: debug log only

  lcd.init();
  lcd.setRotation(1);
  lcd.setBrightness(200);

  fb.setColorDepth(16);
  fb.setPsram(false);
  if (!fb.createSprite(SD_SCREEN_W, SD_SCREEN_H)) {
    lcd.fillScreen(TFT_RED);
    Serial.println("ERROR: framebuffer allocation failed");
  }
  fb.fillScreen(TFT_BLACK);
  if (!assets.begin(ASSET_POOL_BYTES, HEAP_RESERVE)) Serial.println("ERROR: asset pool allocation failed");
  renderer.begin(&lcd, &fb, &assets);

  if (!sdLink.begin(LINK_UART, PIN_LINK_TX, PIN_LINK_RX, PIN_LINK_RTS, PIN_LINK_CTS, SD_LINK_BAUD)) {
    lcd.fillScreen(TFT_RED);
    Serial.println("ERROR: UART init failed");
    while (true) delay(1000);
  }
  sendHello();
  // HELLO can only leave once our CTS input (D5) is LOW, i.e. the host is ready. If it is still stuck after 2 s
  // the host RTS wire is missing (or the host isn't running): turn flow control off so the link still works
  // for diagnosis (link_test reports it).
  if (!sdLink.txIdle(2000)) {
    Serial.printf("[c6] HELLO stuck 2 s, CTS (D5) level=%d: flow control off\n", sdLink.ctsLevel());
    sdLink.setFlowControl(false);
  }
  // Watchdog on the main loop: a hang (bug, bad input) reboots the C6; the Cardputer then sees HELLO and
  // restores the screen.
  esp_task_wdt_config_t wdt = {};
  wdt.timeout_ms = WATCHDOG_MS;
  wdt.idle_core_mask = 0;
  wdt.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
  esp_task_wdt_add(NULL);

  Serial.printf("[ScreenDriver C6] fw %04X proto v%d, link %d baud, free heap %u, asset pool %lu, cts=%d\n",
                FW_VERSION, SD_PROTO_VERSION, SD_LINK_BAUD, (unsigned)ESP.getFreeHeap(),
                (unsigned long)assets.capacity(), sdLink.ctsLevel());
}

// The Arduino USB-serial driver enables an interrupt on every IN token, i.e. every time the PC polls for log
// data. While a PC program has the port open that interrupt storm takes ~57% of the CPU (measured: framebuffer
// memset 361 -> 840 us). Logging works without it, so keep it off. Re-checked every loop in case the driver
// turns it back on (e.g. after a USB reconnect).
static inline void usbTokenIrqOff() {
  if (USB_SERIAL_JTAG.int_ena.val & (1u << 8)) USB_SERIAL_JTAG.int_ena.val &= ~(1u << 8);
}

void loop() {
  esp_task_wdt_reset();
  usbTokenIrqOff();
  uint32_t w0 = micros();
  int n = sdLink.read(rxBuf, sizeof rxBuf, 5);
  st.waitUs += micros() - w0;
  if (n > 0) {
    st.rxBytes += n;
    processBytes(rxBuf, rxBuf + n);
  }
  sdLink.pollEvents();
  drawStatus();
  logStats();
#ifdef SD_SELFTEST
  graphicsActive = true;  // status screen off: only the test touches the display
  static uint32_t lastSt = 0;
  if (millis() - lastSt > 5000) {
    lastSt = millis();
    selfTestResult = selfTest(renderer, fb, assets);
    logSelfTest(selfTestResult);
  }
#endif
}
