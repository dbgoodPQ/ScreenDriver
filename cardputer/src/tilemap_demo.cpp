// Milestone 6 tilemap demo. A world 4 screens wide (80 x 20 tiles of 16x16) is stored on the C6 as a tilemap and
// drawn with ONE command per frame, scrolling endlessly (wrap). Slimes walk on the ground and coins spin above the
// brick platforms: all sprites of a frame go out as one batch. A fixed HUD sits on top.
// Needs /screendriver/tiles.png, coin.png and slime.png on the SD card (tools/make_sample_art.py).
//
// Keys (hold): , left   / right   ; up   . down     space: dig the ground under the screen center
//              a: auto-scroll on/off
#include "demo_common.h"

static RemoteDisplay ext;

enum { A_TILES = 0, A_COIN, A_SLIME, A_MAP };
enum { T_SKY, T_GRASS, T_DIRT, T_BRICK, T_STONE, T_WATER, T_CLOUD, T_FLOWER };
static const int MW = 80, MH = 20, TS = 16;  // world: 1280 x 320 pixels
static const int WORLD_W = MW * TS, WORLD_H = MH * TS;
static const int HUD = 16, VIEW_H = SD_SCREEN_H - HUD;  // the tilemap fills the screen below the HUD
static uint8_t world[MW * MH];

static bool solid(uint8_t t) { return t == T_GRASS || t == T_DIRT || t == T_STONE; }
static int mod(int a, int b) {
  int r = a % b;
  return r < 0 ? r + b : r;
}
// Row of the ground surface in column x (MH: none, e.g. water)
static int surface(int x) {
  for (int y = 10; y < MH; ++y)
    if (solid(world[y * MW + x])) return y;
  return MH;
}

static void makeWorld() {
  for (int x = 0; x < MW; ++x) {
    int g = 14 + (int)lroundf(2 * sinf(x / 5.0f));
    bool pit = x % 20 >= 13 && x % 20 <= 15;
    for (int y = 0; y < MH; ++y) {
      uint8_t t = T_SKY;
      if (pit) t = y >= 17 ? T_WATER : T_SKY;
      else if (y == g) t = T_GRASS;
      else if (y > g) t = y > g + 2 ? T_STONE : T_DIRT;
      world[y * MW + x] = t;
    }
    if (!pit && x % 7 == 3) world[(g - 1) * MW + x] = T_FLOWER;
  }
  for (int px = 5; px < MW - 4; px += 16)
    for (int k = 0; k < 4; ++k) world[9 * MW + px + k] = T_BRICK;
  for (int cx = 2; cx < MW - 1; cx += 9) {
    int cy = 2 + cx % 3;
    world[cy * MW + cx] = world[cy * MW + cx + 1] = T_CLOUD;
  }
}

struct Slime {
  float x;  // world pixels
  float vx;
};
static Slime slimes[8];

static float camX = 0, camY = 96;
static bool autoScroll = true;
static uint32_t frameNo = 0;
static float cpuMs = 0;

static bool loadAll() {
  // The tileset is stored as RGB565: a full screen of tiles renders in ~3.2 ms instead of ~4.9 ms with a palette
  // (palette lookups cost per pixel), and 8 tiles take only 4 KB. The sprites keep their palette (AUTO).
  ext.setImageFormat(RD_FORMAT_RGB565);
  bool ok = ext.loadSheet(A_TILES, "/screendriver/tiles.png", 16, 16);
  ext.setImageFormat(RD_FORMAT_AUTO);
  ok = ok && ext.loadSheet(A_COIN, "/screendriver/coin.png", 16, 16) &&
            ext.loadSheet(A_SLIME, "/screendriver/slime.png", 16, 16) && ext.uploadTilemap(A_MAP, world, MW, MH);
  if (!ok) {
    localLog(TFT_RED, "load failed: %s", ext.lastError());
    return false;
  }
  for (uint8_t id : {A_TILES, A_COIN, A_SLIME, A_MAP})
    localLog(TFT_GREEN, "asset %d: %s %lu bytes", id, id == A_MAP ? "map" : ext.isIndexed(id) ? "palette" : "rgb565",
             (unsigned long)ext.assetBytes(id));
  return true;
}

static void readControls(float& dx, float& dy) {
  M5Cardputer.update();
  auto& kb = M5Cardputer.Keyboard;
  dx = dy = 0;
  if (!kb.isPressed()) return;
  Keyboard_Class::KeysState s = kb.keysState();
  bool pressedNow = kb.isChange();
  for (char c : s.word) {
    if (c == ',') dx = -4;
    else if (c == '/') dx = 4;
    else if (c == ';') dy = -3;
    else if (c == '.') dy = 3;
    else if (pressedNow && c == 'a') autoScroll = !autoScroll;
    else if (pressedNow && c == ' ') {
      // dig: remove the surface tile in the column under the screen center (the map on the C6 is updated too)
      int col = mod((int)camX + SD_SCREEN_W / 2, WORLD_W) / TS;
      int row = surface(col);
      if (row < MH - 1) ext.setTile(A_MAP, col, row, T_SKY);
    }
  }
  if (dx || dy) autoScroll = false;
}

static void drawFrame() {
  int cx = (int)camX, cy = (int)camY;
  ext.startFrame();
  // world: one command
  ext.drawTilemap(A_MAP, A_TILES, cx, cy, 0, HUD, SD_SCREEN_W, VIEW_H, RD_WRAP);

  // sprites (sent as one batch): drawn inside the view only
  ext.setClipRect(0, HUD, SD_SCREEN_W, VIEW_H);
  for (int px = 5; px < MW - 4; px += 16)
    for (int k = 0; k < 4; ++k) {
      int sx = mod(px * TS + k * TS - cx, WORLD_W), sy = HUD + 8 * TS - cy;
      if (sx < SD_SCREEN_W) ext.drawSprite(A_COIN, (frameNo / 4 + k) % 8, sx, sy);
    }
  for (int i = 0; i < 8; ++i) {
    Slime& s = slimes[i];
    int col = mod((int)s.x + 8, WORLD_W) / TS;
    int row = surface(col);
    int sx = mod((int)s.x - cx, WORLD_W), sy = HUD + row * TS - 16 - cy;
    if (sx < SD_SCREEN_W && row < MH)
      ext.drawSprite(A_SLIME, (frameNo / 6 + i) % 4, sx, sy, RD_TRANSPARENT | (s.vx < 0 ? RD_FLIP_X : 0));
  }
  ext.clearClipRect();

  // HUD
  const RemoteDisplay::FrameStats& f = ext.lastFrame();
  char s[80];
  snprintf(s, sizeof s, "%2.0f fps  C6 %4.1f+%4.1f ms  CP %4.2f ms  x %4d", f.fps, f.renderUs / 1000.0f,
           f.flushUs / 1000.0f, cpuMs, mod(cx, WORLD_W));
  ext.setFont(RD_FONT_GLCD);
  ext.setTextSize(1);
  ext.setTextDatum(0);
  ext.setTextColor(TFT_WHITE, TFT_BLACK);
  ext.fillRect(0, 0, SD_SCREEN_W, HUD, TFT_BLACK);
  ext.drawString(s, 4, 4);
  ext.endFrame();
}

static void step(float dx, float dy) {
  if (autoScroll) dx = 1.5f;
  camX += dx;
  camY = constrain(camY + dy, 0.0f, (float)(WORLD_H - VIEW_H));
  for (auto& s : slimes) {
    float nx = s.x + s.vx;
    int ahead = mod((int)nx + (s.vx > 0 ? 15 : 0), WORLD_W) / TS;
    if (surface(ahead) >= MH) s.vx = -s.vx;  // don't walk into the water
    else s.x = nx;
  }
}

void setup() {
  demoSetup("ScreenDriver tilemap demo");
  connectExternal(ext);
  makeWorld();
  while (!loadAll()) delay(2000);
  for (int i = 0; i < 8; ++i) slimes[i] = {(float)(i * 160 + 40), (i % 2 ? 0.6f : -0.6f)};
  localLog(TFT_YELLOW, "hold , / ; . to scroll");
  localLog(TFT_YELLOW, "space: dig   a: auto-scroll");
}

void loop() {
  uint32_t t0 = micros();
  float dx, dy;
  readControls(dx, dy);
  step(dx, dy);
  drawFrame();
  // Cardputer CPU per frame = frame time minus the time endFrame() waited for the C6
  float ms = (micros() - t0 - ext.lastFrame().waitUs) / 1000.0f;
  cpuMs = cpuMs * 0.95f + ms * 0.05f;
  frameNo++;
  if (frameNo % 300 == 0)
    localLog(TFT_WHITE, "%2.0f fps C6 %.1f+%.1f CP %.2f", ext.lastFrame().fps, ext.lastFrame().renderUs / 1000.0f,
             ext.lastFrame().flushUs / 1000.0f, cpuMs);
  ext.wasReset();  // full redraw every frame: nothing else to do after a recovery
}
