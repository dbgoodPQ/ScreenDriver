// Milestone 4 sprite demo. Loads images from the SD card (/screendriver/) into the C6 once, then every
// frame redraws a tiled background, N bouncing animated sprites (flipped by direction) and a rotating,
// scaling ship. Keys: ; more sprites, . fewer sprites, Enter toggles the ship, any key logs the stats.
#include "demo_common.h"

static RemoteDisplay ext;

enum { A_TILE = 0, A_COIN, A_SLIME, A_SHIP, A_BALL };
static const int COUNTS[] = {10, 25, 50, 100, 200};
static int countIdx = 2;
static bool showShip = true;

struct Mover {
  float x, y, vx, vy;
  uint8_t asset;
  uint8_t phase;
};
static Mover movers[200];
static uint32_t frame = 0;

static bool loadAssets() {
  struct { uint8_t id; const char* path; uint16_t fw, fh; } list[] = {
      {A_TILE, "/screendriver/tile.png", 0, 0},     {A_COIN, "/screendriver/coin.png", 16, 16},
      {A_SLIME, "/screendriver/slime.png", 16, 16}, {A_SHIP, "/screendriver/ship.png", 0, 0},
      {A_BALL, "/screendriver/ball.bmp", 0, 0},
  };
  for (auto& e : list) {
    uint32_t t0 = millis();
    bool ok = e.fw ? ext.loadSheet(e.id, e.path, e.fw, e.fh) : ext.loadImage(e.id, e.path);
    if (!ok) {
      localLog(TFT_RED, "%s: %s", e.path + 14, ext.lastError());
      return false;
    }
    localLog(TFT_GREEN, "%-10s %dx%d x%d  %lu ms", e.path + 14, ext.frameWidth(e.id), ext.frameHeight(e.id),
             ext.frameCount(e.id), (unsigned long)(millis() - t0));
  }
  localLog(TFT_WHITE, "C6 pool free: %ld bytes", (long)ext.assetPoolFree());
  return true;
}

static void initMovers() {
  for (int i = 0; i < 200; ++i) {
    Mover& m = movers[i];
    m.x = random(0, 304);
    m.y = random(16, 224);
    m.vx = (random(0, 2) ? 1 : -1) * (0.5f + random(0, 100) / 40.0f);
    m.vy = (random(0, 2) ? 1 : -1) * (0.5f + random(0, 100) / 40.0f);
    m.asset = i % 3 == 0 ? A_COIN : i % 3 == 1 ? A_SLIME : A_BALL;
    m.phase = random(0, 8);
  }
}

static void drawFrame() {
  int n = COUNTS[countIdx];
  ext.startFrame();
  // Background: 10 x 8 tiles of 32x32 (opaque, so no clearing needed)
  for (int ty = 0; ty < 8; ++ty)
    for (int tx = 0; tx < 10; ++tx) ext.drawSprite(A_TILE, 0, tx * 32, ty * 32, 0);

  for (int i = 0; i < n; ++i) {
    Mover& m = movers[i];
    m.x += m.vx;
    m.y += m.vy;
    if (m.x < 0 || m.x > 304) { m.vx = -m.vx; m.x = constrain(m.x, 0.0f, 304.0f); }
    if (m.y < 14 || m.y > 224) { m.vy = -m.vy; m.y = constrain(m.y, 14.0f, 224.0f); }
    uint16_t f = 0;
    uint8_t flags = RD_TRANSPARENT;
    if (m.asset == A_COIN) f = (frame / 3 + m.phase) % 8;
    else if (m.asset == A_SLIME) {
      f = (frame / 5 + m.phase) % 4;
      if (m.vx < 0) flags |= RD_FLIP_X;  // the slime faces where it goes
    }
    ext.drawSprite(m.asset, f, (int16_t)m.x, (int16_t)m.y, flags);
  }

  if (showShip) {
    float a = frame * 2.0f;
    float s = 1.5f + 0.8f * sinf(frame * 0.05f);
    ext.drawSpriteEx(A_SHIP, 0, 160, 120, a, s, s);
  }

  // HUD
  const RemoteDisplay::FrameStats& st = ext.lastFrame();
  char t[64];
  snprintf(t, sizeof t, "%d sprites %3.0f fps  C6 %4.1f+%4.1f ms  CP idle %4.1f", n, st.fps, st.renderUs / 1000.0f,
           st.flushUs / 1000.0f, st.waitUs / 1000.0f);
  ext.fillRect(0, 0, 320, 12, TFT_BLACK);
  ext.setFont(RD_FONT_GLCD);
  ext.setTextColor(TFT_WHITE);
  ext.setTextDatum(0);
  ext.drawString(t, 2, 2);
  ext.endFrame();
  frame++;
}

static void startDemo() {
  connectExternal(ext);
  while (!loadAssets()) {
    localLog(TFT_YELLOW, "copy the sample art to /screendriver/");
    localLog(TFT_YELLOW, "on the SD card; retrying in 3 s");
    delay(3000);
  }
  localLog(TFT_DARKGREY, "; more  . fewer  Enter ship");
}

void setup() {
  demoSetup("ScreenDriver sprite demo");
  initMovers();
  startDemo();
}

void loop() {
  Keys k = readKeys();
  if (k.up && countIdx < 4) countIdx++;
  if (k.down && countIdx > 0) countIdx--;
  if (k.enter) showShip = !showShip;
  if (k.any) {
    const RemoteDisplay::FrameStats& st = ext.lastFrame();
    localLog(TFT_WHITE, "%3d spr %4.0ffps C6 %4.1f+%4.1f", COUNTS[countIdx], st.fps, st.renderUs / 1000.0f,
             st.flushUs / 1000.0f);
  }
  drawFrame();
  // Recovery (C6 reboot, link loss) is automatic, images included; just report it.
  static bool wasConnected = true;
  if (ext.connected() != wasConnected) {
    wasConnected = ext.connected();
    if (wasConnected) localLog(TFT_GREEN, "link back: restored in %lu ms", (unsigned long)ext.health().lastRecoveryMs);
    else localLog(TFT_RED, "link down: drawing paused");
  }
}
