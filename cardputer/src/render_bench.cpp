// Milestone 3 render benchmark. Runs timed scenes on the external screen and reports, per scene:
// frames per second, C6 render and flush time, and the Cardputer's own CPU time per frame
// (frame time minus the time endFrame() spent waiting for the C6). Any key reruns.
#include "demo_common.h"

static RemoteDisplay ext;

// Fast software random numbers (xorshift32). Arduino's random() reads the hardware RNG, which costs
// ~30 us per call on the ESP32-S3 with Wi-Fi off and would hide the library's own CPU time.
static uint32_t rngState = 0x12345678;
static inline int rnd(int lo, int hi) {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 17;
  rngState ^= rngState << 5;
  return lo + (int)(rngState % (uint32_t)(hi - lo));
}

static uint16_t ball[16 * 16];  // 16x16 sprite made in code (no SD card needed)
static bool ballReady = false;

struct Result {
  float fps, renderMs, flushMs, hostMs;
};

// Runs `frames` frames of a scene and measures them.
template <typename F>
static Result runScene(int frames, F drawFrame) {
  ext.endFrame();  // settle: make sure nothing from before is in flight
  uint32_t t0 = micros();
  uint64_t waitSum = 0, renderSum = 0, flushSum = 0;
  for (int i = 0; i < frames; ++i) {
    ext.startFrame();
    drawFrame(i);
    ext.endFrame();
    const RemoteDisplay::FrameStats& f = ext.lastFrame();
    waitSum += f.waitUs;
    renderSum += f.renderUs;
    flushSum += f.flushUs;
  }
  ext.endFrame();  // wait for the last frame so the timing includes it
  uint32_t el = micros() - t0;
  Result r;
  r.fps = frames * 1e6f / el;
  r.renderMs = renderSum / 1000.0f / frames;
  r.flushMs = flushSum / 1000.0f / frames;
  r.hostMs = (el - (uint32_t)waitSum) / 1000.0f / frames;
  return r;
}

static void report(const char* name, const Result& r) {
  localLog(TFT_WHITE, "%-9s%4.0ffps C6%5.1f+%4.1f CP%5.2f", name, r.fps, r.renderMs, r.flushMs, r.hostMs);
}

static void runAll() {
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.setCursor(0, 0);
  localLog(TFT_CYAN, "render bench, times in ms per frame");

  localLog(TFT_DARKGREY, "C6 = render+flush, CP = Cardputer CPU");
  // 1. Full-screen redraw every frame (worst case for the panel flush)
  report("full", runScene(60, [](int i) { ext.fillScreen(i & 1 ? TFT_NAVY : TFT_MAROON); }));

  // 2. 100 random small rectangles per frame, no clear
  report("100 rects", runScene(60, [](int) {
    for (int j = 0; j < 100; ++j)
      ext.fillRect(rnd(0, 300), rnd(0, 220), rnd(4, 20), rnd(4, 20), (uint16_t)rnd(0, 65536));
  }));

  // 3. Text-heavy screen: 12 lines of Font2 text with changing numbers.
  //    a) text with a filled background, b) clear each line with fillRect, then transparent text
  ext.startFrame();
  ext.fillScreen(TFT_BLACK);
  ext.endFrame(true);
  report("text bg", runScene(60, [](int i) {
    ext.setFont(RD_FONT_FONT2);
    ext.setTextColor(TFT_WHITE, TFT_BLACK);
    ext.setTextDatum(0);
    char s[48];
    for (int l = 0; l < 12; ++l) {
      snprintf(s, sizeof s, "line %2d  value %6d  status OK", l, i * 37 + l * 1013);
      ext.drawString(s, 4, 4 + l * 19);
    }
  }));
  report("text fg", runScene(60, [](int i) {
    ext.setFont(RD_FONT_FONT2);
    ext.setTextColor(TFT_YELLOW);
    ext.setTextDatum(0);
    char s[48];
    for (int l = 0; l < 12; ++l) {
      snprintf(s, sizeof s, "line %2d  value %6d  status OK", l, i * 37 + l * 1013);
      ext.fillRect(4, 4 + l * 19, 240, 16, TFT_BLACK);
      ext.drawString(s, 4, 4 + l * 19);
    }
  }));

  // 4. Small moving sprite (32x32): the typical game/UI case, tiny dirty area
  ext.startFrame();
  ext.fillScreen(TFT_BLACK);
  ext.endFrame(true);
  report("sprite", runScene(200, [](int i) {
    int x = (i * 3) % 288, px = ((i - 1) * 3 + 288) % 288;
    ext.fillRect(px, 100, 32, 32, TFT_BLACK);
    ext.fillRoundRect(x, 100, 32, 32, 6, TFT_ORANGE);
  }));

  // 5. 1000 pixels per frame (batched by drawPixel)
  report("pixels", runScene(60, [](int i) {
    uint16_t c = i & 1 ? TFT_WHITE : TFT_BLACK;
    for (int j = 0; j < 1000; ++j) ext.drawPixel(rnd(0, 320), rnd(0, 240), c);
  }));

  // 6./7. Sprites: clear + N transparent 16x16 blits per frame (game-like full redraw)
  if (!ballReady) {
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x) {
        int dx = x - 8, dy = y - 8;
        ball[y * 16 + x] = dx * dx + dy * dy < 56 ? RemoteDisplay::color565(40 + x * 12, 200 - y * 8, 255) : RD_KEY_DEFAULT;
      }
    ballReady = ext.uploadImage(0, ball, 16, 16);
    if (!ballReady) localLog(TFT_RED, "sprite upload failed: %s", ext.lastError());
  }
  if (ballReady) {
    report("50 spr", runScene(60, [](int i) {
      ext.fillScreen(TFT_BLACK);
      for (int j = 0; j < 50; ++j) ext.drawSprite(0, 0, (j * 37 + i * 3) % 304, (j * 53 + i * 2) % 224);
    }));
    report("200 spr", runScene(60, [](int i) {
      ext.fillScreen(TFT_BLACK);
      for (int j = 0; j < 200; ++j) ext.drawSprite(0, 0, (j * 37 + i * 3) % 304, (j * 53 + i * 2) % 224);
    }));
  }

  SdStatusInfo st;
  if (ext.getStatus(st))
    localLog(st.fifoOverflows || st.crcErrors ? TFT_RED : TFT_GREEN, "link: ovf %lu crc %lu bp %lu",
             (unsigned long)st.fifoOverflows, (unsigned long)st.crcErrors, (unsigned long)st.backpressure);
  localLog(TFT_DARKGREY, "any key: rerun");
}

void setup() {
  demoSetup("ScreenDriver render bench");
  connectExternal(ext);
  runAll();
}

void loop() {
  Keys k = readKeys();
  if (k.any) runAll();
  if (ext.wasReset()) runAll();  // the library recovered the link (and re-uploaded the sprite)
  delay(10);
}
