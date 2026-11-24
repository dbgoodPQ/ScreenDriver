// Milestone 3 UI demo: only the widgets that change are redrawn each frame, so the C6 flushes small areas.
// Keys: ; up, . down, Enter toggles the selected item.
#include "demo_common.h"

static RemoteDisplay ext;

static const int ITEMS = 8;
static const char* const itemNames[ITEMS] = {"Wi-Fi", "Bluetooth", "Sound", "Vibration",
                                             "Auto-sleep", "Dark mode", "Logging", "Updates"};
static bool itemOn[ITEMS] = {true, false, true, false, true, true, false, true};
static int sel = 0;

static float bars[6] = {40, 60, 30, 80, 50, 70};
static uint32_t lastClockSec = UINT32_MAX, lastBarsMs = 0, lastFooterMs = 0;
static int progress = 0;

// ---- widgets ----------------------------------------------------------------------
static void drawTitle() {
  ext.fillRect(0, 0, 320, 24, TFT_NAVY);
  ext.setFont(RD_FONT_FONT2);
  ext.setTextSize(1);
  ext.setTextDatum(0);
  ext.setTextColor(TFT_WHITE);
  ext.drawString("ScreenDriver UI demo", 6, 4);
}

static void drawClock() {
  uint32_t s = millis() / 1000;
  char t[16];
  snprintf(t, sizeof t, "%02lu:%02lu", (unsigned long)(s / 60) % 100, (unsigned long)(s % 60));
  ext.setFont(RD_FONT_FONT2);
  ext.setTextDatum(2);
  ext.setTextColor(TFT_YELLOW, TFT_NAVY);
  ext.drawString(t, 314, 4);
  ext.setTextDatum(0);
}

static void drawItem(int i) {
  int y = 30 + i * 22;
  bool s = i == sel;
  ext.fillRect(0, y, 152, 21, s ? TFT_DARKCYAN : TFT_BLACK);
  ext.setFont(RD_FONT_FONT2);
  ext.setTextColor(TFT_WHITE);
  ext.drawString(itemNames[i], 26, y + 3);
  ext.drawRect(6, y + 5, 12, 12, TFT_LIGHTGREY);
  if (itemOn[i]) ext.fillRect(9, y + 8, 6, 6, TFT_GREEN);
}

static void drawProgress() {
  const int x = 164, y = 34, w = 148, h = 18;
  int fill = (w - 4) * progress / 100;
  ext.drawRoundRect(x, y, w, h, 4, TFT_LIGHTGREY);
  ext.fillRect(x + 2, y + 2, fill, h - 4, TFT_GREEN);
  ext.fillRect(x + 2 + fill, y + 2, w - 4 - fill, h - 4, TFT_BLACK);
}

static void drawBars() {
  const int x = 164, y = 64, w = 148, h = 110;
  ext.fillRect(x, y, w, h, TFT_BLACK);
  ext.drawFastHLine(x, y + h - 1, w, TFT_DARKGREY);
  static const uint16_t cs[6] = {TFT_RED, TFT_ORANGE, TFT_YELLOW, TFT_GREEN, TFT_CYAN, TFT_MAGENTA};
  for (int i = 0; i < 6; ++i) {
    int bh = (int)(bars[i] * (h - 4) / 100);
    ext.fillRect(x + 4 + i * 24, y + h - 1 - bh, 18, bh, cs[i]);
  }
}

static void drawFooter() {
  const RemoteDisplay::FrameStats& f = ext.lastFrame();
  char t[80];
  snprintf(t, sizeof t, "%.0f fps | C6 render %.1f ms, flush %.1f ms | idle %.1f ms", f.fps, f.renderUs / 1000.0f,
           f.flushUs / 1000.0f, f.waitUs / 1000.0f);
  ext.fillRect(0, 222, 320, 18, TFT_DARKGREY);
  ext.setFont(RD_FONT_GLCD);
  ext.setTextColor(TFT_WHITE);
  ext.drawString(t, 4, 227);
}

// Everything that isn't redrawn every frame. Also the redraw callback: the library calls it (inside
// startFrame) after recovering from a C6 reboot, a lost link or a corrupted command.
static void drawStatic() {
  ext.fillScreen(TFT_BLACK);
  drawTitle();
  for (int i = 0; i < ITEMS; ++i) drawItem(i);
  ext.setFont(RD_FONT_GLCD);
  ext.setTextColor(TFT_DARKGREY);
  ext.drawString("; up  . down  Enter toggle", 4, 208);
  drawBars();
  lastClockSec = UINT32_MAX;
}

static void drawAll() {
  ext.startFrame();
  drawStatic();
  ext.endFrame(true);
}

void setup() {
  demoSetup("ScreenDriver UI demo");
  connectExternal(ext);
  ext.onRedraw(drawStatic);
  localLog(TFT_DARKGREY, "; up  . down  Enter toggle");
  drawAll();
}

void loop() {
  uint32_t now = millis();
  Keys k = readKeys();
  ext.startFrame();

  if (k.up || k.down) {
    int old = sel;
    sel = (sel + (k.down ? 1 : ITEMS - 1)) % ITEMS;
    drawItem(old);
    drawItem(sel);
  }
  if (k.enter) {
    itemOn[sel] = !itemOn[sel];
    drawItem(sel);
  }
  if (now / 1000 != lastClockSec) {
    lastClockSec = now / 1000;
    drawClock();
  }
  progress = (progress + 1) % 101;
  drawProgress();
  if (now - lastBarsMs >= 200) {
    lastBarsMs = now;
    for (float& b : bars) b = constrain(b + random(-12, 13), 5.0f, 100.0f);
    drawBars();
  }
  if (now - lastFooterMs >= 500) {
    lastFooterMs = now;
    drawFooter();
  }

  ext.endFrame();
  static bool wasConnected = true;
  if (ext.connected() != wasConnected) {
    wasConnected = ext.connected();
    localLog(wasConnected ? TFT_GREEN : TFT_RED, wasConnected ? "link back (%lu ms)" : "link down",
             (unsigned long)ext.health().lastRecoveryMs);
  }
}
