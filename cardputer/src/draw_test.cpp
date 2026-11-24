// Milestone 3 draw test: every primitive in a labelled box on the external screen, plus automatic checks
// (FRAME_DONE, NACK on a bad length and a bad font, link counters). Any key reruns.
#include "demo_common.h"

static RemoteDisplay ext;
static uint16_t testSprite[16 * 16];  // 16x16 test image for the asset checks
static int passCount, failCount;

static void result(const char* name, bool ok, const char* fmt = "", ...) {
  char d[100];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(d, sizeof d, fmt, ap);
  va_end(ap);
  (ok ? passCount : failCount)++;
  localLog(ok ? TFT_GREEN : TFT_RED, "%s %s %s", ok ? "PASS" : "FAIL", name, d);
}

// 4 x 3 grid of 80x80 cells
static void cell(int i, int& x, int& y) {
  x = (i % 4) * 80;
  y = (i / 4) * 80;
}

static void label(int i, const char* text) {
  int x, y;
  cell(i, x, y);
  ext.drawRect(x, y, 80, 80, TFT_DARKGREY);
  ext.setFont(RD_FONT_GLCD);
  ext.setTextSize(1);
  ext.setTextColor(TFT_WHITE);
  ext.setTextDatum(9);  // bottom-center
  ext.drawString(text, x + 40, y + 77);
}

static void drawAll() {
  int x, y;
  ext.startFrame();
  ext.fillScreen(TFT_BLACK);

  cell(0, x, y); ext.fillRect(x + 10, y + 10, 60, 50, TFT_BLUE);                 label(0, "fillRect");
  cell(1, x, y); ext.drawRect(x + 10, y + 10, 60, 50, TFT_YELLOW);               label(1, "drawRect");
  cell(2, x, y);
  for (int a = 0; a < 8; ++a) {
    float r = a * PI / 4;
    ext.drawLine(x + 40, y + 35, x + 40 + (int)(28 * cosf(r)), y + 35 + (int)(28 * sinf(r)), TFT_GREEN);
  }
  label(2, "drawLine");
  cell(3, x, y);
  for (int i = 0; i < 6; ++i) {
    ext.drawFastHLine(x + 10, y + 10 + i * 10, 60, TFT_CYAN);
    ext.drawFastVLine(x + 10 + i * 12, y + 10, 50, TFT_MAGENTA);
  }
  label(3, "H/V lines");
  cell(4, x, y); ext.drawCircle(x + 40, y + 35, 26, TFT_ORANGE);                 label(4, "drawCircle");
  cell(5, x, y); ext.fillCircle(x + 40, y + 35, 26, TFT_RED);                    label(5, "fillCircle");
  cell(6, x, y); ext.drawRoundRect(x + 10, y + 10, 60, 50, 12, TFT_WHITE);       label(6, "drawRoundRect");
  cell(7, x, y); ext.fillRoundRect(x + 10, y + 10, 60, 50, 12, TFT_PURPLE);      label(7, "fillRoundRect");
  cell(8, x, y); ext.drawTriangle(x + 40, y + 8, x + 10, y + 62, x + 70, y + 62, TFT_GREENYELLOW);
  label(8, "drawTriangle");
  cell(9, x, y); ext.fillTriangle(x + 40, y + 8, x + 10, y + 62, x + 70, y + 62, TFT_PINK);
  label(9, "fillTriangle");
  cell(10, x, y);
  for (int py = 0; py < 50; py += 4)
    for (int px = 0; px < 60; px += 4) ext.drawPixel(x + 10 + px, y + 10 + py, (px + py) % 8 ? TFT_WHITE : TFT_RED);
  label(10, "drawPixel");
  // Clip: a circle bigger than its box, cut at the inner edges of the box
  cell(11, x, y);
  ext.setClipRect(x + 10, y + 10, 60, 50);
  ext.fillCircle(x + 40, y + 35, 45, TFT_SKYBLUE);
  ext.clearClipRect();
  label(11, "clip");
  ext.endFrame();
}

static void runChecks() {
  passCount = failCount = 0;
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.setCursor(0, 0);
  localLog(TFT_CYAN, "ScreenDriver draw test");

  SdHostLink& link = ext.link();
  uint32_t nacks0 = ext.nackCount();
  uint32_t done0 = link.framesDone;
  uint32_t t0 = millis();
  drawAll();
  uint32_t start = millis();
  while (link.framesDone <= done0 && millis() - start < 1000) link.poll(5);
  result("frame", link.framesDone > done0, "sent in %lu ms, C6 render %lu us, flush %lu us", (unsigned long)(start - t0),
         (unsigned long)link.lastRenderUs, (unsigned long)link.lastFlushUs);
  result("no NACKs", ext.nackCount() == nacks0, "");

  // A FILL_RECT with a too-short payload must be refused with BAD_LEN
  uint8_t shortRect[4] = {0, 0, 0, 0};
  link.send(SD_FILL_RECT, shortRect, sizeof shortRect);
  bool got = link.waitFor(SD_NACK, 500);
  result("bad length", got && ext.lastNackError() == SD_ERR_BAD_LEN, "nack err=%d", ext.lastNackError());

  // An unknown font must be refused with BAD_ARG
  ext.setFont(9);
  ext.drawString("x", -100, -100);
  got = link.waitFor(SD_NACK, 500);
  result("bad font", got && ext.lastNackError() == SD_ERR_BAD_ARG, "nack err=%d", ext.lastNackError());
  ext.setFont(RD_FONT_GLCD);

  SdStatusInfo st;
  got = ext.getStatus(st);
  result("link clean", got && st.fifoOverflows == 0 && st.crcErrors == 0, "ovf %lu crc %lu bp %lu",
         (unsigned long)st.fifoOverflows, (unsigned long)st.crcErrors, (unsigned long)st.backpressure);

  // ---- assets (milestone 4)
  for (int i = 0; i < 16 * 16; ++i) testSprite[i] = (i / 16 + i % 16) & 1 ? TFT_YELLOW : TFT_BLUE;
  int32_t pool0 = ext.assetPoolFree();
  bool up = ext.uploadImage(60, testSprite, 16, 16);
  int32_t pool1 = ext.assetPoolFree();
  result("upload", up && pool1 == pool0 - 512, "%s pool %ld->%ld", up ? "" : ext.lastError(), (long)pool0, (long)pool1);
  // An image bigger than the pool must be refused before any data is sent
  bool big = ext.uploadImage(61, testSprite, 320, 320);
  result("pool full", !big && ext.lastNackError() == SD_ERR_NO_MEM, "nack err=%d", ext.lastNackError());
  // Drawing an asset the C6 doesn't have must be refused. drawSprite() skips ids it never uploaded, so send
  // the command directly. The library reads such a refusal as "the C6 lost its assets" and recovers by itself
  // (reset + re-upload of asset 60): check that as well.
  uint32_t rec0 = ext.health().recoveries;
  uint8_t blit[8] = {62, 0, 0, 0, 0, 0, 0, 0};
  link.send(SD_BLIT, blit, sizeof blit);
  got = link.waitFor(SD_NACK, 500);
  result("bad asset", got && ext.lastNackError() == SD_ERR_BAD_ASSET, "nack err=%d", ext.lastNackError());
  ext.poll();
  ext.wasReset();  // consume the flag: this recovery was ours
  result("auto recover", ext.health().recoveries == rec0 + 1 && ext.hasAsset(60), "%lu ms",
         (unsigned long)ext.health().lastRecoveryMs);
  ext.freeAsset(60);
  int32_t pool2 = ext.assetPoolFree();
  result("free", pool2 == pool0, "pool %ld", (long)pool2);
  drawAll();  // the recovery cleared the external screen

  localLog(failCount ? TFT_RED : TFT_GREEN, "== %d passed, %d failed ==", passCount, failCount);
  localLog(TFT_YELLOW, "Check the external screen: 12 labelled");
  localLog(TFT_YELLOW, "boxes; 'clip' = circle cut at box edge.");
  localLog(TFT_DARKGREY, "any key: rerun");
}

void setup() {
  demoSetup("ScreenDriver draw test");
  connectExternal(ext);
  runChecks();
}

void loop() {
  Keys k = readKeys();
  if (k.any) runChecks();
  if (ext.wasReset()) {  // automatic recovery after a C6 reboot or link loss
    localLog(TFT_ORANGE, "link recovered, rerunning");
    runChecks();
  }
  delay(10);
}
