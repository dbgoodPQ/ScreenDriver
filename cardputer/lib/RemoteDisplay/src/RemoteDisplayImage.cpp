// RemoteDisplay: loading PNG/BMP images from the Cardputer-ADV's SD card.
#include "RemoteDisplay.h"

#if defined(RD_NO_FILE_IMAGES)
// Apps without M5GFX (or with their own SD setup) build with -DRD_NO_FILE_IMAGES: images come
// only from RAM (uploadImage), and loadImage/loadSheet fail.
bool RemoteDisplay::mountSD() { return false; }
bool RemoteDisplay::loadFile(uint8_t, const char*, uint16_t, uint16_t, uint16_t, uint8_t) {
  snprintf(_errBuf, sizeof _errBuf, "image files disabled (RD_NO_FILE_IMAGES)");
  return false;
}
#else
// SD.h must come before M5GFX.h so that M5GFX enables its file-based image decoders.
#include <SPI.h>
#include <SD.h>
#include <M5GFX.h>

// Cardputer-ADV microSD (SPI): not shared with the link pins
#define RD_SD_SCK  40
#define RD_SD_MISO 39
#define RD_SD_MOSI 14
#define RD_SD_CS   12

bool RemoteDisplay::mountSD() {
  if (SD.cardType() != CARD_NONE) return true;  // already mounted (by us or the app)
  SPI.begin(RD_SD_SCK, RD_SD_MISO, RD_SD_MOSI, RD_SD_CS);
  return SD.begin(RD_SD_CS, SPI, 25000000);
}

// Reads the image size from a PNG or BMP header.
static bool imageInfo(File& f, bool& png, int32_t& w, int32_t& h) {
  uint8_t hd[26];
  if (f.read(hd, sizeof hd) != sizeof hd) return false;
  f.seek(0);
  if (hd[0] == 0x89 && hd[1] == 'P' && hd[2] == 'N' && hd[3] == 'G') {
    png = true;
    w = (hd[16] << 24) | (hd[17] << 16) | (hd[18] << 8) | hd[19];
    h = (hd[20] << 24) | (hd[21] << 16) | (hd[22] << 8) | hd[23];
    return true;
  }
  if (hd[0] == 'B' && hd[1] == 'M') {
    png = false;
    w = (int32_t)sd_rd_u32(hd + 18);
    h = abs((int32_t)sd_rd_u32(hd + 22));  // negative height = top-down BMP
    return true;
  }
  return false;
}

bool RemoteDisplay::loadFile(uint8_t id, const char* path, uint16_t fw, uint16_t fh, uint16_t key, uint8_t format) {
  if (strlen(path) >= sizeof(_slots[0].path)) {
    _err = "path too long (63 characters max)";
    return false;
  }
  if (!mountSD()) {
    _err = "no SD card";
    return false;
  }
  File f = SD.open(path);
  if (!f) {
    snprintf(_errBuf, sizeof _errBuf, "file not found: %s", path);
    _err = _errBuf;
    return false;
  }
  bool png = false;
  int32_t w = 0, h = 0;
  bool known = imageInfo(f, png, w, h);
  f.close();
  if (!known) {
    _err = "not a PNG or BMP file";
    return false;
  }
  if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
    _err = "image size out of range";
    return false;
  }

  // Decode into a temporary sprite pre-filled with the transparent color: PNG alpha blends onto it, so
  // fully transparent pixels end up exactly that color.
  LGFX_Sprite spr;
  spr.setColorDepth(16);
  spr.setPsram(false);
  if (!spr.createSprite(w, h)) {
    snprintf(_errBuf, sizeof _errBuf, "not enough Cardputer RAM for %ldx%ld", (long)w, (long)h);
    _err = _errBuf;
    return false;
  }
  spr.fillScreen((uint16_t)key);
  bool ok = png ? spr.drawPngFile(SD, path, 0, 0) : spr.drawBmpFile(SD, path, 0, 0);
  if (!ok) {
    _err = "could not decode the image";
    return false;
  }
  // The sprite stores pixels byte-swapped (panel order): convert to plain RGB565 values for the upload.
  uint16_t* px = (uint16_t*)spr.getBuffer();
  for (int32_t i = 0; i < w * h; ++i) px[i] = __builtin_bswap16(px[i]);

  ok = sendAsset(id, px, w, h, fw, fh, key, format);
  spr.deleteSprite();
  if (!ok) return false;
  _slots[id].mem = nullptr;  // the pixels are gone: restore() reloads from the file
  strcpy(_slots[id].path, path);
  return true;
}
#endif  // RD_NO_FILE_IMAGES

bool RemoteDisplay::loadImage(uint8_t id, const char* path, uint16_t transparent) {
  return loadFile(id, path, 0, 0, transparent, _imageFormat);
}

bool RemoteDisplay::loadSheet(uint8_t id, const char* path, uint16_t frameW, uint16_t frameH, uint16_t transparent) {
  return loadFile(id, path, frameW, frameH, transparent, _imageFormat);
}
