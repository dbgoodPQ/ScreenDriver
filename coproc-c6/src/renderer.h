// ScreenDriver C6 renderer (milestones 3-6: primitives, text, sprites, palette images, tilemaps).
// Executes drawing commands into the 320x240 framebuffer sprite and tracks the changed areas.
// END_FRAME flushes only those areas to the ILI9341 (or the whole screen when most of it changed).
#pragma once
#include <Arduino.h>
#include "panel_config.h"
#include "sd_protocol.h"
#include "assets.h"

// ---------------------------------------------------------------------------
// Dirty rectangle list
// ---------------------------------------------------------------------------
struct DRect {
  int16_t x, y, w, h;
  int32_t area() const { return (int32_t)w * h; }
};

static inline DRect drect_union(const DRect& a, const DRect& b) {
  int16_t x0 = min(a.x, b.x), y0 = min(a.y, b.y);
  int16_t x1 = max(a.x + a.w, b.x + b.w), y1 = max(a.y + a.h, b.y + b.h);
  return {x0, y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)};
}

class DirtyList {
 public:
  static const int MAX = 16;
  static const int32_t MERGE_SLACK = 2048;  // merge if the union wastes at most this many pixels
  static const int32_t FULL_PCT = 60;       // flush the whole screen above this share of dirty area

  void clear() {
    _n = 0;
    _full = false;
  }
  void setFull() { _full = true; }
  bool full() const { return _full; }
  int count() const { return _n; }
  const DRect& operator[](int i) const { return _r[i]; }

  // Adds a rectangle, already clipped by the caller.
  void add(DRect r) {
    if (_full || r.w <= 0 || r.h <= 0) return;
    // Absorb every existing rectangle that is cheap to merge with (overlapping or close)
    for (int i = 0; i < _n;) {
      DRect u = drect_union(_r[i], r);
      if (u.area() <= _r[i].area() + r.area() + MERGE_SLACK) {
        r = u;
        _r[i] = _r[--_n];
        i = 0;  // the bigger rect may now reach others
      } else {
        ++i;
      }
    }
    if (_n == MAX) mergeClosestPair();
    _r[_n++] = r;
    int32_t total = 0;
    for (int i = 0; i < _n; ++i) total += _r[i].area();
    if (total * 100 > (int32_t)SD_SCREEN_W * SD_SCREEN_H * FULL_PCT) _full = true;
  }

 private:
  void mergeClosestPair() {
    int bi = 0, bj = 1;
    int32_t best = INT32_MAX;
    for (int i = 0; i < _n; ++i)
      for (int j = i + 1; j < _n; ++j) {
        int32_t waste = drect_union(_r[i], _r[j]).area() - _r[i].area() - _r[j].area();
        if (waste < best) { best = waste; bi = i; bj = j; }
      }
    _r[bi] = drect_union(_r[bi], _r[bj]);
    _r[bj] = _r[--_n];
  }
  DRect _r[MAX];
  int _n = 0;
  bool _full = false;
};

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------
class Renderer {
 public:
  void begin(LGFX_ILI9341* lcd, LGFX_Sprite* fb, AssetStore* assets) {
    _lcd = lcd;
    _fb = fb;
    _assets = assets;
    _fb->setTextWrap(true, false);
    setFont(SD_FONT_GLCD);
    _fb->setTextSize(1);
    _fb->setTextColor(TFT_WHITE);
    _fb->setTextDatum(0);
    _fb->setCursor(0, 0);
  }

  // True for the opcodes handled here (drawing state, primitives, text).
  static bool handles(uint8_t cmd) {
    return (cmd >= SD_SET_CLIP && cmd <= SD_SET_CURSOR) || (cmd >= SD_FILL_SCREEN && cmd <= SD_DRAW_PIXELS) ||
           cmd == SD_DRAW_TEXT || cmd == SD_PRINT || (cmd >= SD_BLIT && cmd <= SD_DRAW_TILEMAP) || cmd == SD_COPY_RECT;
  }

  // Executes one command. Returns SD_ERR_NONE or an error code for a NACK.
  uint8_t handle(uint8_t cmd, const uint8_t* p, uint16_t n) {
    uint32_t t0 = micros();
    uint8_t err = run(cmd, p, n);
    renderUs += micros() - t0;
    return err;
  }

  // Clears the framebuffer and marks the whole screen dirty (INIT with the clear flag).
  void clearAll() {
    _fb->clearClipRect();
    _clip = {0, 0, SD_SCREEN_W, SD_SCREEN_H};
    _fb->fillScreen(TFT_BLACK);
    _dirty.setFull();
  }

  // Flushes the dirty areas to the panel. Returns the flush time in us.
  uint32_t flush(bool forceFull) {
    uint32_t t0 = micros();
    _lcd->startWrite();
    if (forceFull || _dirty.full()) {
      _fb->pushSprite(_lcd, 0, 0);
      lastFlushRects = 0;
      lastFlushPixels = (uint32_t)SD_SCREEN_W * SD_SCREEN_H;
    } else {
      lastFlushRects = _dirty.count();
      lastFlushPixels = 0;
      for (int i = 0; i < _dirty.count(); ++i) {
        const DRect& r = _dirty[i];
        _lcd->setClipRect(r.x, r.y, r.w, r.h);
        _fb->pushSprite(_lcd, 0, 0);  // only the clipped area is transferred
        lastFlushPixels += r.area();
      }
      _lcd->clearClipRect();
    }
    _lcd->endWrite();
    _lcd->waitDMA();
    _dirty.clear();
    return micros() - t0;
  }

  uint32_t renderUs = 0;  // time spent executing draw commands since the last frame
  int lastFlushRects = 0;
  uint32_t lastFlushPixels = 0;

 private:
  static int16_t i16(const uint8_t* p) { return (int16_t)sd_rd_u16(p); }

  // Adds a bounding box (in framebuffer coordinates), clipped to the clip rectangle and the screen.
  void mark(int x, int y, int w, int h) {
    int x0 = max(x, (int)_clip.x), y0 = max(y, (int)_clip.y);
    int x1 = min(x + w, _clip.x + _clip.w), y1 = min(y + h, _clip.y + _clip.h);
    if (x1 <= x0 || y1 <= y0) return;
    _dirty.add({(int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)});
  }
  void markPoints(const int16_t* xs, const int16_t* ys, int count) {
    int x0 = INT16_MAX, y0 = INT16_MAX, x1 = INT16_MIN, y1 = INT16_MIN;
    for (int i = 0; i < count; ++i) {
      x0 = min(x0, (int)xs[i]); x1 = max(x1, (int)xs[i]);
      y0 = min(y0, (int)ys[i]); y1 = max(y1, (int)ys[i]);
    }
    mark(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
  }

  // Draws frame f of an image asset (RGB565 or INDEXED) with its top-left at x,y. flags: SD_BLIT_*.
  // markDirty = false when the caller marks a larger area itself (tilemaps).
  uint8_t drawFrame(const Asset* a, uint16_t f, int x, int y, uint8_t fl, bool markDirty = true) {
    if (f >= a->frames) return SD_ERR_BAD_ARG;
    const uint8_t* pool = _assets->pool();
    bool key = fl & SD_BLIT_TRANSPARENT, fx = fl & SD_BLIT_FLIP_X, fy = fl & SD_BLIT_FLIP_Y;
    if (a->fmt == SD_FMT_RGB565)
      blit((const uint16_t*)a->frame(pool, f), a->fw, a->fh, x, y, key, a->key, fx, fy, markDirty);
    else if (a->fmt == SD_FMT_INDEXED)
      blitIdx(a->frame(pool, f), a->palette(pool), a->fw, a->fh, x, y, key && a->key < 256 ? a->key : -1, fx, fy,
              markDirty);
    else
      return SD_ERR_BAD_ASSET;  // a tilemap is not an image
    return SD_ERR_NONE;
  }

 public:  // public so the selftest can time it alone
  // One opaque, unflipped row of palette pixels: the common case (tiles). Writes two pixels per 32-bit store,
  // unrolled 8 wide (1.5x the plain loop; measured with the selftest).
  static IRAM_ATTR void idxRow(uint16_t* d, const uint8_t* s, const uint16_t* pal, int w) {
    if (((uintptr_t)d & 2) && w > 0) {  // align the destination to 4 bytes
      *d++ = pal[*s++];
      --w;
    }
    uint32_t* d32 = (uint32_t*)d;
    for (; w >= 8; w -= 8, s += 8, d32 += 4) {
      uint32_t a = pal[s[0]], b = pal[s[1]], c = pal[s[2]], e = pal[s[3]];
      uint32_t f = pal[s[4]], g = pal[s[5]], h = pal[s[6]], k = pal[s[7]];
      d32[0] = a | (b << 16);
      d32[1] = c | (e << 16);
      d32[2] = f | (g << 16);
      d32[3] = h | (k << 16);
    }
    for (; w >= 2; w -= 2, s += 2) *d32++ = pal[s[0]] | ((uint32_t)pal[s[1]] << 16);
    if (w) *(uint16_t*)d32 = pal[*s];
  }

 private:
  // The same with a transparent palette index, unrolled 4 wide
  static IRAM_ATTR void idxRowKey(uint16_t* d, const uint8_t* s, const uint16_t* pal, int w, int key) {
    for (; w >= 4; w -= 4, s += 4, d += 4) {
      int a = s[0], b = s[1], c = s[2], e = s[3];
      if (a != key) d[0] = pal[a];
      if (b != key) d[1] = pal[b];
      if (c != key) d[2] = pal[c];
      if (e != key) d[3] = pal[e];
    }
    for (; w > 0; --w, ++s, ++d)
      if (*s != key) *d = pal[*s];
  }

  // A 16-bit row with a transparent color, unrolled 4 wide
  static IRAM_ATTR void row565Key(uint16_t* d, const uint16_t* s, int w, uint16_t key) {
    for (; w >= 4; w -= 4, s += 4, d += 4) {
      uint16_t a = s[0], b = s[1], c = s[2], e = s[3];
      if (a != key) d[0] = a;
      if (b != key) d[1] = b;
      if (c != key) d[2] = c;
      if (e != key) d[3] = e;
    }
    for (; w > 0; --w, ++s, ++d)
      if (*s != key) *d = *s;
  }

  // Same as blit() for an 8-bit palette image: key is the transparent palette index, -1 for none.
  IRAM_ATTR void blitIdx(const uint8_t* src, const uint16_t* pal, int sw, int sh, int x, int y, int key, bool fx,
                         bool fy, bool markDirty) {
    int x0 = max(x, (int)_clip.x), y0 = max(y, (int)_clip.y);
    int x1 = min(x + sw, _clip.x + _clip.w), y1 = min(y + sh, _clip.y + _clip.h);
    if (x1 <= x0 || y1 <= y0) return;
    const int w = x1 - x0, sx0 = x0 - x;
    const int stepS = fy ? -sw : sw;
    const uint8_t* srow = src + (fy ? sh - 1 - (y0 - y) : (y0 - y)) * sw;
    uint16_t* drow = (uint16_t*)_fb->getBuffer() + y0 * SD_SCREEN_W + x0;
    for (int row = y0; row < y1; ++row, srow += stepS, drow += SD_SCREEN_W) {
      if (!fx) {
        const uint8_t* s = srow + sx0;
        if (key < 0) idxRow(drow, s, pal, w);
        else idxRowKey(drow, s, pal, w, key);
      } else {
        const uint8_t* s = srow + (sw - 1 - sx0);  // walks backwards
        if (key < 0) {
          for (int i = 0; i < w; ++i) drow[i] = pal[s[-i]];
        } else {
          for (int i = 0; i < w; ++i) {
            int c = s[-i];
            if (c != key) drow[i] = pal[c];
          }
        }
      }
    }
    if (markDirty) _dirty.add({(int16_t)x0, (int16_t)y0, (int16_t)w, (int16_t)(y1 - y0)});
  }

  // Copies a w x h image (panel byte order) into the framebuffer at x,y, honoring the clip rectangle.
  // Optional transparent key and flips. Much faster than LovyanGFX's generic path for small sprites.
  // Runs from RAM (IRAM_ATTR) so its speed doesn't depend on the flash cache; one tight loop per case.
  IRAM_ATTR void blit(const uint16_t* src, int sw, int sh, int x, int y, bool useKey, uint16_t key, bool fx,
                      bool fy, bool markDirty) {
    int x0 = max(x, (int)_clip.x), y0 = max(y, (int)_clip.y);
    int x1 = min(x + sw, _clip.x + _clip.w), y1 = min(y + sh, _clip.y + _clip.h);
    if (x1 <= x0 || y1 <= y0) return;
    const int w = x1 - x0, sx0 = x0 - x;
    const int stepS = fy ? -sw : sw;
    const uint16_t* srow = src + (fy ? sh - 1 - (y0 - y) : (y0 - y)) * sw;
    uint16_t* drow = (uint16_t*)_fb->getBuffer() + y0 * SD_SCREEN_W + x0;
    for (int row = y0; row < y1; ++row, srow += stepS, drow += SD_SCREEN_W) {
      if (!fx) {
        const uint16_t* s = srow + sx0;
        if (!useKey) memcpy(drow, s, w * 2);
        else row565Key(drow, s, w, key);
      } else {
        const uint16_t* s = srow + (sw - 1 - sx0);  // walks backwards
        if (!useKey) {
          for (int i = 0; i < w; ++i) drow[i] = s[-i];
        } else {
          for (int i = 0; i < w; ++i) {
            uint16_t c = s[-i];
            if (c != key) drow[i] = c;
          }
        }
      }
    }
    if (markDirty) _dirty.add({(int16_t)x0, (int16_t)y0, (int16_t)w, (int16_t)(y1 - y0)});
  }

  static int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
  static int mod(int a, int b) {
    int r = a % b;
    return r < 0 ? r + b : r;
  }

  // DRAW_TILEMAP: fills the screen rectangle rx,ry,rw,rh with the map's tiles, the map pixel (sx, sy) landing
  // at the rectangle's top-left. Outside the map nothing is drawn, unless wrap repeats it.
  uint8_t drawTilemap(const Asset* m, const Asset* ts, int sx, int sy, int rx, int ry, int rw, int rh, uint8_t fl) {
    int x0 = max(rx, (int)_clip.x), y0 = max(ry, (int)_clip.y);
    int x1 = min(rx + rw, _clip.x + _clip.w), y1 = min(ry + rh, _clip.y + _clip.h);
    if (x1 <= x0 || y1 <= y0) return SD_ERR_NONE;
    const int tw = ts->fw, th = ts->fh;
    const bool wrap = fl & SD_TILEMAP_WRAP;
    // map pixel at screen (x0, y0)
    int wx = sx + (x0 - rx), wy = sy + (y0 - ry);
    const DRect saved = _clip;
    _clip = {(int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)};
    const uint8_t tileFlags = fl & SD_BLIT_TRANSPARENT;
    int tx0 = floorDiv(wx, tw), ty0 = floorDiv(wy, th);
    int px0 = x0 - (wx - tx0 * tw), py = y0 - (wy - ty0 * th);
    const uint8_t* map = m->frame(_assets->pool(), 0);
    for (int ty = ty0; py < y1; ++ty, py += th) {
      int my = wrap ? mod(ty, m->h) : ty;
      if (my < 0 || my >= m->h) continue;
      const uint8_t* row = map + my * m->w;
      int px = px0;
      for (int tx = tx0; px < x1; ++tx, px += tw) {
        int mx = wrap ? mod(tx, m->w) : tx;
        if (mx < 0 || mx >= m->w) continue;
        uint8_t t = row[mx];
        if (t == SD_TILE_EMPTY || t >= ts->frames) continue;
        drawFrame(ts, t, px, py, tileFlags, false);
      }
    }
    _clip = saved;
    _dirty.add({(int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)});
    return SD_ERR_NONE;
  }

  bool setFont(uint8_t id) {
    static const lgfx::IFont* const fonts[] = {&fonts::Font0, &fonts::Font2, &fonts::Font4, &fonts::Font7,
                                                &fonts::FreeSans9pt7b, &fonts::FreeSansBold12pt7b};
    if (id >= sizeof fonts / sizeof fonts[0]) return false;
    _fb->setFont(fonts[id]);
    return true;
  }

  // Copies a payload string into a NUL-terminated buffer.
  const char* str(const uint8_t* p, uint16_t n) {
    if (n > SD_MAX_PAYLOAD) n = SD_MAX_PAYLOAD;
    memcpy(_str, p, n);
    _str[n] = 0;
    return _str;
  }

  uint8_t run(uint8_t cmd, const uint8_t* p, uint16_t n) {
    auto need = [&](uint16_t len) { return n >= len; };
    switch (cmd) {
      // ---- drawing state
      case SD_SET_CLIP: {
        if (!need(8)) return SD_ERR_BAD_LEN;
        int x = i16(p), y = i16(p + 2), w = i16(p + 4), h = i16(p + 6);
        int x0 = max(x, 0), y0 = max(y, 0), x1 = min(x + w, (int)SD_SCREEN_W), y1 = min(y + h, (int)SD_SCREEN_H);
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
        _clip = {(int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)};
        _fb->setClipRect(_clip.x, _clip.y, _clip.w, _clip.h);
        return SD_ERR_NONE;
      }
      case SD_CLEAR_CLIP:
        _clip = {0, 0, SD_SCREEN_W, SD_SCREEN_H};
        _fb->clearClipRect();
        return SD_ERR_NONE;
      case SD_SET_TEXT_STYLE: {
        if (!need(8)) return SD_ERR_BAD_LEN;
        if (!setFont(p[0])) return SD_ERR_BAD_ARG;
        _fb->setTextSize(p[1] ? p[1] : 1);
        _fg = sd_rd_u16(p + 2);
        _bg = sd_rd_u16(p + 4);
        _drawBg = p[6] & SD_TEXT_DRAW_BG;
        if (_drawBg) _fb->setTextColor(_fg, _bg);
        else _fb->setTextColor(_fg);
        _fb->setTextDatum(p[7]);
        return SD_ERR_NONE;
      }
      case SD_SET_CURSOR:
        if (!need(4)) return SD_ERR_BAD_LEN;
        _fb->setCursor(i16(p), i16(p + 2));
        return SD_ERR_NONE;

      // ---- primitives
      case SD_FILL_SCREEN:
        if (!need(2)) return SD_ERR_BAD_LEN;
        _fb->fillScreen(sd_rd_u16(p));  // respects the clip rectangle
        mark(0, 0, SD_SCREEN_W, SD_SCREEN_H);
        return SD_ERR_NONE;
      case SD_FILL_RECT:
      case SD_DRAW_RECT: {
        if (!need(10)) return SD_ERR_BAD_LEN;
        int x = i16(p), y = i16(p + 2), w = i16(p + 4), h = i16(p + 6);
        uint16_t c = sd_rd_u16(p + 8);
        if (cmd == SD_FILL_RECT) _fb->fillRect(x, y, w, h, c);
        else _fb->drawRect(x, y, w, h, c);
        mark(x, y, w, h);
        return SD_ERR_NONE;
      }
      case SD_DRAW_LINE: {
        if (!need(10)) return SD_ERR_BAD_LEN;
        int16_t xs[2] = {i16(p), i16(p + 4)}, ys[2] = {i16(p + 2), i16(p + 6)};
        _fb->drawLine(xs[0], ys[0], xs[1], ys[1], sd_rd_u16(p + 8));
        markPoints(xs, ys, 2);
        return SD_ERR_NONE;
      }
      case SD_DRAW_HLINE:
      case SD_DRAW_VLINE: {
        if (!need(8)) return SD_ERR_BAD_LEN;
        int x = i16(p), y = i16(p + 2), len = i16(p + 4);
        uint16_t c = sd_rd_u16(p + 6);
        if (cmd == SD_DRAW_HLINE) { _fb->drawFastHLine(x, y, len, c); mark(x, y, len, 1); }
        else { _fb->drawFastVLine(x, y, len, c); mark(x, y, 1, len); }
        return SD_ERR_NONE;
      }
      case SD_DRAW_CIRCLE:
      case SD_FILL_CIRCLE: {
        if (!need(8)) return SD_ERR_BAD_LEN;
        int x = i16(p), y = i16(p + 2), r = i16(p + 4);
        uint16_t c = sd_rd_u16(p + 6);
        if (cmd == SD_FILL_CIRCLE) _fb->fillCircle(x, y, r, c);
        else _fb->drawCircle(x, y, r, c);
        mark(x - r, y - r, 2 * r + 1, 2 * r + 1);
        return SD_ERR_NONE;
      }
      case SD_DRAW_RRECT:
      case SD_FILL_RRECT: {
        if (!need(12)) return SD_ERR_BAD_LEN;
        int x = i16(p), y = i16(p + 2), w = i16(p + 4), h = i16(p + 6), r = i16(p + 8);
        uint16_t c = sd_rd_u16(p + 10);
        if (cmd == SD_FILL_RRECT) _fb->fillRoundRect(x, y, w, h, r, c);
        else _fb->drawRoundRect(x, y, w, h, r, c);
        mark(x, y, w, h);
        return SD_ERR_NONE;
      }
      case SD_DRAW_TRIANGLE:
      case SD_FILL_TRIANGLE: {
        if (!need(14)) return SD_ERR_BAD_LEN;
        int16_t xs[3] = {i16(p), i16(p + 4), i16(p + 8)}, ys[3] = {i16(p + 2), i16(p + 6), i16(p + 10)};
        uint16_t c = sd_rd_u16(p + 12);
        if (cmd == SD_FILL_TRIANGLE) _fb->fillTriangle(xs[0], ys[0], xs[1], ys[1], xs[2], ys[2], c);
        else _fb->drawTriangle(xs[0], ys[0], xs[1], ys[1], xs[2], ys[2], c);
        markPoints(xs, ys, 3);
        return SD_ERR_NONE;
      }
      case SD_DRAW_PIXELS: {
        if (n < 2 || (n - 2) % 4) return SD_ERR_BAD_LEN;
        uint16_t c = sd_rd_u16(p);
        int count = (n - 2) / 4;
        int x0 = INT16_MAX, y0 = INT16_MAX, x1 = INT16_MIN, y1 = INT16_MIN;
        for (int i = 0; i < count; ++i) {
          int x = i16(p + 2 + i * 4), y = i16(p + 4 + i * 4);
          _fb->drawPixel(x, y, c);
          x0 = min(x0, x); x1 = max(x1, x); y0 = min(y0, y); y1 = max(y1, y);
        }
        if (count) mark(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
        return SD_ERR_NONE;
      }

      // ---- text
      case SD_DRAW_TEXT: {
        if (!need(4)) return SD_ERR_BAD_LEN;
        int x = i16(p), y = i16(p + 2);
        const char* s = str(p + 4, n - 4);
        // Text box from the text size and alignment (datum)
        int w = _fb->textWidth(s), h = _fb->fontHeight();
        uint8_t d = _fb->getTextDatum();
        int bx = x - ((d & 3) == 1 ? w / 2 : (d & 3) == 2 ? w : 0);
        int by, bh = h;
        bool exactBox = true;
        switch (d & 0x1C) {
          case 0x04: by = y - h / 2; break;      // middle
          case 0x08: by = y - h; break;          // bottom
          case 0x10: by = y - h; bh = 2 * h; exactBox = false; break;  // baseline: conservative
          default: by = y; break;                // top
        }
        if (_drawBg && exactBox) {
          // Fast path: LovyanGFX draws a text background pixel by pixel. One fillRect for the box, then
          // transparent glyphs, gives the same result several times faster.
          _fb->fillRect(bx, by, w, h, _bg);
          _fb->setTextColor(_fg);
          _fb->drawString(s, x, y);
          _fb->setTextColor(_fg, _bg);
        } else {
          _fb->drawString(s, x, y);
        }
        mark(bx - 2, by - 2, w + 4, bh + 4);  // small margin for glyph overhangs
        return SD_ERR_NONE;
      }
      case SD_PRINT: {
        const char* s = str(p, n);
        uint8_t d = _fb->getTextDatum();
        _fb->setTextDatum(0);  // console text always grows from the cursor's top-left
        int x0 = _fb->getCursorX(), y0 = _fb->getCursorY();
        _fb->print(s);
        int x1 = _fb->getCursorX(), y1 = _fb->getCursorY(), h = _fb->fontHeight();
        _fb->setTextDatum(d);
        if (y1 == y0) mark(x0 - 2, y0 - 2, x1 - x0 + 4, h + 4);
        else mark(0, y0 - 2, SD_SCREEN_W, y1 - y0 + h + 4);  // wrapped or new lines: full-width band
        return SD_ERR_NONE;
      }

      // ---- sprites
      case SD_BLIT: {
        // u8 id, u16 frame, i16 x, y, u8 flags
        if (!need(8)) return SD_ERR_BAD_LEN;
        const Asset* a = _assets->get(p[0]);
        if (!a) return SD_ERR_BAD_ASSET;
        return drawFrame(a, sd_rd_u16(p + 1), i16(p + 3), i16(p + 5), p[7]);
      }
      case SD_BLIT_BATCH: {
        // n x BLIT payload. All entries are drawn; the first error (if any) is reported.
        if (!n || n % 8) return SD_ERR_BAD_LEN;
        uint8_t err = SD_ERR_NONE;
        for (const uint8_t* e = p; e < p + n; e += 8) {
          const Asset* a = _assets->get(e[0]);
          uint8_t r = a ? drawFrame(a, sd_rd_u16(e + 1), i16(e + 3), i16(e + 5), e[7]) : SD_ERR_BAD_ASSET;
          if (r && !err) err = r;
        }
        return err;
      }
      case SD_DRAW_TILEMAP: {
        // u8 map, u8 tileset, i16 scroll_x, scroll_y, i16 x, y, w, h (screen rectangle), u8 flags
        if (!need(15)) return SD_ERR_BAD_LEN;
        const Asset* m = _assets->get(p[0]);
        const Asset* ts = _assets->get(p[1]);
        if (!m || m->fmt != SD_FMT_TILEMAP || !ts || ts->fmt == SD_FMT_TILEMAP) return SD_ERR_BAD_ASSET;
        return drawTilemap(m, ts, i16(p + 2), i16(p + 4), i16(p + 6), i16(p + 8), i16(p + 10), i16(p + 12), p[14]);
      }
      case SD_BLIT_EX: {
        // u8 id, u16 frame, i16 cx, cy (where the frame's center lands), i16 angle (0.1 deg),
        // i16 scale_x, scale_y (256 = 1.0, negative = flip), u8 flags
        if (!need(14)) return SD_ERR_BAD_LEN;
        const Asset* a = _assets->get(p[0]);
        if (!a) return SD_ERR_BAD_ASSET;
        uint16_t f = sd_rd_u16(p + 1);
        if (f >= a->frames) return SD_ERR_BAD_ARG;
        int cx = i16(p + 3), cy = i16(p + 5);
        float ang = i16(p + 7) / 10.0f, zx = i16(p + 9) / 256.0f, zy = i16(p + 11) / 256.0f;
        if (zx == 0 || zy == 0) return SD_ERR_NONE;
        if (a->fmt == SD_FMT_TILEMAP) return SD_ERR_BAD_ASSET;
        const uint8_t* pool = _assets->pool();
        const float fcx = cx, fcy = cy, hx = a->fw / 2.0f, hy = a->fh / 2.0f;
        bool key = p[13] & SD_BLIT_TRANSPARENT;
        if (a->fmt == SD_FMT_INDEXED) {
          const lgfx::swap565_t* pal = (const lgfx::swap565_t*)a->palette(pool);
          const uint8_t* d = a->frame(pool, f);
          if (key && a->key < 256)
            _fb->pushImageRotateZoom(fcx, fcy, hx, hy, ang, zx, zy, a->fw, a->fh, d, (uint32_t)a->key,
                                     lgfx::palette_8bit, pal);
          else
            _fb->pushImageRotateZoom(fcx, fcy, hx, hy, ang, zx, zy, a->fw, a->fh, d, lgfx::palette_8bit, pal);
        } else {
          const uint16_t* d = (const uint16_t*)a->frame(pool, f);
          if (key)
            _fb->pushImageRotateZoom(fcx, fcy, hx, hy, ang, zx, zy, a->fw, a->fh, d, (uint16_t)a->key);
          else
            _fb->pushImageRotateZoom(fcx, fcy, hx, hy, ang, zx, zy, a->fw, a->fh, d);
        }
        // Bounding box of the rotated, scaled frame
        float r = ang * (float)PI / 180.0f, c = fabsf(cosf(r)), s = fabsf(sinf(r));
        float hw = a->fw * fabsf(zx) / 2, hh = a->fh * fabsf(zy) / 2;
        int ex = (int)(hw * c + hh * s) + 2, ey = (int)(hw * s + hh * c) + 2;
        mark(cx - ex, cy - ey, 2 * ex + 1, 2 * ey + 1);
        return SD_ERR_NONE;
      }
      case SD_BLIT_INLINE: {
        // i16 x, y, u16 w, h, u8 format, pixels (RGB565 little-endian, row-major)
        if (!need(9)) return SD_ERR_BAD_LEN;
        int x = i16(p), y = i16(p + 2), w = sd_rd_u16(p + 4), h = sd_rd_u16(p + 6);
        if (p[8] != SD_FMT_RGB565) return SD_ERR_BAD_ARG;
        if ((uint32_t)n != 9u + (uint32_t)w * h * 2) return SD_ERR_BAD_LEN;
        const uint8_t* px = p + 9;
        int x0 = max(x, (int)_clip.x), y0 = max(y, (int)_clip.y);
        int x1 = min(x + w, _clip.x + _clip.w), y1 = min(y + h, _clip.y + _clip.h);
        if (x1 <= x0 || y1 <= y0) return SD_ERR_NONE;
        uint8_t* fbuf = (uint8_t*)_fb->getBuffer();
        for (int dy = y0; dy < y1; ++dy) {
          const uint8_t* s = px + ((dy - y) * w + (x0 - x)) * 2;
          uint8_t* d = fbuf + (dy * SD_SCREEN_W + x0) * 2;
          for (int i = 0; i < x1 - x0; ++i, s += 2, d += 2) {
            d[0] = s[1];  // little-endian -> panel byte order
            d[1] = s[0];
          }
        }
        _dirty.add({(int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)});
        return SD_ERR_NONE;
      }
      case SD_COPY_RECT: {
        // i16 src_x, src_y, w, h, dst_x, dst_y
        if (!need(12)) return SD_ERR_BAD_LEN;
        int sx = i16(p), sy = i16(p + 2), w = i16(p + 4), h = i16(p + 6), dx = i16(p + 8), dy = i16(p + 10);
        // Trim the area so both the source and the destination lie on the screen
        if (sx < 0) { w += sx; dx -= sx; sx = 0; }
        if (dx < 0) { w += dx; sx -= dx; dx = 0; }
        if (sy < 0) { h += sy; dy -= sy; sy = 0; }
        if (dy < 0) { h += dy; sy -= dy; dy = 0; }
        w = min(w, min(SD_SCREEN_W - sx, SD_SCREEN_W - dx));
        h = min(h, min(SD_SCREEN_H - sy, SD_SCREEN_H - dy));
        if (w <= 0 || h <= 0) return SD_ERR_NONE;
        _fb->copyRect(dx, dy, w, h, sx, sy);
        mark(dx, dy, w, h);
        return SD_ERR_NONE;
      }
      default:
        return SD_ERR_UNKNOWN_CMD;
    }
  }

  LGFX_ILI9341* _lcd = nullptr;
  LGFX_Sprite* _fb = nullptr;
  AssetStore* _assets = nullptr;
  DirtyList _dirty;
  DRect _clip = {0, 0, SD_SCREEN_W, SD_SCREEN_H};
  uint16_t _fg = TFT_WHITE, _bg = TFT_BLACK;
  bool _drawBg = false;
  char _str[SD_MAX_PAYLOAD + 1];
};
