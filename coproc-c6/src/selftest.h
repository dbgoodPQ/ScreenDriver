// C6 rendering self-test (build target "selftest", flag SD_SELFTEST). Times the renderer's own paths with no link involved and
// logs the result to USB every 5 s, so the numbers can be compared between firmware variants.
#pragma once
#include <Arduino.h>
#include "renderer.h"

struct SelfTestResult {
  bool done = false;
  uint32_t cpuMhz = 0, fillUs = 0, memsetUs = 0, textUs = 0, blitUs = 0, blitPlainUs = 0;
  uintptr_t fbAddr = 0, poolAddr = 0;
};

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
      uint8_t f = 63;
      assets.freeCmd(&f, 1);
    }
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
}
