// Example: adding the ScreenDriver external screen to an existing Cardputer app.
//
// Pretend this is your app: it has its own values to show and its own Cardputer screen. It uses the external
// 320x240 screen in two ways (see the cardputer-external-screen skill, patterns A and C):
//   - second screen (at start): extra content on the external screen, only changed values are redrawn
//   - mirror (any key): the app draws into a canvas that is shown on both screens; only changed rows are sent
// The app keeps working without the external screen and picks it up when it is plugged in later.
#include <M5Cardputer.h>
#include "RemoteDisplay.h"

RemoteDisplay ext;

// ---- second screen (pattern A) ---------------------------------------------------------------------------
// Static parts: drawn once, and again after a recovery (onRedraw).
static void drawExtLayout() {
  ext.fillScreen(TFT_BLACK);
  ext.setFont(RD_FONT_FONT2);
  ext.setTextColor(TFT_DARKGREY);
  ext.setTextDatum(0);  // top-left
  ext.drawString("Temperature", 10, 10);
  ext.drawString("Messages", 10, 80);
}

// Values: drawn only when they change. A filled text background clears the old value.
static void updateExt(float temp, int msgs) {
  static float lastTemp = NAN;
  static int lastMsgs = -1;
  if (temp == lastTemp && msgs == lastMsgs && !ext.wasReset()) {
    ext.poll();  // nothing to draw: keep the link checks (and reconnection) running
    return;
  }
  ext.startFrame();
  char s[32];
  ext.setFont(RD_FONT_FONT4);
  ext.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(s, sizeof s, "%5.1f C ", temp);
  ext.drawString(s, 10, 32);
  snprintf(s, sizeof s, "%4d   ", msgs);
  ext.drawString(s, 10, 102);
  ext.endFrame();
  lastTemp = temp;
  lastMsgs = msgs;
}

// ---- mirror (pattern C) ----------------------------------------------------------------------------------
// The app draws into this canvas instead of the display; presentFrame() shows it on both screens.
static M5Canvas canvas(&M5Cardputer.Display);
static uint32_t rowHash[135];  // per row: a hash of what the external screen shows (0 = not sent yet)
static uint16_t rowBuf[240 * 2];

static void setupMirror() {
  canvas.setColorDepth(16);
  canvas.createSprite(240, 135);
  memset(rowHash, 0, sizeof rowHash);
  ext.onRedraw([] {  // after a recovery: clear and send every row again
    ext.fillScreen(TFT_BLACK);
    memset(rowHash, 0, sizeof rowHash);
  });
}

static uint32_t hashRow(const uint16_t* p) {
  uint32_t h = 2166136261u;
  for (int x = 0; x < 240; ++x) h = (h ^ p[x]) * 16777619u;
  return h | 1;  // never 0
}

static void presentFrame() {
  canvas.pushSprite(0, 0);  // the Cardputer screen, as before
  ext.startFrame();
  const uint16_t* fb = (const uint16_t*)canvas.getBuffer();
  for (int y = 0; y < 135;) {
    uint32_t h = hashRow(fb + y * 240);
    if (h == rowHash[y]) {
      ++y;
      continue;
    }
    // up to 2 changed rows per command (one command holds 1015 bytes of pixels)
    int n = 1;
    rowHash[y] = h;
    if (y + 1 < 135) {
      uint32_t h2 = hashRow(fb + (y + 1) * 240);
      if (h2 != rowHash[y + 1]) {
        rowHash[y + 1] = h2;
        n = 2;
      }
    }
    // M5Canvas keeps 16-bit pixels byte-swapped (panel order); pushImage wants RGB565 values
    for (int i = 0; i < n * 240; ++i) rowBuf[i] = __builtin_bswap16(fb[y * 240 + i]);
    ext.pushImage(40, 52 + y, 240, n, rowBuf);  // centered on 320x240
    y += n;
  }
  ext.endFrame();
}

// ---- the app -----------------------------------------------------------------------------------------------
static bool mirror = false;

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);  // first the Cardputer, then the external screen
  Serial.begin(115200);

  // 1.5 s timeout and no retry loop: without the screen the app just runs, and the library connects by itself
  // when the C6 appears (drawing calls are dropped meanwhile).
  if (!ext.begin(1, 1500)) Serial.printf("external screen: %s\n", ext.lastError());
  ext.onRedraw(drawExtLayout);
  ext.startFrame();
  drawExtLayout();
  ext.endFrame();

  M5Cardputer.Display.setFont(&fonts::Font2);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.drawString("Second screen mode", 8, 8);
  M5Cardputer.Display.drawString("any key: mirror mode", 8, 30);
}

void loop() {
  M5Cardputer.update();
  if (!mirror && M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    mirror = true;
    setupMirror();
  }
  if (!mirror) {
    // the app's own values (here: made up)
    updateExt(20.0f + (millis() / 1000) % 10, (int)(millis() / 3000));
  } else {
    canvas.fillScreen(TFT_NAVY);
    canvas.setTextColor(TFT_WHITE);
    canvas.setFont(&fonts::Font4);
    canvas.setCursor(10, 50);
    canvas.printf("mirror %lu", (unsigned long)(millis() / 100));
    presentFrame();
  }
  delay(10);
}
