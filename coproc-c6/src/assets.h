// ScreenDriver C6 asset store (milestone 4).
// Images and sprite sheets live in one RAM pool, referenced by id (0..63). The pool never fragments:
// freeing an asset slides the ones above it down (they are only ever referenced by id).
//
// Pixels are stored in the framebuffer's own format (RGB565 big-endian, the "swap565" order the panel
// wants), so blits are plain copies. A sheet is stored frame after frame, each frame row-major: the host
// sends it already in that order (see docs/protocol.md, ASSET_DATA).
#pragma once
#include <Arduino.h>
#include "esp_heap_caps.h"
#include "sd_protocol.h"

struct Asset {
  bool used = false;      // slot allocated (upload may still be in progress)
  bool complete = false;  // ASSET_END received with all bytes
  uint16_t w = 0, h = 0, fw = 0, fh = 0;  // image and frame size
  uint16_t key = 0;       // transparent color, stored byte-swapped like the pixels
  uint16_t frames = 0;
  uint32_t offset = 0;    // byte offset in the pool
  uint32_t size = 0;      // bytes
  uint32_t received = 0;  // bytes received so far

  const uint16_t* frame(const uint8_t* pool, uint16_t f) const {
    return (const uint16_t*)(pool + offset + (uint32_t)f * fw * fh * 2);
  }
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

  // ASSET_BEGIN: u8 id, u8 format, u16 w, h, frame_w, frame_h, u16 transparent
  uint8_t beginAsset(const uint8_t* p, uint16_t n) {
    if (n < 12) return SD_ERR_BAD_LEN;
    uint8_t id = p[0], fmt = p[1];
    uint16_t w = sd_rd_u16(p + 2), h = sd_rd_u16(p + 4), fw = sd_rd_u16(p + 6), fh = sd_rd_u16(p + 8);
    uint16_t key = sd_rd_u16(p + 10);
    if (id >= SD_MAX_ASSETS || fmt != SD_FMT_RGB565 || !w || !h) return SD_ERR_BAD_ARG;
    if (!fw) fw = w;
    if (!fh) fh = h;
    if (fw > w || fh > h || w % fw || h % fh) return SD_ERR_BAD_ARG;
    freeAsset(id);  // re-uploading an id replaces it
    uint32_t size = ((uint32_t)w * h * 2 + 3) & ~3u;  // keep every asset 4-byte aligned
    if (size > freeBytes()) return SD_ERR_NO_MEM;
    Asset& a = _a[id];
    a = Asset();
    a.used = true;
    a.w = w; a.h = h; a.fw = fw; a.fh = fh;
    a.key = (uint16_t)((key >> 8) | (key << 8));
    a.frames = (w / fw) * (h / fh);
    a.offset = _top;
    a.size = size;
    _top += size;
    return SD_ERR_NONE;
  }

  // ASSET_DATA: u8 id, u32 offset, bytes (RGB565 little-endian, in frame order)
  uint8_t data(const uint8_t* p, uint16_t n) {
    if (n < 5) return SD_ERR_BAD_LEN;
    uint8_t id = p[0];
    uint32_t off = sd_rd_u32(p + 1);
    uint16_t len = n - 5;
    if (id >= SD_MAX_ASSETS || !_a[id].used || _a[id].complete) return SD_ERR_BAD_ASSET;
    Asset& a = _a[id];
    uint32_t pixBytes = (uint32_t)a.w * a.h * 2;
    if (off + len > pixBytes) return SD_ERR_BAD_ARG;
    uint8_t* dst = _pool + a.offset;
    const uint8_t* src = p + 5;
    for (uint16_t i = 0; i < len; ++i) dst[(off + i) ^ 1] = src[i];  // little-endian -> panel byte order
    a.received += len;
    return SD_ERR_NONE;
  }

  // ASSET_END: u8 id. Fails (and frees the asset) if bytes are missing.
  uint8_t endAsset(const uint8_t* p, uint16_t n) {
    if (n < 1) return SD_ERR_BAD_LEN;
    uint8_t id = p[0];
    if (id >= SD_MAX_ASSETS || !_a[id].used) return SD_ERR_BAD_ASSET;
    Asset& a = _a[id];
    if (a.received != (uint32_t)a.w * a.h * 2) {
      freeAsset(id);
      return SD_ERR_BAD_ASSET;
    }
    a.complete = true;
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
