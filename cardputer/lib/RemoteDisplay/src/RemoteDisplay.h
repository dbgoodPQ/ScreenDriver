// RemoteDisplay: M5GFX-style drawing on the ScreenDriver coprocessor (XIAO ESP32-C6 + ILI9341, 320x240).
// Every call is turned into a small command for the C6, which renders into its own framebuffer.
// Nothing appears on the panel until endFrame(): the C6 then sends only the changed areas.
//
//   RemoteDisplay ext;
//   ext.begin();                        // resets the C6 and waits for it
//   ext.startFrame();
//   ext.fillRect(10, 10, 100, 40, TFT_BLUE);
//   ext.setFont(RD_FONT_FONT4); ext.setTextColor(TFT_WHITE);
//   ext.drawString("Hello", 20, 18);
//   ext.endFrame();                     // waits for the previous frame, then sends this one
//
// Colors are RGB565, so M5GFX's TFT_* constants and color565() work as usual.
#pragma once
#include <Arduino.h>
#include <functional>
#include "SdHostLink.h"

// Oldest C6 firmware this library works with (0600 = palette images, tilemaps, sprite batches)
#define RD_MIN_C6_FW 0x0600

enum : uint8_t {
  RD_FONT_GLCD = SD_FONT_GLCD,        // 6x8 pixels
  RD_FONT_FONT2 = SD_FONT_FONT2,      // 16 px
  RD_FONT_FONT4 = SD_FONT_FONT4,      // 26 px
  RD_FONT_FONT7 = SD_FONT_FONT7,      // 7-segment, 48 px, digits only
  RD_FONT_SANS9 = SD_FONT_SANS9,      // FreeSans 9 pt
  RD_FONT_SANSB12 = SD_FONT_SANSB12,  // FreeSans Bold 12 pt
};

// Sprite flags for drawSprite()
#define RD_TRANSPARENT 0x01  // skip pixels of the asset's transparent color
#define RD_FLIP_X      0x02
#define RD_FLIP_Y      0x04
#define RD_WRAP        0x08  // drawTilemap(): repeat the map in both directions
#define RD_TILE_EMPTY  0xFF  // a tile number that is never drawn (see-through)

// How images are stored on the C6 (setImageFormat). AUTO: as 8-bit palette images when they have at most 256
// colors (all pixel art; half the pool space of RGB565), else RGB565.
enum : uint8_t { RD_FORMAT_AUTO = 0, RD_FORMAT_RGB565 = 1, RD_FORMAT_INDEXED = 2 };
// Default transparent color: magenta. PNG transparency is mapped to it automatically, so opaque magenta
// pixels in an image become transparent too.
#define RD_KEY_DEFAULT 0xF81F

class RemoteDisplay {
 public:
  struct FrameStats {
    uint16_t frameNo = 0;  // C6 frame counter (of the last completed frame)
    uint32_t renderUs = 0; // C6 time spent executing that frame's commands
    uint32_t flushUs = 0;  // C6 time spent sending it to the panel
    uint32_t waitUs = 0;   // time endFrame() waited for the previous frame (the Cardputer was idle)
    float fps = 0;         // frames per second, measured on the Cardputer
  };

  // Resets the C6, waits for its HELLO and initialises the panel (rotation 1 or 3, both landscape).
  // Returns false if the C6 does not answer; lastError() says why.
  bool begin(uint8_t rotation = 1, uint32_t timeoutMs = 4000);
  const char* lastError() const { return _err; }

  int32_t width() const { return SD_SCREEN_W; }
  int32_t height() const { return SD_SCREEN_H; }
  static constexpr uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }

  // ---- frames
  void startFrame();
  // Sends END_FRAME. Blocks until the previous frame is done (at most one frame in flight).
  void endFrame(bool fullFlush = false);
  const FrameStats& lastFrame() const { return _stats; }

  // ---- primitives
  void fillScreen(uint16_t color);
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color);
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color);
  void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color);
  void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color);
  void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
  void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
  void drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color);
  void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color);
  // Pixels of the same color are batched into one command (up to 255 per command).
  void drawPixel(int16_t x, int16_t y, uint16_t color);

  // ---- clipping
  void setClipRect(int16_t x, int16_t y, int16_t w, int16_t h);
  void clearClipRect();

  // ---- text (the style is sent lazily, just before the next text command)
  void setFont(uint8_t fontId);
  void setTextSize(uint8_t size);
  void setTextColor(uint16_t fg);               // transparent background
  void setTextColor(uint16_t fg, uint16_t bg);  // filled background
  // M5GFX/LovyanGFX textdatum values (TL_DATUM, MC_DATUM, ... also work):
  //   top: 0 left, 1 center, 2 right | middle: 4, 5, 6 | bottom: 8, 9, 10 | baseline: 16, 17, 18
  void setTextDatum(uint8_t datum);
  void drawString(const char* s, int16_t x, int16_t y);

  // Console-style text at the cursor: wraps at the right edge, '\n' starts a new line.
  void setCursor(int16_t x, int16_t y);
  void print(const char* s);
  void println(const char* s = "");
  void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

  // ---- images and sprites (milestones 4 and 6)
  // Images are uploaded to the C6 once (ids 0..63) and then drawn with tiny commands.
  // Images with at most 256 colors are stored with a palette (1 byte per pixel), see setImageFormat().
  void setImageFormat(uint8_t format) { _imageFormat = format; }  // for the following uploads
  // Load a PNG or BMP (16/24-bit) from the SD card. PNG transparency becomes the transparent color.
  // The Cardputer needs w*h*2 bytes of free RAM while loading (about 100 KB max without PSRAM).
  bool loadImage(uint8_t id, const char* path, uint16_t transparent = RD_KEY_DEFAULT);
  // A sprite sheet: frames of frameW x frameH, numbered row by row from the top-left.
  bool loadSheet(uint8_t id, const char* path, uint16_t frameW, uint16_t frameH, uint16_t transparent = RD_KEY_DEFAULT);
  // Upload pixels from RAM/flash (RGB565 values, row-major). frameW/H = 0: a single image.
  // restore() re-sends these from the same pointer, so keep the pixels alive (e.g. a const array).
  bool uploadImage(uint8_t id, const uint16_t* pixels, uint16_t w, uint16_t h, uint16_t frameW = 0,
                   uint16_t frameH = 0, uint16_t transparent = RD_KEY_DEFAULT);
  void freeAsset(uint8_t id);
  void freeAllAssets();
  bool hasAsset(uint8_t id) const { return id < SD_MAX_ASSETS && _slots[id].used; }
  uint16_t frameCount(uint8_t id) const;
  uint16_t frameWidth(uint8_t id) const { return hasAsset(id) ? _slots[id].fw : 0; }
  uint16_t frameHeight(uint8_t id) const { return hasAsset(id) ? _slots[id].fh : 0; }
  bool isIndexed(uint8_t id) const { return hasAsset(id) && _slots[id].indexed; }
  uint32_t assetBytes(uint8_t id) const;  // C6 pool space the asset uses
  // Free bytes in the C6 asset pool (asks the C6). Returns -1 if it does not answer.
  int32_t assetPoolFree();

  // Draws frame `frame` of an asset with its top-left at x,y. flags: RD_TRANSPARENT | RD_FLIP_X | RD_FLIP_Y
  // Consecutive drawSprite() calls are sent to the C6 as one command (up to 127 sprites), so many sprites are cheap.
  // setSpriteBatch(n): at most n sprites per command (1..127, 1 = one command per sprite).
  void setSpriteBatch(uint8_t n);
  void drawSprite(uint8_t id, uint16_t frame, int16_t x, int16_t y, uint8_t flags = RD_TRANSPARENT);
  // Rotated (degrees, clockwise) and scaled; cx,cy is where the frame's center lands. Negative scale flips.
  void drawSpriteEx(uint8_t id, uint16_t frame, int16_t cx, int16_t cy, float angleDeg, float scaleX = 1.0f,
                    float scaleY = 1.0f, uint8_t flags = RD_TRANSPARENT);
  // Draws pixels straight from RAM (RGB565, row-major), without storing them on the C6.
  void pushImage(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t* pixels);
  // Copies a screen area (on the C6 framebuffer), e.g. to scroll.
  void copyRect(int16_t srcX, int16_t srcY, int16_t w, int16_t h, int16_t dstX, int16_t dstY);

  // ---- tilemaps (milestone 6)
  // A tilemap is a w x h grid of tile numbers (1 byte each, row-major) stored on the C6 under an asset id. Any
  // image/sheet works as its tileset: tile number n is frame n of the sheet; RD_TILE_EMPTY is never drawn.
  // Keep `tiles` alive: setTile() writes into it and restore() re-sends it.
  bool uploadTilemap(uint8_t id, uint8_t* tiles, uint16_t w, uint16_t h);
  void setTile(uint8_t mapId, uint16_t tx, uint16_t ty, uint8_t tile);
  uint8_t getTile(uint8_t mapId, uint16_t tx, uint16_t ty) const;
  // Fills the screen rectangle x,y,w,h with the map; map pixel (scrollX, scrollY) lands at the rectangle's
  // top-left. flags: RD_TRANSPARENT (tile pixels of the tileset's transparent color are skipped), RD_WRAP.
  // Without RD_WRAP, areas outside the map are left as they are.
  void drawTilemap(uint8_t mapId, uint8_t tilesetId, int16_t scrollX, int16_t scrollY, int16_t x = 0, int16_t y = 0,
                   int16_t w = SD_SCREEN_W, int16_t h = SD_SCREEN_H, uint8_t flags = 0);

  // After wasReset(): begin() again and re-upload every asset (from the SD card or RAM). Then redraw.
  bool restore();
  // Mounts the Cardputer-ADV's SD card (done automatically by loadImage/loadSheet).
  static bool mountSD();

  // ---- robustness (milestone 5)
  // The library watches the link. If the C6 reboots, the cable comes off or a command is corrupted, it recovers
  // by itself: drawing is dropped while the link is down (the app keeps running), then the C6 is reset, the
  // images are re-uploaded, and the redraw callback is called.
  //
  // The callback runs at the start of the next frame (inside startFrame()): draw everything that is not redrawn
  // every frame (backgrounds, labels). Don't call startFrame()/endFrame() in it. Apps that redraw the whole
  // screen every frame don't need one.
  void onRedraw(std::function<void()> fn) { _onRedraw = fn; }
  bool connected() const { return _connected; }
  struct Health {
    uint32_t linkDowns = 0;       // times the link was lost
    uint32_t recoveries = 0;      // successful recoveries
    uint32_t reboots = 0;         // of which: the C6 had rebooted
    uint32_t redraws = 0;         // redraw callbacks run
    uint32_t crcNacks = 0;        // commands lost to corruption
    uint32_t lastRecoveryMs = 0;  // time the last recovery took (reset + re-upload)
    uint32_t lastDownMs = 0;      // how long the link was down before that
  };
  const Health& health() const { return _health; }

  // ---- misc
  void setBrightness(uint8_t level);
  // Requests the C6's STATUS (link counters, fps). Blocks up to timeoutMs.
  bool getStatus(SdStatusInfo& out, uint32_t timeoutMs = 500);
  // Handles pending replies without blocking. Call it now and then if you don't call endFrame().
  void poll();
  // Handles replies and runs the link checks (and recovery). Call it in your loop if you don't draw frames.
  // True (once) after an automatic recovery: the external screen was cleared, so redraw it.
  bool wasReset();
  uint32_t nackCount() const { return _link.nacks; }
  uint8_t lastNackCmd() const { return _link.lastNackCmd; }
  uint8_t lastNackError() const { return _link.lastNackErr; }

  SdHostLink& link() { return _link; }

 private:
  void cmd(uint8_t c, const uint8_t* p = nullptr, uint16_t n = 0);
  void checkHealth();
  void goDown();
  void probe(uint32_t now);
  void recover(bool rebooted);
  bool restoreWithTimeout(uint32_t helloTimeoutMs);
  uint32_t doneCount() const { return _link.framesDone + _forgiven; }
  void flushPixels();
  void flushBlits();
  void flushPending() {
    if (_pxCount) flushPixels();
    if (_blitCount) flushBlits();
  }
  void sendStyleIfNeeded();
  void sendText(uint8_t c, const uint8_t* prefix, uint16_t prefixLen, const char* s);
  uint8_t cmdSeq(uint8_t c, const uint8_t* p, uint16_t n);
  bool waitAck(uint8_t seq, uint32_t timeoutMs);
  bool sendAsset(uint8_t id, const uint16_t* px, uint16_t w, uint16_t h, uint16_t fw, uint16_t fh, uint16_t key,
                 uint8_t format);
  bool sendTilemap(uint8_t id, uint8_t* tiles, uint16_t w, uint16_t h);
  bool beginUpload(const uint8_t* hdr, uint16_t n);
  void dataPut(const uint8_t* bytes, uint32_t n);
  bool endUpload();
  bool loadFile(uint8_t id, const char* path, uint16_t fw, uint16_t fh, uint16_t key, uint8_t format);
  static const char* errName(uint8_t e);

  struct Slot {
    bool used = false;
    const uint16_t* mem = nullptr;  // uploaded from RAM
    char path[64] = "";             // or loaded from the SD card
    uint8_t* map = nullptr;         // or a tilemap (the app's array)
    uint16_t w = 0, h = 0, fw = 0, fh = 0, key = 0;
    uint8_t format = RD_FORMAT_AUTO;  // requested format (for restore)
    bool indexed = false;             // stored with a palette
    uint16_t palCount = 0;
  };
  uint8_t _imageFormat = RD_FORMAT_AUTO;
  // upload stream
  uint8_t _upId = 0;
  uint32_t _upStart = 0;
  uint16_t _upFill = 0;
  Slot _slots[SD_MAX_ASSETS];
  uint8_t _rotation = 1;
  char _errBuf[96];

  SdHostLink _link;
  bool _linkUp = false;
  const char* _err = "";
  uint32_t _helloBase = 0;

  // text style (sent lazily)
  uint8_t _font = RD_FONT_GLCD, _size = 1, _datum = 0;
  uint16_t _fg = 0xFFFF, _bg = 0;
  bool _drawBg = false, _styleDirty = true;

  // pixel batch: color + up to 255 points
  uint8_t _px[2 + 255 * 4];
  uint16_t _pxCount = 0, _pxColor = 0;

  // sprite batch: consecutive drawSprite() calls (BLIT payloads, 8 bytes each)
  static const int BLIT_MAX = SD_MAX_PAYLOAD / 8;  // 128 would fill the payload exactly; keep it below
  uint8_t _blit[(BLIT_MAX - 1) * 8];
  uint16_t _blitCount = 0;
  uint8_t _blitLimit = BLIT_MAX - 1;  // setSpriteBatch()

  // health
  bool _connected = false, _inRecovery = false, _redrawPending = false, _fullNext = false, _resetFlag = false;
  std::function<void()> _onRedraw;
  Health _health;
  uint32_t _seenDone = 0, _pendingSinceMs = 0, _forgiven = 0;
  uint32_t _crcSeen = 0, _badAssetSeen = 0, _txDropsSeen = 0, _pongSeen = 0;
  uint32_t _downSinceMs = 0, _lastProbeMs = 0;
  int _probeSeq = -1;

  // frame pacing
  uint32_t _framesSent = 0;
  uint32_t _lastEndMs = 0;
  FrameStats _stats;
};
