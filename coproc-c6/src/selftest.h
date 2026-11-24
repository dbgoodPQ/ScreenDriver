// C6 rendering self-test (build target "selftest", flag SD_SELFTEST). Times the renderer's own paths with no link involved and
// logs the result to USB every 5 s, so the numbers can be compared between firmware variants.
#pragma once
#include <Arduino.h>
#include "renderer.h"

struct SelfTestResult {
  bool done = false;
  uint32_t cpuMhz = 0, fillUs = 0, memsetUs = 0, textUs = 0, blitUs = 0, blitPlainUs = 0;
  uint32_t idxUs = 0, idxPlainUs = 0, batchUs = 0, tile565Us = 0, tileIdxUs = 0;
  bool tilesMatch = false;
  uint32_t mIdxLong = 0, mIdxShort = 0, mIdxPlain = 0, mMemcpy = 0, mMemcpyOdd = 0;
  uintptr_t fbAddr = 0, poolAddr = 0;
};

// Uploads an asset through the same commands the link uses (header = ASSET_BEGIN payload)
static bool stUpload(AssetStore& as, const uint8_t* hdr, uint16_t hlen, const uint8_t* bytes, uint32_t n) {
  if (as.beginAsset(hdr, hlen) != SD_ERR_NONE) return false;
  static uint8_t d[5 + 1000];
  for (uint32_t off = 0; off < n; off += 1000) {
    uint16_t len = min<uint32_t>(1000, n - off);
    d[0] = hdr[0];
    sd_wr_u32(d + 1, off);
    memcpy(d + 5, bytes + off, len);
    if (as.data(d, 5 + len) != SD_ERR_NONE) return false;
  }
  return as.endAsset(hdr, 1) == SD_ERR_NONE;
}

static SelfTestResult selfTest(Renderer& r, LGFX_Sprite& fb, AssetStore& assets) {
  SelfTestResult t;
  t.cpuMhz = getCpuFrequencyMhz();
  t.fbAddr = (uintptr_t)fb.getBuffer();
  t.poolAddr = (uintptr_t)assets.pool();
  auto timeIt = [](int reps, auto fn) {
    uint32_t t0 = micros();
    for (int i = 0; i < reps; ++i) fn(i);
    return (micros() - t0) / reps;
  };
  uint8_t p[64];

  // FILL_SCREEN through the renderer
  t.fillUs = timeIt(10, [&](int i) {
    sd_wr_u16(p, i & 1 ? TFT_NAVY : TFT_BLACK);
    r.handle(SD_FILL_SCREEN, p, 2);
  });
  // Raw memory bandwidth on the framebuffer
  t.memsetUs = timeIt(10, [&](int i) { memset(fb.getBuffer(), i, SD_SCREEN_W * SD_SCREEN_H * 2); });

  // 12 lines of Font2 text with a background (same as the bench's "text bg")
  uint8_t style[8] = {SD_FONT_FONT2, 1, 0xFF, 0xFF, 0, 0, SD_TEXT_DRAW_BG, 0};
  r.handle(SD_SET_TEXT_STYLE, style, 8);
  t.textUs = timeIt(5, [&](int i) {
    for (int l = 0; l < 12; ++l) {
      char s[48];
      int n = snprintf(s, sizeof s, "line %2d  value %6d  status OK", l, i * 37 + l * 1013);
      sd_wr_i16(p, 4);
      sd_wr_i16(p + 2, 4 + l * 19);
      memcpy(p + 4, s, n);
      r.handle(SD_DRAW_TEXT, p, 4 + n);
    }
  });

  // 200 blits of a 16x16 sprite, with and without the transparent key
  if (assets.capacity()) {
    uint8_t b[12] = {63, SD_FMT_RGB565, 16, 0, 16, 0, 0, 0, 0, 0, 0x1F, 0xF8};
    if (assets.beginAsset(b, 12) == SD_ERR_NONE) {
      uint8_t d[5 + 512];
      d[0] = 63;
      sd_wr_u32(d + 1, 0);
      for (int i = 0; i < 256; ++i) sd_wr_u16(d + 5 + i * 2, (i % 16 + i / 16) % 5 ? 0x07E0 : 0xF81F);
      assets.data(d, sizeof d);
      uint8_t id = 63;
      assets.endAsset(&id, 1);
      for (int pass = 0; pass < 2; ++pass) {
        uint32_t us = timeIt(5, [&](int i) {
          for (int j = 0; j < 200; ++j) {
            uint8_t q[8] = {63, 0, 0};
            sd_wr_i16(q + 3, (j * 37 + i * 3) % 304);
            sd_wr_i16(q + 5, (j * 53 + i * 2) % 224);
            q[7] = pass == 0 ? SD_BLIT_TRANSPARENT : 0;
            r.handle(SD_BLIT, q, 8);
          }
        });
        (pass == 0 ? t.blitUs : t.blitPlainUs) = us;
      }
      // the same 200 sprites as BLIT_BATCH commands (127 + 73)
      t.batchUs = timeIt(5, [&](int i) {
        static uint8_t q[127 * 8];
        for (int j0 = 0; j0 < 200; j0 += 127) {
          int n = min(127, 200 - j0);
          for (int k = 0; k < n; ++k) {
            int j = j0 + k;
            uint8_t* e = q + k * 8;
            e[0] = 63; e[1] = e[2] = 0;
            sd_wr_i16(e + 3, (j * 37 + i * 3) % 304);
            sd_wr_i16(e + 5, (j * 53 + i * 2) % 224);
            e[7] = SD_BLIT_TRANSPARENT;
          }
          r.handle(SD_BLIT_BATCH, q, n * 8);
        }
      });
      uint8_t f = 63;
      assets.freeCmd(&f, 1);
    }

    // the same sprite as a 5-color palette image (index 1 = transparent)
    {
      static uint8_t px[10 + 256];
      const uint16_t pal[5] = {0x07E0, 0xF81F, 0x001F, 0xFFFF, 0xF800};
      for (int i = 0; i < 5; ++i) sd_wr_u16(px + i * 2, pal[i]);
      for (int i = 0; i < 256; ++i) px[10 + i] = (i % 16 + i / 16) % 5 ? (i % 3 == 0 ? 0 : 2 + i % 3) : 1;
      uint8_t h[14] = {62, SD_FMT_INDEXED, 16, 0, 16, 0, 0, 0, 0, 0, 1, 0, 5, 0};
      if (stUpload(assets, h, 14, px, sizeof px)) {
        for (int pass = 0; pass < 2; ++pass) {
          uint32_t us = timeIt(5, [&](int i) {
            for (int j = 0; j < 200; ++j) {
              uint8_t q[8] = {62, 0, 0};
              sd_wr_i16(q + 3, (j * 37 + i * 3) % 304);
              sd_wr_i16(q + 5, (j * 53 + i * 2) % 224);
              q[7] = pass == 0 ? SD_BLIT_TRANSPARENT : 0;
              r.handle(SD_BLIT, q, 8);
            }
          });
          (pass == 0 ? t.idxUs : t.idxPlainUs) = us;
        }
      }
      uint8_t f = 62;
      assets.freeCmd(&f, 1);
    }

    // full-screen tilemap (21 x 16 tiles visible: scrolled by 8,8), 8-tile tileset as RGB565 and as palette
    {
      static uint8_t ts565[128 * 16 * 2], tsIdx[16 + 128 * 16], map[20 * 15];
      const uint16_t pal[8] = {0x867D, 0x2589, 0x8A22, 0xB104, 0x7BEF, 0x1C9F, 0xFFFF, 0xFFE0};
      for (int i = 0; i < 8; ++i) sd_wr_u16(tsIdx + i * 2, pal[i]);
      for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 128; ++x) {
          uint8_t c = (x / 16 + (x ^ y) % 3) % 8;
          tsIdx[16 + y * 128 + x] = c;
          sd_wr_u16(ts565 + (y * 128 + x) * 2, pal[c]);
        }
      for (int i = 0; i < 20 * 15; ++i) map[i] = (i * 7 + i / 20) % 8;
      uint8_t h1[12] = {61, SD_FMT_RGB565, 128, 0, 16, 0, 16, 0, 16, 0, 0, 0};
      uint8_t h2[14] = {59, SD_FMT_INDEXED, 128, 0, 16, 0, 16, 0, 16, 0, 0, 1, 8, 0};  // key 256 = none
      uint8_t h3[12] = {60, SD_FMT_TILEMAP, 20, 0, 15, 0, 0, 0, 0, 0, 0, 0};
      if (stUpload(assets, h1, 12, ts565, sizeof ts565) && stUpload(assets, h2, 14, tsIdx, sizeof tsIdx) &&
          stUpload(assets, h3, 12, map, sizeof map)) {
        for (int pass = 0; pass < 2; ++pass) {
          uint32_t us = timeIt(5, [&](int i) {
            uint8_t q[15] = {60, (uint8_t)(pass == 0 ? 61 : 59)};
            sd_wr_i16(q + 2, 8 + i);
            sd_wr_i16(q + 4, 8);
            sd_wr_i16(q + 6, 0);
            sd_wr_i16(q + 8, 0);
            sd_wr_i16(q + 10, SD_SCREEN_W);
            sd_wr_i16(q + 12, SD_SCREEN_H);
            q[14] = SD_TILEMAP_WRAP;
            r.handle(SD_DRAW_TILEMAP, q, 15);
          });
          (pass == 0 ? t.tile565Us : t.tileIdxUs) = us;
        }
        // correctness: both tilesets hold the same colors and neither contains its transparent color, so the opaque
        // row copies (passes 0, 1) and the keyed loops forced by the transparent flag (passes 2, 3) must all give the
        // same framebuffer. Odd, even and negative scrolls exercise both alignments and the wrap arithmetic.
        t.tilesMatch = true;
        for (int sxs : {7, 8, -37}) {
          uint32_t sum[4];
          for (int pass = 0; pass < 4; ++pass) {
            uint8_t q[15] = {60, (uint8_t)(pass % 2 == 0 ? 61 : 59)};
            sd_wr_i16(q + 2, sxs);
            sd_wr_i16(q + 4, sxs < 0 ? -21 : 3);
            sd_wr_i16(q + 6, 1);
            sd_wr_i16(q + 8, 0);
            sd_wr_i16(q + 10, SD_SCREEN_W - 2);
            sd_wr_i16(q + 12, SD_SCREEN_H);
            q[14] = SD_TILEMAP_WRAP | (pass >= 2 ? SD_BLIT_TRANSPARENT : 0);
            memset(fb.getBuffer(), 0, SD_SCREEN_W * SD_SCREEN_H * 2);
            r.handle(SD_DRAW_TILEMAP, q, 15);
            const uint16_t* px = (const uint16_t*)fb.getBuffer();
            uint32_t h = 2166136261u;
            for (int k = 0; k < SD_SCREEN_W * SD_SCREEN_H; ++k) h = (h ^ px[k]) * 16777619u;
            sum[pass] = h;
          }
          if (sum[0] != sum[1] || sum[0] != sum[2] || sum[0] != sum[3]) t.tilesMatch = false;
        }
      }
      for (uint8_t f : {61, 59, 60}) assets.freeCmd(&f, 1);
    }
  }
  // Row copies alone on the framebuffer: 240 rows of 320 pixels (per-pixel cost), and the same pixels as 16-pixel
  // rows (per-call overhead), against the plain palette loop and memcpy. Sources live in a static buffer in RAM.
  {
    static uint8_t idx[320];
    static uint16_t pal[16], src[322];
    for (int i = 0; i < 320; ++i) idx[i] = i % 16;
    for (int i = 0; i < 16; ++i) pal[i] = i * 0x1111;
    for (int i = 0; i < 322; ++i) src[i] = i;
    uint16_t* fbp = (uint16_t*)fb.getBuffer();
    t.mIdxLong = timeIt(5, [&](int) { for (int y = 0; y < 240; ++y) Renderer::idxRow(fbp + y * 320, idx, pal, 320); });
    t.mIdxShort = timeIt(5, [&](int) {
      for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; x += 16) Renderer::idxRow(fbp + y * 320 + x, idx + x, pal, 16);
    });
    t.mIdxPlain = timeIt(5, [&](int) {
      for (int y = 0; y < 240; ++y) {
        uint16_t* d = fbp + y * 320;
        for (int i = 0; i < 320; ++i) d[i] = pal[idx[i]];
      }
    });
    t.mMemcpy = timeIt(5, [&](int) { for (int y = 0; y < 240; ++y) memcpy(fbp + y * 320, src, 640); });
    t.mMemcpyOdd = timeIt(5, [&](int) { for (int y = 0; y < 240; ++y) memcpy(fbp + y * 320, src + 1, 640); });
  }
  r.flush(true);
  r.renderUs = 0;
  t.done = true;
  return t;
}

static void logSelfTest(const SelfTestResult& t) {
  Serial.printf("[selftest] cpu %lu MHz | fillScreen %lu us | memset fb %lu us | 12 text lines %lu us | "
                "200 blits key %lu us, plain %lu us | fb @%08lx pool @%08lx\n",
                (unsigned long)t.cpuMhz, (unsigned long)t.fillUs, (unsigned long)t.memsetUs, (unsigned long)t.textUs,
                (unsigned long)t.blitUs, (unsigned long)t.blitPlainUs, (unsigned long)t.fbAddr,
                (unsigned long)t.poolAddr);
  Serial.printf("[selftest] 200 blits: 565 key %lu / plain %lu | palette key %lu / plain %lu | batched 565 key %lu us\n",
                (unsigned long)t.blitUs, (unsigned long)t.blitPlainUs, (unsigned long)t.idxUs,
                (unsigned long)t.idxPlainUs, (unsigned long)t.batchUs);
  Serial.printf("[selftest] full-screen tilemap: 565 tileset %lu us | palette tileset %lu us | same pixels: %s\n",
                (unsigned long)t.tile565Us, (unsigned long)t.tileIdxUs, t.tilesMatch ? "yes" : "NO");
  Serial.printf("[selftest] rows, full screen: idxRow 320px %lu / 16px %lu / plain loop %lu | memcpy %lu / odd %lu us\n",
                (unsigned long)t.mIdxLong, (unsigned long)t.mIdxShort, (unsigned long)t.mIdxPlain,
                (unsigned long)t.mMemcpy, (unsigned long)t.mMemcpyOdd);
}
