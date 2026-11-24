// Render benchmark (milestones 3-6). Runs timed scenes on the external screen and reports, per scene:
// frames per second, C6 render and flush time, and the Cardputer's own CPU time per frame
// (frame time minus the time endFrame() spent waiting for the C6). Any key reruns.
// Every run is also appended to /screendriver/bench.txt on the SD card (if one is inserted).
#include "demo_common.h"
#include <SD.h>

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
static uint16_t tiles[64 * 16];  // 4 tiles of 16x16
static uint8_t tmap[40 * 30];
static bool mapReady = false;

struct Result {
  float fps, renderMs, flushMs, hostMs, waitMs;
};

// Results file on the SD card: opened for each run, closed at its end so the card can be pulled at once
static const char* LOG_PATH = "/screendriver/bench.txt";
static File logFile;
static int runNo = 0;

static void fileLog(const char* fmt, ...) {
  if (!logFile) return;
  char s[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s, sizeof s, fmt, ap);
  va_end(ap);
  logFile.println(s);
}

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
  r.waitMs = waitSum / 1000.0f / frames;
  return r;
}

static void report(const char* name, const Result& r) {
  localLog(TFT_WHITE, "%-9s%4.0ffps C6%5.1f+%4.1f CP%5.2f", name, r.fps, r.renderMs, r.flushMs, r.hostMs);
  fileLog("%-9s fps %5.1f  frame %6.2f ms  C6 render %6.2f flush %6.2f  CP cpu %5.2f wait %6.2f ms", name, r.fps,
          1000.0f / r.fps, r.renderMs, r.flushMs, r.hostMs, r.waitMs);
}

static void runAll() {
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.setCursor(0, 0);
  localLog(TFT_CYAN, "render bench, times in ms per frame");

  localLog(TFT_DARKGREY, "C6 = render+flush, CP = Cardputer CPU");
  runNo++;
  if (RemoteDisplay::mountSD()) logFile = SD.open(LOG_PATH, FILE_APPEND);
  if (!logFile) localLog(TFT_ORANGE, "no SD card: results not saved");
  fileLog("");
  fileLog("=== render bench run %d  (uptime %lu s, C6 fw %04X)", runNo, (unsigned long)(millis() / 1000),
          ext.link().hello.fw);
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
    // id 0: AUTO (stored with a palette, 1 byte per pixel), id 3: the same image as RGB565
    ballReady = ext.uploadImage(0, ball, 16, 16);
    ext.setImageFormat(RD_FORMAT_RGB565);
    ballReady = ballReady && ext.uploadImage(3, ball, 16, 16);
    ext.setImageFormat(RD_FORMAT_AUTO);
    if (!ballReady) localLog(TFT_RED, "sprite upload failed: %s", ext.lastError());
  }
  if (ballReady) {
    report("50 spr", runScene(60, [](int i) {
      ext.fillScreen(TFT_BLACK);
      for (int j = 0; j < 50; ++j) ext.drawSprite(0, 0, (j * 37 + i * 3) % 304, (j * 53 + i * 2) % 224);
    }));
    // 200 sprites: palette image in batches of 127 (the default), 16 and 1 (one command each), then RGB565
    static uint8_t sprId = 0;
    auto spr200 = [](int i) {
      ext.fillScreen(TFT_BLACK);
      for (int j = 0; j < 200; ++j) ext.drawSprite(sprId, 0, (j * 37 + i * 3) % 304, (j * 53 + i * 2) % 224);
    };
    report("200 spr", runScene(60, spr200));
    ext.setSpriteBatch(16);
    report("200 b16", runScene(60, spr200));
    ext.setSpriteBatch(1);
    report("200 b1", runScene(60, spr200));
    ext.setSpriteBatch(127);
    sprId = 3;
    report("200 565", runScene(60, spr200));
    sprId = 0;
  }

  // 8. Tilemap: a full screen of 16x16 tiles (300 tiles), scrolling, in one command per frame
  if (!mapReady) {
    const uint16_t tc[4] = {TFT_DARKGREEN, TFT_BROWN, TFT_NAVY, TFT_DARKGREY};
    for (int t = 0; t < 4; ++t)
      for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) tiles[y * 64 + t * 16 + x] = (x == 0 || y == 0) ? TFT_BLACK : tc[t];
    for (int i = 0; i < 40 * 30; ++i) tmap[i] = (i * 7 + i / 40) % 4;
    // tileset id 1: AUTO (palette), id 4: RGB565
    mapReady = ext.uploadImage(1, tiles, 64, 16, 16, 16) && ext.uploadTilemap(2, tmap, 40, 30);
    ext.setImageFormat(RD_FORMAT_RGB565);
    mapReady = mapReady && ext.uploadImage(4, tiles, 64, 16, 16, 16);
    ext.setImageFormat(RD_FORMAT_AUTO);
    if (!mapReady) localLog(TFT_RED, "tilemap upload failed: %s", ext.lastError());
  }
  if (mapReady) {
    report("tilemap", runScene(60, [](int i) { ext.drawTilemap(2, 1, i * 3, i * 2, 0, 0, 320, 240, RD_WRAP); }));
    report("tile 565", runScene(60, [](int i) { ext.drawTilemap(2, 4, i * 3, i * 2, 0, 0, 320, 240, RD_WRAP); }));
  }

  SdStatusInfo st;
  if (ext.getStatus(st)) {
    localLog(st.fifoOverflows || st.crcErrors ? TFT_RED : TFT_GREEN, "link: ovf %lu crc %lu bp %lu",
             (unsigned long)st.fifoOverflows, (unsigned long)st.crcErrors, (unsigned long)st.backpressure);
    fileLog("link: ovf %lu crc %lu bp %lu  C6 heap %lu pool free %lu", (unsigned long)st.fifoOverflows,
            (unsigned long)st.crcErrors, (unsigned long)st.backpressure, (unsigned long)st.freeHeap,
            (unsigned long)st.assetFree);
  } else {
    fileLog("link: status query failed (%s)", ext.lastError());
  }
  if (logFile) {
    logFile.close();
    localLog(TFT_GREEN, "saved to %s", LOG_PATH);
  }
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
