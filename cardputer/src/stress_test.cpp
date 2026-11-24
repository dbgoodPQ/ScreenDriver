// Milestone 5 stress test: the Cardputer app must survive anything that happens to the link or the C6, and the
// external screen must recover by itself. Every test ends with a PASS/FAIL verdict; the menu collects them.
//   1 fuzz         5 min of random commands with silly values: the C6 never reboots or stalls
//   2 unplug       pull the link connector and plug it back, twice: restored within 2 s each time
//   3 reboot       the C6 is reset 3 times without warning: back within 2 s each time
//   4 corruption   1% of commands get a bad CRC for 30 s: every loss detected, the screen repaired
//   5 watchdog     the C6 is told to hang: its watchdog reboots it and the screen comes back
//   6 long run     animation until you press 0: heap stable, no overflows, no recoveries
//   A              runs 1, 3, 4, 5 one after the other (no hands needed)
#include "demo_common.h"

static RemoteDisplay ext;
static M5Canvas canvas(&M5Cardputer.Display);  // drawn off-screen, then pushed: no flicker
static uint16_t ball[16 * 16];

// ---- results table ---------------------------------------------------------------
enum Res : uint8_t { R_NONE, R_PASS, R_FAIL, R_ABORT };
struct Result {
  Res res = R_NONE;
  char detail[28] = "";
};
static Result results[7];
static const char* const names[7] = {"", "fuzz 5 min", "unplug x2", "reboot x3", "corrupt 30s", "watchdog",
                                     "long run"};

// ---- state -----------------------------------------------------------------------
enum Mode { MENU, RUNNING, VERDICT };
static Mode mode = MENU;
static int test = 0;
static uint32_t testStart = 0, frames = 0, verdictMs = 0;
static RemoteDisplay::Health h0;
static bool autoRun = false;
static int autoIdx = 0;
static const int AUTO_TESTS[] = {1, 3, 4, 5};

// per-test
static uint32_t corrupted0 = 0;
static uint32_t resetSentMs = 0, recAtReset = 0, nextResetMs = 0;
static int resetsDone = 0;
static uint32_t worstMs = 0;
static uint32_t downSinceMs = 0;
static int32_t heapFirst = -1, heapMin = INT32_MAX, heapMax = 0, heapLast = 0;
static uint32_t lastHeapMs = 0;
static SdStatusInfo st0, stLast;

static uint32_t loopsThisSec = 0, loopRate = 0, lastRateMs = 0;

// ---- fast random numbers ---------------------------------------------------------
static uint32_t rngState = 0xC0FFEE;
static inline uint32_t rnd32() {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 17;
  rngState ^= rngState << 5;
  return rngState;
}
static inline int rnd(int lo, int hi) { return lo + (int)(rnd32() % (uint32_t)(hi - lo)); }
static int16_t rv() {  // mostly on screen, sometimes far off, sometimes any int16
  uint32_t r = rnd32() % 100;
  if (r < 70) return rnd(-40, 360);
  if (r < 90) return rnd(-2000, 2000);
  return (int16_t)rnd32();
}
static uint16_t rcolor() { return (uint16_t)rnd32(); }

// ---- external screen scenes --------------------------------------------------------
struct Ball {
  float x, y, vx, vy;
};
static Ball balls[30];

static void initScene() {
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) {
      int dx = x - 8, dy = y - 8;
      ball[y * 16 + x] = dx * dx + dy * dy < 56 ? RemoteDisplay::color565(255, 120 + x * 8, 40 + y * 12) : RD_KEY_DEFAULT;
    }
  for (auto& b : balls) {
    b.x = rnd(0, 304);
    b.y = rnd(20, 224);
    b.vx = rnd(1, 4) * (rnd(0, 2) ? 1 : -1);
    b.vy = rnd(1, 4) * (rnd(0, 2) ? 1 : -1);
  }
}

static void sceneFrame(const char* title) {
  ext.startFrame();
  ext.fillScreen(TFT_NAVY);
  for (auto& b : balls) {
    b.x += b.vx;
    b.y += b.vy;
    if (b.x < 0 || b.x > 304) b.vx = -b.vx;
    if (b.y < 20 || b.y > 224) b.vy = -b.vy;
    ext.drawSprite(0, 0, (int16_t)b.x, (int16_t)b.y);
  }
  ext.setFont(RD_FONT_FONT2);
  ext.setTextSize(1);
  ext.setTextDatum(0);
  ext.setTextColor(TFT_WHITE, TFT_NAVY);
  ext.drawString(title, 4, 2);
  ext.endFrame();
  frames++;
}

static void fuzzFrame() {
  ext.startFrame();
  for (int i = 0; i < 40; ++i) {
    switch (rnd(0, 24)) {
      case 0: if (rnd(0, 20) == 0) ext.fillScreen(rcolor()); break;
      case 1: ext.fillRect(rv(), rv(), rv(), rv(), rcolor()); break;
      case 2: ext.drawRect(rv(), rv(), rv(), rv(), rcolor()); break;
      case 3: ext.drawLine(rv(), rv(), rv(), rv(), rcolor()); break;
      case 4: ext.drawFastHLine(rv(), rv(), rv(), rcolor()); break;
      case 5: ext.drawFastVLine(rv(), rv(), rv(), rcolor()); break;
      case 6: ext.drawCircle(rv(), rv(), rv(), rcolor()); break;
      case 7: ext.fillCircle(rv(), rv(), rv(), rcolor()); break;
      case 8: ext.drawRoundRect(rv(), rv(), rv(), rv(), rv(), rcolor()); break;
      case 9: ext.fillRoundRect(rv(), rv(), rv(), rv(), rv(), rcolor()); break;
      case 10: ext.drawTriangle(rv(), rv(), rv(), rv(), rv(), rv(), rcolor()); break;
      case 11: ext.fillTriangle(rv(), rv(), rv(), rv(), rv(), rv(), rcolor()); break;
      case 12: for (int k = rnd(1, 300); k > 0; --k) ext.drawPixel(rv(), rv(), rcolor()); break;
      case 13: ext.setClipRect(rv(), rv(), rv(), rv()); break;
      case 14: ext.clearClipRect(); break;
      case 15:
        ext.setFont(rnd(0, 8));  // 6 and 7 don't exist: the C6 refuses them (BAD_ARG)
        ext.setTextSize(rnd(0, 10) == 0 ? rnd(1, 256) : rnd(1, 4));
        ext.setTextDatum(rnd(0, 32));
        if (rnd(0, 2)) ext.setTextColor(rcolor());
        else ext.setTextColor(rcolor(), rcolor());
        break;
      case 16:
      case 17: {
        char s[65];
        int n = rnd(0, 64);
        for (int k = 0; k < n; ++k) s[k] = (char)rnd(1, 256);
        s[n] = 0;
        if (rnd(0, 2)) ext.drawString(s, rv(), rv());
        else {
          ext.setCursor(rv(), rv());
          ext.print(s);
        }
        break;
      }
      case 18: ext.drawSprite(0, rnd(0, 3), rv(), rv(), rnd(0, 8)); break;  // frames 1, 2 don't exist
      case 19: ext.drawSpriteEx(0, 0, rv(), rv(), rnd(-1000, 1000), rnd(-2000, 2000) / 100.0f, rnd(-2000, 2000) / 100.0f, rnd(0, 2)); break;
      case 20: ext.pushImage(rv(), rv(), 16, rnd(1, 17), ball); break;
      case 21: ext.copyRect(rv(), rv(), rv(), rv(), rv(), rv()); break;
      case 22: ext.setBrightness(rnd(60, 256)); break;
      default: ext.fillRect(rnd(0, 320), rnd(0, 240), rnd(1, 40), rnd(1, 40), rcolor()); break;
    }
  }
  ext.clearClipRect();
  ext.endFrame();
  frames++;
}

// corruption: incremental scene, repaired through the redraw callback
static int boxX = 0;
static void corruptionStatic() {
  ext.fillScreen(TFT_DARKGREEN);
  ext.setFont(RD_FONT_FONT2);
  ext.setTextSize(1);
  ext.setTextDatum(0);
  ext.setTextColor(TFT_WHITE);
  ext.drawString("4 CORRUPTION: no yellow trails may stay", 6, 4);
  ext.fillRect(boxX, 110, 24, 24, TFT_YELLOW);
}
static void corruptionFrame() {
  ext.startFrame();
  int old = boxX;
  boxX = (boxX + 3) % 296;
  ext.fillRect(old, 110, 24, 24, TFT_DARKGREEN);  // if this is lost, a trail stays until the redraw fixes it
  ext.fillRect(boxX, 110, 24, 24, TFT_YELLOW);
  ext.endFrame();
  frames++;
}

// verdict on the external screen too
static void verdictExternal() {
  const Result& r = results[test];
  bool ok = r.res == R_PASS;
  uint16_t bg = ok ? TFT_DARKGREEN : r.res == R_ABORT ? TFT_DARKGREY : TFT_MAROON;
  ext.fillScreen(bg);
  ext.setTextDatum(4 + 1);  // middle-center
  ext.setTextColor(TFT_WHITE);
  ext.setFont(RD_FONT_FONT4);
  ext.setTextSize(1);
  char t[40];
  snprintf(t, sizeof t, "%d %s", test, names[test]);
  ext.drawString(t, 160, 60);
  ext.setTextSize(2);
  ext.drawString(ok ? "PASS" : r.res == R_ABORT ? "ABORTED" : "FAIL", 160, 120);
  ext.setTextSize(1);
  ext.drawString(r.detail, 160, 185);
  ext.setTextDatum(0);
}

// ---- Cardputer screen -------------------------------------------------------------
static int row = 0;
static void line(uint16_t color, const char* fmt, ...) {
  char s[48];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s, sizeof s, fmt, ap);
  va_end(ap);
  canvas.setTextColor(color);
  canvas.drawString(s, 2, 2 + row * 9);
  row++;
}
static void header(uint16_t headerBg, const char* title) {
  canvas.fillScreen(TFT_BLACK);
  canvas.fillRect(0, 0, 240, 12, headerBg);
  canvas.setFont(&fonts::Font0);
  canvas.setTextSize(1);
  canvas.setTextColor(TFT_WHITE);
  canvas.drawString(title, 3, 2);
  canvas.setTextColor(ext.connected() ? TFT_GREEN : TFT_RED);
  canvas.drawRightString(ext.connected() ? "link UP" : "link DOWN", 237, 2);
  row = 2;
}

static void drawMenu() {
  header(TFT_NAVY, "SCREENDRIVER STRESS TEST");
  line(TFT_DARKGREY, "  test        result");
  for (int t = 1; t <= 6; ++t) {
    const Result& r = results[t];
    uint16_t c = r.res == R_PASS ? TFT_GREEN : r.res == R_FAIL ? TFT_RED : TFT_LIGHTGREY;
    const char* rs = r.res == R_PASS ? "PASS" : r.res == R_FAIL ? "FAIL" : r.res == R_ABORT ? "abrt" : "--";
    line(c, "%d %-11s %-4s %.20s", t, names[t], rs, r.detail);
  }
  line(TFT_CYAN, "A run 1,3,4,5 in a row (~7 min)");
  row++;
  line(TFT_DARKGREY, "press a key to start a test");
}

static uint32_t remaining(uint32_t totalMs) {
  uint32_t el = millis() - testStart;
  return el >= totalMs ? 0 : (totalMs - el + 999) / 1000;
}

static void drawRunning() {
  char title[40];
  snprintf(title, sizeof title, "%d %s%s", test, names[test], autoRun ? "  (auto)" : "");
  header(TFT_DARKCYAN, title);
  const RemoteDisplay::Health& hl = ext.health();
  switch (test) {
    case 1:
      line(TFT_YELLOW, "DO: nothing. Random commands are");
      line(TFT_YELLOW, "    sent; the C6 must stay up.");
      row++;
      line(TFT_WHITE, "time left     %lu s", (unsigned long)remaining(300000));
      line(TFT_WHITE, "frames sent   %lu", (unsigned long)frames);
      line(TFT_WHITE, "C6 refusals   %lu (expected)", (unsigned long)ext.nackCount());
      line(hl.reboots > h0.reboots ? TFT_RED : TFT_WHITE, "C6 reboots    %lu", (unsigned long)(hl.reboots - h0.reboots));
      line(hl.linkDowns > h0.linkDowns ? TFT_RED : TFT_WHITE, "link losses   %lu", (unsigned long)(hl.linkDowns - h0.linkDowns));
      break;
    case 2: {
      int cycles = hl.recoveries - h0.recoveries;
      line(TFT_YELLOW, "DO: pull the link connector off the");
      line(TFT_YELLOW, "    Cardputer, wait 3 s, plug it back.");
      line(TFT_YELLOW, "    Do it twice.");
      row++;
      line(TFT_WHITE, "cycles done   %d / 2", cycles);
      if (!ext.connected() && downSinceMs)
        line(TFT_RED, "link DOWN for %.1f s", (millis() - downSinceMs) / 1000.0f);
      else
        line(TFT_GREEN, "link up");
      line(TFT_WHITE, "app loop      %lu /s (must not stop)", (unsigned long)loopRate);
      if (cycles) line(TFT_WHITE, "last restore  %lu ms", (unsigned long)hl.lastRecoveryMs);
      break;
    }
    case 3:
      line(TFT_YELLOW, "DO: nothing. The C6 is reset 3 times");
      line(TFT_YELLOW, "    without warning.");
      row++;
      line(TFT_WHITE, "resets done   %d / 3", resetsDone);
      if (resetSentMs) line(TFT_ORANGE, "waiting...    %.1f s", (millis() - resetSentMs) / 1000.0f);
      if (resetsDone) line(TFT_WHITE, "slowest back  %lu ms (max 2000)", (unsigned long)worstMs);
      break;
    case 4:
      line(TFT_YELLOW, "DO: watch the external screen: the");
      line(TFT_YELLOW, "    yellow box must leave no trails.");
      row++;
      line(TFT_WHITE, "time left     %lu s", (unsigned long)remaining(30000));
      line(TFT_WHITE, "corrupted     %lu", (unsigned long)(ext.link().corrupted - corrupted0));
      line(TFT_WHITE, "detected      %lu", (unsigned long)(hl.crcNacks - h0.crcNacks));
      line(TFT_WHITE, "repairs       %lu", (unsigned long)(hl.redraws - h0.redraws));
      break;
    case 5:
      line(TFT_YELLOW, "DO: nothing. The C6 was told to hang;");
      line(TFT_YELLOW, "    its watchdog must reboot it.");
      row++;
      line(TFT_ORANGE, "waiting       %.1f s (max 10)", (millis() - resetSentMs) / 1000.0f);
      break;
    case 6: {
      uint32_t s = (millis() - testStart) / 1000;
      line(TFT_YELLOW, "DO: let it run (30-60 min is good),");
      line(TFT_YELLOW, "    then press 0 to get the verdict.");
      row++;
      line(TFT_WHITE, "running       %lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
      if (heapFirst >= 0)
        line(TFT_WHITE, "C6 heap       %ld (min %ld)", (long)heapLast, (long)heapMin);
      line(TFT_WHITE, "overflows     %lu", (unsigned long)(stLast.fifoOverflows - st0.fifoOverflows));
      line(TFT_WHITE, "recoveries    %lu", (unsigned long)(hl.recoveries - h0.recoveries));
      break;
    }
  }
  canvas.setTextColor(TFT_DARKGREY);
  canvas.drawString(test == 6 ? "0 = stop and judge" : "0 = abort", 3, 124);
}

static void drawVerdict() {
  const Result& r = results[test];
  bool ok = r.res == R_PASS;
  uint16_t bg = ok ? TFT_DARKGREEN : r.res == R_ABORT ? TFT_DARKGREY : TFT_MAROON;
  canvas.fillScreen(bg);
  canvas.setTextColor(TFT_WHITE);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::Font2);
  char t[40];
  snprintf(t, sizeof t, "%d %s", test, names[test]);
  canvas.drawString(t, 120, 22);
  canvas.setFont(&fonts::Font4);
  canvas.drawString(ok ? "PASS" : r.res == R_ABORT ? "ABORTED" : "FAIL", 120, 58);
  canvas.setFont(&fonts::Font2);
  canvas.drawString(r.detail, 120, 90);
  canvas.setFont(&fonts::Font0);
  if (test == 4) canvas.drawString("also check: no yellow trails left", 120, 108);
  canvas.drawString(autoRun ? "next test in a moment..." : "any key: back to the menu", 120, 124);
  canvas.setTextDatum(top_left);
}

static void refreshScreen(bool force = false) {
  static uint32_t last = 0;
  if (!force && millis() - last < 250) return;
  last = millis();
  if (mode == MENU) drawMenu();
  else if (mode == RUNNING) drawRunning();
  else drawVerdict();
  canvas.pushSprite(0, 0);
}

// ---- test control ------------------------------------------------------------------
static void finish(Res res, const char* fmt, ...) {
  Result& r = results[test];
  r.res = res;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(r.detail, sizeof r.detail, fmt, ap);
  va_end(ap);
  ext.link().corruptPerMille = 0;
  ext.setBrightness(200);  // the fuzz test may have dimmed it
  Serial.printf("[stress] test %d %s: %s\n", test, res == R_PASS ? "PASS" : res == R_FAIL ? "FAIL" : "ABORT", r.detail);
  mode = VERDICT;
  verdictMs = millis();
  ext.onRedraw(verdictExternal);
  ext.startFrame();
  verdictExternal();
  ext.endFrame(true);
  refreshScreen(true);
}

static void startTest(int t) {
  test = t;
  mode = RUNNING;
  testStart = millis();
  frames = 0;
  h0 = ext.health();
  ext.onRedraw(nullptr);
  switch (t) {
    case 3:
      resetsDone = 0;
      worstMs = 0;
      resetSentMs = 0;
      nextResetMs = millis() + 1000;
      break;
    case 4:
      corrupted0 = ext.link().corrupted;
      ext.onRedraw(corruptionStatic);
      ext.startFrame();
      corruptionStatic();
      ext.endFrame(true);
      ext.link().corruptPerMille = 10;
      break;
    case 5:
      if (ext.link().hello.fw < 0x0500) {
        finish(R_FAIL, "needs C6 fw 0500");
        return;
      }
      resetSentMs = millis();
      recAtReset = h0.recoveries;
      ext.link().send(SD_DEBUG_HANG);
      ext.link().flushTx();
      break;
    case 6:
      heapFirst = -1;
      heapMin = INT32_MAX;
      heapMax = 0;
      lastHeapMs = 0;
      ext.getStatus(st0);
      stLast = st0;
      break;
  }
  refreshScreen(true);
}

static void runTest() {
  const RemoteDisplay::Health& hl = ext.health();
  switch (test) {
    case 1:
      fuzzFrame();
      if (millis() - testStart >= 300000) {
        SdStatusInfo st;
        bool alive = ext.getStatus(st);
        ext.setBrightness(200);
        uint32_t rb = hl.reboots - h0.reboots, dn = hl.linkDowns - h0.linkDowns;
        if (alive && !rb && !dn) finish(R_PASS, "%lu frames, 0 reboots", (unsigned long)frames);
        else finish(R_FAIL, "%lu reboots, %lu losses", (unsigned long)rb, (unsigned long)dn);
      }
      break;
    case 2: {
      sceneFrame("2 UNPLUG: pull the link connector, plug it back");
      int cycles = hl.recoveries - h0.recoveries;
      static int seen = 0;
      if (cycles == 0) seen = 0;
      if (cycles > seen) {
        seen = cycles;
        worstMs = cycles == 1 ? hl.lastRecoveryMs : max(worstMs, hl.lastRecoveryMs);
      }
      if (cycles >= 2) {
        if (worstMs <= 2000) finish(R_PASS, "restored in %lu ms", (unsigned long)worstMs);
        else finish(R_FAIL, "restore took %lu ms", (unsigned long)worstMs);
      }
      break;
    }
    case 3:
      sceneFrame("3 REBOOT: the C6 is reset 3 times");
      if (!resetSentMs && millis() >= nextResetMs) {
        resetSentMs = millis();
        recAtReset = hl.recoveries;
        ext.link().send(SD_RESET);  // unexpected, as far as the library knows
        ext.link().flushTx();
      } else if (resetSentMs && hl.recoveries > recAtReset) {
        worstMs = max(worstMs, millis() - resetSentMs);
        resetsDone++;
        resetSentMs = 0;
        nextResetMs = millis() + 1500;
        if (resetsDone == 3) {
          if (worstMs <= 2000) finish(R_PASS, "3/3 back, slowest %lu ms", (unsigned long)worstMs);
          else finish(R_FAIL, "slowest back %lu ms", (unsigned long)worstMs);
        }
      } else if (resetSentMs && millis() - resetSentMs > 10000) {
        finish(R_FAIL, "reset %d: no recovery", resetsDone + 1);
      }
      break;
    case 4:
      corruptionFrame();
      if (millis() - testStart >= 30000) {
        ext.link().corruptPerMille = 0;
        uint32_t sent = ext.link().corrupted - corrupted0, seen = hl.crcNacks - h0.crcNacks;
        bool ok = sent > 0 && seen == sent && hl.redraws > h0.redraws && hl.linkDowns == h0.linkDowns;
        finish(ok ? R_PASS : R_FAIL, "%lu/%lu found, %lu repairs", (unsigned long)seen, (unsigned long)sent,
               (unsigned long)(hl.redraws - h0.redraws));
      }
      break;
    case 5:
      sceneFrame("5 WATCHDOG: the C6 hangs, then reboots");
      if (hl.recoveries > recAtReset) {
        float s = (millis() - resetSentMs) / 1000.0f;
        if (hl.reboots > h0.reboots) finish(R_PASS, "back in %.1f s", s);
        else finish(R_FAIL, "recovered, but no reboot seen");
      } else if (millis() - resetSentMs > 10000) {
        finish(R_FAIL, "no recovery in 10 s");
      }
      break;
    case 6:
      sceneFrame("6 LONG RUN: press 0 on the Cardputer to stop");
      if (millis() - lastHeapMs >= 10000) {
        lastHeapMs = millis();
        SdStatusInfo st;
        if (ext.getStatus(st)) {
          stLast = st;
          heapLast = st.freeHeap;
          if (heapFirst < 0) heapFirst = heapLast;
          heapMin = min(heapMin, heapLast);
          heapMax = max(heapMax, heapLast);
        }
      }
      break;
  }
}

static void stopLongRun() {
  const RemoteDisplay::Health& hl = ext.health();
  SdStatusInfo st;
  bool alive = ext.getStatus(st);
  if (alive) {
    stLast = st;
    heapLast = st.freeHeap;
    if (heapFirst < 0) heapFirst = heapLast;
  }
  uint32_t mins = (millis() - testStart) / 60000;
  int32_t drift = heapFirst - heapLast;
  uint32_t ovf = stLast.fifoOverflows - st0.fifoOverflows, rec = hl.recoveries - h0.recoveries;
  bool ok = alive && ovf == 0 && rec == 0 && drift < 2048;
  if (ok) finish(R_PASS, "%lu min, heap drift %ld", (unsigned long)mins, (long)drift);
  else finish(R_FAIL, "drift %ld ovf %lu rec %lu", (long)drift, (unsigned long)ovf, (unsigned long)rec);
}

static void startAuto() {
  autoRun = true;
  autoIdx = 0;
  startTest(AUTO_TESTS[0]);
}

void setup() {
  demoSetup("ScreenDriver stress test");
  connectExternal(ext);
  initScene();
  if (!ext.uploadImage(0, ball, 16, 16)) localLog(TFT_RED, "sprite upload failed: %s", ext.lastError());
  canvas.setColorDepth(16);
  canvas.createSprite(240, 135);
  refreshScreen(true);
}

void loop() {
  Keys k = readKeys();
  char key = k.any && k.text.length() ? k.text[0] : 0;

  loopsThisSec++;
  if (millis() - lastRateMs >= 1000) {
    loopRate = loopsThisSec;
    loopsThisSec = 0;
    lastRateMs = millis();
  }
  // track how long the link has been down (for the unplug screen)
  if (!ext.connected() && !downSinceMs) downSinceMs = millis();
  if (ext.connected()) downSinceMs = 0;

  switch (mode) {
    case MENU:
      if (key >= '1' && key <= '6') startTest(key - '0');
      else if (key == 'a' || key == 'A') startAuto();
      else sceneFrame("stress test: choose a test on the Cardputer");
      break;
    case RUNNING:
      if (key == '0') {
        if (test == 6) stopLongRun();
        else {
          autoRun = false;
          finish(R_ABORT, "aborted");
        }
      } else {
        runTest();
      }
      break;
    case VERDICT:
      ext.poll();
      if (autoRun && millis() - verdictMs > 3000) {
        if (++autoIdx < (int)(sizeof AUTO_TESTS / sizeof AUTO_TESTS[0])) startTest(AUTO_TESTS[autoIdx]);
        else {
          autoRun = false;
          mode = MENU;
        }
      } else if (!autoRun && k.any) {
        mode = MENU;
      }
      delay(5);
      break;
  }
  ext.wasReset();  // recovery is automatic; consume the flag
  refreshScreen();
}
