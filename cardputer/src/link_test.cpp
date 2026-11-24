// Milestone 2 link test, running on the Cardputer-ADV.
// Checks the UART link to the C6 coprocessor: HELLO, ping latency, throughput, RTS/CTS flow control,
// and recovery from corrupted data. Results go to the Cardputer screen and the USB serial monitor.
// Press the G0 button (BtnA) to run the tests again.
#include <M5Unified.h>
#include <stdarg.h>
#include "SdHostLink.h"

static SdHostLink sdLink;
static int passCount = 0, failCount = 0;

static void logf(uint16_t color, const char* fmt, ...) {
  char s[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s, sizeof s, fmt, ap);
  va_end(ap);
  Serial.println(s);
  M5.Display.setTextColor(color, TFT_BLACK);
  M5.Display.println(s);
}

static void result(const char* name, bool ok, const char* fmt = "", ...) {
  char d[120];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(d, sizeof d, fmt, ap);
  va_end(ap);
  (ok ? passCount : failCount)++;
  logf(ok ? TFT_GREEN : TFT_RED, "%s %s %s", ok ? "PASS" : "FAIL", name, d);
}

static bool getStatus(SdStatusInfo& out) {
  sdLink.send(SD_GET_STATUS);
  if (!sdLink.waitFor(SD_STATUS, 500)) return false;
  out = sdLink.status;
  return true;
}

// Sends a PING with a 16-byte pattern and checks the echoed PONG.
static bool ping(uint32_t timeoutMs = 300) {
  static uint8_t ctr = 0;
  uint8_t pl[16];
  for (int i = 0; i < 16; ++i) pl[i] = (uint8_t)(ctr + i * 7);
  ctr++;
  uint8_t s = sdLink.send(SD_PING, pl, sizeof pl);
  if (!sdLink.waitFor(SD_PONG, timeoutMs, s)) return false;
  return sdLink.reply().payloadLen == sizeof pl && memcmp(sdLink.reply().payload(), pl, sizeof pl) == 0;
}

// Pushes enough zero bytes to finish any partial frame the C6 parser is stuck in, then drains replies.
static void resync() {
  static const uint8_t zeros[64] = {};
  for (int i = 0; i < (SD_MAX_PAYLOAD + SD_FRAME_OVERHEAD) / 64 + 2; ++i) sdLink.port().sendRaw(zeros, sizeof zeros);
  sdLink.port().waitTxDone();
  while (sdLink.poll(30)) {}
}

static void sendNops(int count) {
  static uint8_t blob[SD_MAX_PAYLOAD];
  for (int i = 0; i < SD_MAX_PAYLOAD; ++i) blob[i] = (uint8_t)i;
  for (int i = 0; i < count; ++i) sdLink.send(SD_NOP, blob, sizeof blob);
}

// Runs when HELLO doesn't arrive. Works out which wire is at fault.
// Returns true if the link works with flow control turned off (testing then continues without it).
static bool diagnose() {
  SdUartPort& port = sdLink.port();
  logf(TFT_YELLOW, "-- diagnostics --");
  bool txDone = port.txIdle(200);
  int cts = port.ctsLevel();
  port.pollEvents();
  logf(TFT_YELLOW, "TX: %s", txDone ? "sent ok" : "STUCK (held by CTS)");
  logf(TFT_YELLOW, "CTS G6 (<-D9): %s", cts == 0 ? "LOW (C6 ready)" : "HIGH (C6 not ready/no wire)");
  logf(TFT_YELLOW, "rx from C6: %lu B, line err %lu, ovf %lu", (unsigned long)sdLink.rxBytes,
       (unsigned long)port.lineErrors, (unsigned long)port.fifoOverflows);

  // Retry with our flow control off. The C6 turns its own off if its HELLO gets stuck.
  // If our RESET was stuck in the TX buffer, it goes out now. Give the C6 time to reboot
  // and say HELLO before sending a second RESET, so it doesn't reboot again mid-test.
  uint32_t rx0 = sdLink.rxBytes;
  sdLink.helloSeen = false;
  port.setFlowControl(false);
  uint32_t t = millis();
  while (millis() - t < 2500 && !sdLink.helloSeen) sdLink.poll(20);
  bool ok = sdLink.helloSeen;
  if (!ok) {
    sdLink.send(SD_RESET);
    ok = sdLink.waitFor(SD_HELLO, 4000);
  }
  logf(TFT_YELLOW, "retry, flow ctrl OFF: %s (rx +%lu B)", ok ? "HELLO ok" : "no HELLO",
       (unsigned long)(sdLink.rxBytes - rx0));

  if (ok) {
    if (!txDone || cts != 0)
      logf(TFT_ORANGE, "=> data wires OK. G6<-D9 wire bad/missing or swapped with G4->D5");
    else
      logf(TFT_ORANGE, "=> data wires OK. Check G4->D5 (C6 CTS)");
    logf(TFT_ORANGE, "continuing WITHOUT flow control");
  } else if (sdLink.rxBytes - rx0 > 0) {
    logf(TFT_ORANGE, "=> bytes arrive but no valid frame: TX/RX swapped, no GND, or baud mismatch");
  } else {
    logf(TFT_ORANGE, "=> nothing from C6. Did its screen reset? yes: check D6->G15. no: check G13->D7");
  }
  return ok;
}

// Sends DEBUG_PINS and waits for the PINS reply.
// Retries up to 3 times. On failure the pin fields are set to 0xFF, so an old value is never reused.
static bool pinsCmd(uint8_t action) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    sdLink.pinD5 = sdLink.pinRts = sdLink.pinFc = 0xFF;
    uint8_t s = sdLink.send(SD_DEBUG_PINS, &action, 1);
    if (sdLink.waitFor(SD_PINS, 300)) return true;
    Serial.printf("pins action %u (seq %u): no reply, retry\n", action, s);
    while (sdLink.poll(20)) {}
  }
  return false;
}

// Wire toggle test for the two flow-control wires. Both sides drive their RTS pin as a plain GPIO
// and read the other side's, with flow control off. Leaves flow control on (both sides) if both wires are OK.
static bool wireTest() {
  SdUartPort& port = sdLink.port();
  port.setFlowControl(false);
  port.forceRts(0);  // G4 LOW, so the C6 can always transmit
  delay(2);
  while (sdLink.poll(20)) {}  // drain anything left over

  // G4 -> D5: we drive G4, the C6 reports D5
  int g4lo = -1, g4hi = -1, d5lo = -1, d5hi = -1;
  bool replies = true;
  port.forceRts(1);
  delay(2);
  g4hi = port.rtsLevel();
  replies &= pinsCmd(SD_PINS_READ);
  d5hi = sdLink.pinD5;
  port.forceRts(0);
  delay(2);
  g4lo = port.rtsLevel();
  replies &= pinsCmd(SD_PINS_READ);
  d5lo = sdLink.pinD5;

  // D9 -> G6: the C6 drives D9, we read G6
  int d9lo = -1, d9hi = -1, g6lo = -1, g6hi = -1;
  replies &= pinsCmd(SD_PINS_RTS_HIGH);
  d9hi = sdLink.pinRts;
  g6hi = port.ctsLevel();
  replies &= pinsCmd(SD_PINS_RTS_LOW);
  d9lo = sdLink.pinRts;
  g6lo = port.ctsLevel();

  if (!replies) {
    result("wires", false, "no PINS reply (C6 fw too old?)");
    pinsCmd(SD_PINS_RELEASE_FC_OFF);
    port.forceRts(-1);
    return false;
  }
  bool g4ok = d5hi == 1 && d5lo == 0;
  bool g6ok = g6hi == 1 && g6lo == 0;
  result("G4->D5", g4ok, "G4 drv 1/0 (pad %d/%d) -> D5 %d/%d", g4hi, g4lo, d5hi, d5lo);
  result("D9->G6", g6ok, "D9 drv 1/0 (pad %d/%d) -> G6 %d/%d", d9hi, d9lo, g6hi, g6lo);
  if (!g4ok && d5hi == 1 && d5lo == 1) logf(TFT_ORANGE, "   D5 always HIGH: G4->D5 open (pull-up) or wrong pin");
  if (!g6ok && g6hi == 1 && g6lo == 1) logf(TFT_ORANGE, "   G6 always HIGH: D9->G6 open (pull-up) or wrong pin");
  if ((!g4ok && d5hi == 0 && d5lo == 0) || (!g6ok && g6hi == 0 && g6lo == 0))
    logf(TFT_ORANGE, "   stuck LOW: short to GND?");

  bool both = g4ok && g6ok;
  // G4 is still forced LOW, so the C6's reply gets out even after it turns flow control on.
  pinsCmd(both ? SD_PINS_RELEASE_FC_ON : SD_PINS_RELEASE_FC_OFF);
  port.forceRts(-1);
  port.setFlowControl(both);
  if (!both) logf(TFT_ORANGE, "continuing WITHOUT flow control");
  return both;
}

static void runTests() {
  passCount = failCount = 0;
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(0, 0);
  logf(TFT_CYAN, "ScreenDriver link test @ %d baud", SD_LINK_BAUD);

  // 1. RESET -> HELLO (flow control back on in case a previous run's diagnostics turned it off)
  sdLink.port().setFlowControl(true);
  // The C6 prints its ROM boot log at 115200 baud on our RX line while it reboots. At link speed that's
  // garbage. Let it pass, then flush our receiver so RTS (G4) asserts again before the C6 sends HELLO.
  sdLink.helloSeen = false;
  sdLink.send(SD_RESET);
  sdLink.port().waitTxDone();
  sdLink.port().pollEvents();
  uint32_t le0 = sdLink.port().lineErrors, rx0 = sdLink.rxBytes;
  uint32_t t0 = millis();
  while (millis() - t0 < 250 && !sdLink.helloSeen) sdLink.poll(10);
  sdLink.port().pollEvents();
  logf(TFT_DARKGREY, "C6 boot noise: %lu B, %lu line errors", (unsigned long)(sdLink.rxBytes - rx0),
       (unsigned long)(sdLink.port().lineErrors - le0));
  bool ok = sdLink.helloSeen;
  if (!ok) {
    sdLink.flushInput();
    ok = sdLink.waitFor(SD_HELLO, 4000);  // the C6 waits up to 2 s for CTS before giving up on flow control
  }
  if (!ok) {
    result("hello", false, "no HELLO in 3 s");
    if (!diagnose()) {
      logf(TFT_RED, "== stopped (G0: rerun) ==");
      return;
    }
  }
  result("hello", sdLink.hello.proto == SD_PROTO_VERSION, "proto v%d fw %04X %dx%d", sdLink.hello.proto, sdLink.hello.fw,
         sdLink.hello.width, sdLink.hello.height);
  delay(300);       // let the C6 finish drawing its status screen
  while (sdLink.poll(20)) {}

  // 1b. Flow-control wires, toggled one at a time
  bool fcOn = wireTest();

  // 2. Ping latency
  {
    const int N = 100;
    uint32_t total = 0, worst = 0;
    int fails = 0;
    for (int i = 0; i < N; ++i) {
      uint32_t t = micros();
      if (!ping()) fails++;
      uint32_t d = micros() - t;
      total += d;
      if (d > worst) worst = d;
    }
    result("ping", fails == 0, "avg %lu us, max %lu us, lost %d/%d", (unsigned long)(total / N),
           (unsigned long)worst, fails, N);
  }

  // 3. Throughput: 256 x 1 KB NOP frames, then a PING as a barrier
  {
    SdStatusInfo a, b;
    getStatus(a);
    const int N = 256;
    uint32_t t = micros();
    sendNops(N);
    bool got = ping(15000);
    uint32_t us = micros() - t;
    getStatus(b);
    float bytes = N * (SD_MAX_PAYLOAD + SD_FRAME_OVERHEAD);
    float kbs = bytes / (us / 1e6f) / 1024.0f;
    float maxKbs = SD_LINK_BAUD / 10.0f / 1024.0f;
    uint32_t frames = b.rxFrames - a.rxFrames;
    bool clean = (b.crcErrors == a.crcErrors) && (b.lenErrors == a.lenErrors) && (b.fifoOverflows == a.fifoOverflows);
    result("throughput", got && clean && frames == (uint32_t)N + 2, "%.0f KB/s (%.0f%% of %.0f) frames %lu",
           kbs, 100.0f * kbs / maxKbs, maxKbs, (unsigned long)frames);
  }

  // 4. Flow control (needs both wires): the C6 stalls for 1.5 s while we push 66 KB (more than its 16 KB RX buffer)
  if (!fcOn) {
    logf(TFT_ORANGE, "SKIP flow ctrl (wires not OK)");
  } else {
    SdStatusInfo a, b;
    getStatus(a);
    uint8_t ms[2];
    sd_wr_u16(ms, 1500);
    uint32_t t = millis();
    sdLink.send(SD_DEBUG_STALL, ms, 2);
    sendNops(64);
    bool got = ping(8000);
    uint32_t el = millis() - t;
    getStatus(b);
    uint32_t frames = b.rxFrames - a.rxFrames;
    bool noLoss = b.fifoOverflows == a.fifoOverflows && b.crcErrors == a.crcErrors && frames == 64 + 3;
    bool heldBack = b.backpressure > a.backpressure;
    result("flow ctrl", got && noLoss && heldBack && el >= 1500, "%lu ms, frames %lu/67, ovf %lu, bp +%lu",
           (unsigned long)el, (unsigned long)frames, (unsigned long)(b.fifoOverflows - a.fifoOverflows),
           (unsigned long)(b.backpressure - a.backpressure));
  }

  // 5a. Bad CRC -> NACK(ERR_CRC)
  {
    uint8_t f[8 + 4] = {};
    uint8_t pl[4] = {1, 2, 3, 4};
    uint16_t crc = sd_crc16(pl, 4, sd_make_header(f, 0x77, SD_PING, 4));
    memcpy(f + 6, pl, 4);
    f[10] = (uint8_t)crc ^ 0xFF;  // corrupt
    f[11] = crc >> 8;
    uint32_t n0 = sdLink.nacks;
    sdLink.port().sendRaw(f, sizeof f);
    bool got = sdLink.waitFor(SD_NACK, 500);
    result("bad crc", got && sdLink.nacks > n0 && sdLink.lastNackErr == SD_ERR_CRC, "nack err=%d", sdLink.lastNackErr);
  }

  // 5b. Random garbage, then a ping must still work
  {
    uint8_t g[200];
    for (auto& b : g) {
      b = (uint8_t)esp_random();
      if (b == SD_SYNC0) b = 0;
    }
    sdLink.port().sendRaw(g, sizeof g);
    result("garbage", ping(500), "");
  }

  // 5c. Truncated frame (header claims 600 bytes, only 10 arrive), then resync()
  {
    uint8_t f[16] = {};
    sd_make_header(f, 0x55, SD_NOP, 598);
    sdLink.port().sendRaw(f, sizeof f);
    resync();
    result("truncated", ping(500), "recovered with resync()");
  }

  // 6. Unassigned opcode -> NACK(ERR_UNKNOWN_CMD). (Until M3 this used FILL_SCREEN, which is now implemented.)
  {
    uint8_t c[2] = {0, 0};
    uint8_t s = sdLink.send(0x7F, c, 2);
    bool got = sdLink.waitFor(SD_NACK, 500);
    result("nack path", got && sdLink.lastNackSeq == s && sdLink.lastNackErr == SD_ERR_UNKNOWN_CMD, "");
  }

  logf(failCount ? TFT_RED : TFT_GREEN, "== %d passed, %d failed == (G0: rerun)", passCount, failCount);
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextScroll(true);
  M5.Display.fillScreen(TFT_BLACK);
  if (!sdLink.begin()) {
    logf(TFT_RED, "UART init failed");
    return;
  }
  delay(1000);
  runTests();
}

void loop() {
  M5.update();
  if (M5.BtnA.wasPressed()) runTests();
  delay(10);
}
