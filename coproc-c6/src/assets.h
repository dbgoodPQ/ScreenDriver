// ScreenDriver C6 asset store (milestones 4 and 6).
// Images, sprite sheets and tilemaps live in one RAM pool, referenced by id (0..63). The pool never fragments:
// freeing an asset slides the ones above it down (they are only ever referenced by id).
//
// Formats (see docs/protocol.md, ASSET_BEGIN):
//   RGB565   2 bytes per pixel, stored in the framebuffer's own byte order (the "swap565" order the panel wants),
//            so blits are plain copies.
//   INDEXED  a palette of up to 256 colors (stored in panel byte order) followed by 1 byte per pixel.
//            The transparent color is a palette index.
//   TILEMAP  w x h tile numbers, 1 byte each, row-major. Drawn with a tileset (any image asset) by DRAW_TILEMAP.
// A sheet is stored frame after frame, each frame row-major: the host sends it already in that order.
#pragma once
#include <Arduino.h>
#include "esp_heap_caps.h"
#include "sd_protocol.h"

struct Asset {
  bool used = false;      // slot allocated (upload may still be in progress)
  bool complete = false;  // ASSET_END received with all bytes
  uint8_t fmt = SD_FMT_RGB565;
  uint16_t w = 0, h = 0, fw = 0, fh = 0;  // image and frame size (tilemap: map size in tiles)
  uint16_t key = 0;       // RGB565: transparent color, byte-swapped like the pixels. INDEXED: palette index (>255: none)
  uint16_t frames = 0;
  uint16_t palCount = 0;  // INDEXED: palette entries
  uint32_t offset = 0;    // byte offset in the pool
  uint32_t size = 0;      // bytes reserved (4-byte aligned)
  uint32_t received = 0;  // bytes received so far

  uint8_t bpp() const { return fmt == SD_FMT_RGB565 ? 2 : 1; }
  uint32_t palBytes() const { return (uint32_t)palCount * 2; }
  uint32_t dataBytes() const { return palBytes() + (uint32_t)w * h * bpp(); }
  // Start of frame f's pixels (RGB565: uint16_t values, INDEXED: palette indices; TILEMAP: the map, f = 0)
  const uint8_t* frame(const uint8_t* pool, uint16_t f) const {
    return pool + offset + palBytes() + (uint32_t)f * fw * fh * bpp();
  }
  const uint16_t* palette(const uint8_t* pool) const { return (const uint16_t*)(pool + offset); }
};

class AssetStore {
 public:
  // Reserves the pool: `want` bytes, or less if the heap can't spare it (keeping `reserve` bytes free).
  bool begin(uint32_t want, uint32_t reserve) {
    uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    uint32_t sz = want;
    if (largest < sz + reserve) sz = largest > reserve ? largest - reserve : 0;
    sz &= ~3u;
    if (sz < 4096) return false;
    _pool = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_8BIT);
    if (!_pool) return false;
    _cap = sz;
    return true;
  }

  uint32_t capacity() const { return _cap; }
  uint32_t used() const { return _top; }
  uint32_t freeBytes() const { return _cap - _top; }
  int count() const {
    int c = 0;
    for (auto& a : _a) c += a.used;
    return c;
  }
  const uint8_t* pool() const { return _pool; }

  // Returns the asset if it is uploaded and ready to draw, else nullptr.
  const Asset* get(uint8_t id) const {
    if (id >= SD_MAX_ASSETS || !_a[id].complete) return nullptr;
    return &_a[id];
  }

  // ASSET_BEGIN: u8 id, u8 format, u16 w, h, frame_w, frame_h, u16 transparent [, u16 palette_count (INDEXED)]
  uint8_t beginAsset(const uint8_t* p, uint16_t n) {
    if (n < 12) return SD_ERR_BAD_LEN;
    uint8_t id = p[0], fmt = p[1];
    uint16_t w = sd_rd_u16(p + 2), h = sd_rd_u16(p + 4), fw = sd_rd_u16(p + 6), fh = sd_rd_u16(p + 8);
    uint16_t key = sd_rd_u16(p + 10), pal = 0;
    if (id >= SD_MAX_ASSETS || fmt > SD_FMT_TILEMAP || !w || !h) return SD_ERR_BAD_ARG;
    if (fmt == SD_FMT_INDEXED) {
      if (n < 14) return SD_ERR_BAD_LEN;
      pal = sd_rd_u16(p + 12);
      if (!pal || pal > 256) return SD_ERR_BAD_ARG;
    }
    if (fmt == SD_FMT_TILEMAP) fw = fh = 0;
    if (!fw) fw = w;
    if (!fh) fh = h;
    if (fw > w || fh > h || w % fw || h % fh) return SD_ERR_BAD_ARG;
    freeAsset(id);  // re-uploading an id replaces it
    Asset a;
    a.used = true;
    a.fmt = fmt;
    a.w = w; a.h = h; a.fw = fw; a.fh = fh;
    a.palCount = pal;
    a.key = fmt == SD_FMT_RGB565 ? (uint16_t)((key >> 8) | (key << 8)) : key;
    a.frames = (w / fw) * (h / fh);
    a.size = (a.dataBytes() + 3) & ~3u;  // keep every asset 4-byte aligned
    if (a.size > freeBytes()) return SD_ERR_NO_MEM;
    a.offset = _top;
    _top += a.size;
    _a[id] = a;
    return SD_ERR_NONE;
  }

  // ASSET_DATA: u8 id, u32 offset, bytes. RGB565: pixels little-endian. INDEXED: the palette (u16 little-endian
  // each), then the pixel indices. TILEMAP: the tile numbers.
  uint8_t data(const uint8_t* p, uint16_t n) {
    if (n < 5) return SD_ERR_BAD_LEN;
    uint8_t id = p[0];
    uint32_t off = sd_rd_u32(p + 1);
    uint16_t len = n - 5;
    if (id >= SD_MAX_ASSETS || !_a[id].used || _a[id].complete) return SD_ERR_BAD_ASSET;
    Asset& a = _a[id];
    if (off + len > a.dataBytes()) return SD_ERR_BAD_ARG;
    uint8_t* dst = _pool + a.offset;
    const uint8_t* src = p + 5;
    // 16-bit colors (all RGB565 pixels, or the palette) go to panel byte order; index bytes are copied as is
    uint32_t swapEnd = a.fmt == SD_FMT_RGB565 ? a.dataBytes() : a.palBytes();
    uint16_t i = 0;
    for (; i < len && off + i < swapEnd; ++i) dst[(off + i) ^ 1] = src[i];
    if (i < len) memcpy(dst + off + i, src + i, len - i);
    a.received += len;
    return SD_ERR_NONE;
  }

  // ASSET_END: u8 id. Fails (and frees the asset) if bytes are missing.
  uint8_t endAsset(const uint8_t* p, uint16_t n) {
    if (n < 1) return SD_ERR_BAD_LEN;
    uint8_t id = p[0];
    if (id >= SD_MAX_ASSETS || !_a[id].used) return SD_ERR_BAD_ASSET;
    Asset& a = _a[id];
    if (a.received != a.dataBytes()) {
      freeAsset(id);
      return SD_ERR_BAD_ASSET;
    }
    a.complete = true;
    return SD_ERR_NONE;
  }

  // TILE_SET: u8 map, u16 tx, ty, tile bytes (written along the row, continuing on the next rows)
  uint8_t setTiles(const uint8_t* p, uint16_t n) {
    if (n < 6) return SD_ERR_BAD_LEN;
    uint8_t id = p[0];
    if (id >= SD_MAX_ASSETS || !_a[id].complete || _a[id].fmt != SD_FMT_TILEMAP) return SD_ERR_BAD_ASSET;
    const Asset& a = _a[id];
    uint16_t tx = sd_rd_u16(p + 1), ty = sd_rd_u16(p + 3);
    uint32_t start = (uint32_t)ty * a.w + tx, count = n - 5;
    if (tx >= a.w || ty >= a.h || start + count > (uint32_t)a.w * a.h) return SD_ERR_BAD_ARG;
    memcpy(_pool + a.offset + start, p + 5, count);
    return SD_ERR_NONE;
  }

  // ASSET_FREE: u8 id, 0xFF = all
  uint8_t freeCmd(const uint8_t* p, uint16_t n) {
    if (n < 1) return SD_ERR_BAD_LEN;
    if (p[0] == 0xFF) {
      freeAll();
      return SD_ERR_NONE;
    }
    if (p[0] >= SD_MAX_ASSETS) return SD_ERR_BAD_ARG;
    freeAsset(p[0]);
    return SD_ERR_NONE;
  }

  void freeAll() {
    for (auto& a : _a) a = Asset();
    _top = 0;
  }

 private:
  // Frees one asset and slides everything stored above it down.
  void freeAsset(uint8_t id) {
    Asset& a = _a[id];
    if (!a.used) return;
    uint32_t start = a.offset, sz = a.size;
    memmove(_pool + start, _pool + start + sz, _top - start - sz);
    for (auto& b : _a)
      if (b.used && b.offset > start) b.offset -= sz;
    _top -= sz;
    a = Asset();
  }

  uint8_t* _pool = nullptr;
  uint32_t _cap = 0, _top = 0;
  Asset _a[SD_MAX_ASSETS];
};
