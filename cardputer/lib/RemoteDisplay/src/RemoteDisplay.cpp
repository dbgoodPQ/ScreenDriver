#include "RemoteDisplay.h"
#include <stdarg.h>

// Packs int16 values little-endian into buf. Returns the end pointer.
static uint8_t* put16(uint8_t* w, int16_t v) { return sd_wr_u16(w, (uint16_t)v); }

bool RemoteDisplay::begin(uint8_t rotation, uint32_t timeoutMs) {
  if (!_linkUp) {
    if (!_link.begin()) {
      _err = "UART init failed";
      return false;
    }
    _linkUp = true;
  }
  _pxCount = 0;
  _styleDirty = true;
  _framesSent = 0;
  _link.framesDone = 0;
  _lastEndMs = 0;
  _stats = FrameStats();
  _connected = false;
  _link.txWaitMs = 200;

  // RESET -> HELLO. The C6 prints its ROM boot log on our RX line while it reboots: let it pass,
  // then flush our receiver before the real HELLO arrives.
  _link.helloSeen = false;
  _link.send(SD_RESET);
  _link.port().waitTxDone();
  uint32_t t0 = millis();
  while (millis() - t0 < 250 && !_link.helloSeen) _link.poll(10);
  if (!_link.helloSeen) {
    _link.flushInput();
    _link.waitFor(SD_HELLO, timeoutMs);
  }
  if (!_link.helloSeen) {
    _err = "no HELLO from the C6 (power, wiring or C6 firmware?)";
    return false;
  }
  if (_link.hello.proto != SD_PROTO_VERSION) {
    _err = "protocol version mismatch: update the C6 or Cardputer firmware";
    return false;
  }
  if (_link.hello.fw < RD_MIN_C6_FW) {
    snprintf(_errBuf, sizeof _errBuf, "C6 firmware %04X is too old: flash %04X or newer", _link.hello.fw,
             RD_MIN_C6_FW);
    _err = _errBuf;
    return false;
  }
  _helloBase = _link.helloCount;
  _rotation = rotation;
  // health tracking starts from here
  _seenDone = 0;
  _pendingSinceMs = 0;
  _forgiven = 0;
  _crcSeen = _link.crcNacks;
  _badAssetSeen = _link.badAssetNacks;
  _txDropsSeen = _link.txDrops;
  _connected = true;

  uint8_t init[2] = {(uint8_t)(rotation == 3 ? 3 : 1), SD_INIT_CLEAR};
  _link.send(SD_INIT, init, 2);
  _err = "";
  return true;
}

// ---- low level -----------------------------------------------------------------
void RemoteDisplay::cmd(uint8_t c, const uint8_t* p, uint16_t n) {
  if (!_connected) return;  // link down: drop drawing, the app keeps running
  if (_pxCount) flushPixels();
  _link.send(c, p, n);
}

void RemoteDisplay::flushPixels() {
  if (!_pxCount) return;
  if (!_connected) {
    _pxCount = 0;
    return;
  }
  sd_wr_u16(_px, _pxColor);
  uint16_t n = _pxCount;
  _pxCount = 0;  // before send(), so cmd() doesn't recurse
  _link.send(SD_DRAW_PIXELS, _px, 2 + n * 4);
}

void RemoteDisplay::poll() { checkHealth(); }

bool RemoteDisplay::wasReset() {
  checkHealth();
  bool r = _resetFlag;
  _resetFlag = false;
  return r;
}

// ---- health and recovery ---------------------------------------------------------
// Link down when: frames were sent but no FRAME_DONE came back for 500 ms, or the UART buffer stayed full
// (sends dropped). The C6 lost its state when: it said HELLO (rebooted) or refused to draw an asset we uploaded.
void RemoteDisplay::checkHealth() {
  if (!_linkUp || _inRecovery) return;
  while (_link.poll(0)) {
  }
  uint32_t now = millis();
  if (_link.framesDone != _seenDone) {
    _seenDone = _link.framesDone;
    _pendingSinceMs = 0;
  }
  if (_link.crcNacks != _crcSeen) {  // commands lost: part of the screen may be wrong
    _health.crcNacks += _link.crcNacks - _crcSeen;
    _crcSeen = _link.crcNacks;
    _redrawPending = true;
  }
  if (_connected) {
    bool rebooted = _link.helloCount != _helloBase;
    bool assetsLost = _link.badAssetNacks != _badAssetSeen;
    if (rebooted || assetsLost) {
      recover(rebooted);
      return;
    }
    bool stalled = _pendingSinceMs && now - _pendingSinceMs > 500;
    bool dropped = _link.txDrops != _txDropsSeen;
    if (stalled || dropped) goDown();
  }
  if (!_connected) probe(now);
}

void RemoteDisplay::goDown() {
  _connected = false;
  _health.linkDowns++;
  _downSinceMs = millis();
  _link.txWaitMs = 0;  // never wait for buffer space while down
  _pxCount = 0;
  _lastProbeMs = 0;
  _probeSeq = -1;
}

// While down: every 250 ms send a PING. A PONG (link back) or a HELLO (C6 rebooted) starts the recovery.
void RemoteDisplay::probe(uint32_t now) {
  if (_link.helloCount != _helloBase) {
    recover(true);
    return;
  }
  if (_probeSeq >= 0 && _link.pongCount != _pongSeen && _link.lastPongSeq == (uint8_t)_probeSeq) {
    recover(false);
    return;
  }
  if (now - _lastProbeMs >= 250) {
    _lastProbeMs = now;
    _pongSeen = _link.pongCount;
    uint8_t b = 0x5D;
    _probeSeq = _link.send(SD_PING, &b, 1);
    _link.flushTx();
  }
}

// Reset the C6, re-upload the images, then redraw at the next frame.
void RemoteDisplay::recover(bool rebooted) {
  uint32_t t0 = millis();
  _inRecovery = true;
  bool ok = restoreWithTimeout(1500);
  _inRecovery = false;
  if (ok) {
    _health.recoveries++;
    if (rebooted) _health.reboots++;
    _health.lastRecoveryMs = millis() - t0;
    if (_downSinceMs) _health.lastDownMs = t0 - _downSinceMs;
    _downSinceMs = 0;
    _redrawPending = true;
    _resetFlag = true;
  } else {
    if (_connected) _connected = false;
    if (!_downSinceMs) {
      _downSinceMs = t0;
      _health.linkDowns++;
    }
    _link.txWaitMs = 0;
    _helloBase = _link.helloCount;
    _lastProbeMs = millis();
    _probeSeq = -1;
  }
}

// ---- frames --------------------------------------------------------------------
void RemoteDisplay::startFrame() {
  checkHealth();
  cmd(SD_BEGIN_FRAME);
  if (_redrawPending && _connected) {
    _redrawPending = false;
    _fullNext = true;
    _health.redraws++;
    if (_onRedraw) _onRedraw();
  }
}

void RemoteDisplay::endFrame(bool fullFlush) {
  flushPixels();
  checkHealth();
  uint32_t now = millis();
  if (!_connected) {  // nothing to send; keep the app's frame rate measurable
    _stats.waitUs = 0;
  } else {
    // At most one frame in flight: wait (up to 100 ms) for the previous frame's FRAME_DONE.
    uint32_t t0 = micros();
    uint32_t start = millis();
    while (doneCount() < _framesSent && millis() - start < 100) _link.poll(5);
    if (doneCount() < _framesSent) _forgiven += _framesSent - doneCount();  // late or lost: don't wait again
    if (doneCount() > _framesSent && _forgiven) _forgiven -= min(_forgiven, doneCount() - _framesSent);
    _stats.waitUs = micros() - t0;
    _stats.frameNo = _link.lastFrameNo;
    _stats.renderUs = _link.lastRenderUs;
    _stats.flushUs = _link.lastFlushUs;

    uint8_t flags = (fullFlush || _fullNext) ? SD_END_FULL_FLUSH : 0;
    _fullNext = false;
    _link.send(SD_END_FRAME, &flags, 1);
    _link.flushTx();
    _framesSent++;
    if (!_pendingSinceMs) _pendingSinceMs = millis();
    checkHealth();  // notices dropped sends at once
    now = millis();
  }

  if (_lastEndMs) {
    float inst = 1000.0f / max<uint32_t>(1, now - _lastEndMs);
    _stats.fps = _stats.fps == 0 ? inst : _stats.fps * 0.9f + inst * 0.1f;
  }
  _lastEndMs = now;
}

// ---- primitives ----------------------------------------------------------------
void RemoteDisplay::fillScreen(uint16_t color) {
  uint8_t p[2];
  sd_wr_u16(p, color);
  cmd(SD_FILL_SCREEN, p, 2);
}

void RemoteDisplay::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  uint8_t p[10], *q = p;
  q = put16(q, x); q = put16(q, y); q = put16(q, w); q = put16(q, h); sd_wr_u16(q, color);
  cmd(SD_FILL_RECT, p, 10);
}

void RemoteDisplay::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  uint8_t p[10], *q = p;
  q = put16(q, x); q = put16(q, y); q = put16(q, w); q = put16(q, h); sd_wr_u16(q, color);
  cmd(SD_DRAW_RECT, p, 10);
}

void RemoteDisplay::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
  uint8_t p[10], *q = p;
  q = put16(q, x0); q = put16(q, y0); q = put16(q, x1); q = put16(q, y1); sd_wr_u16(q, color);
  cmd(SD_DRAW_LINE, p, 10);
}

void RemoteDisplay::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
  uint8_t p[8], *q = p;
  q = put16(q, x); q = put16(q, y); q = put16(q, w); sd_wr_u16(q, color);
  cmd(SD_DRAW_HLINE, p, 8);
}

void RemoteDisplay::drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
  uint8_t p[8], *q = p;
  q = put16(q, x); q = put16(q, y); q = put16(q, h); sd_wr_u16(q, color);
  cmd(SD_DRAW_VLINE, p, 8);
}

void RemoteDisplay::drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color) {
  uint8_t p[8], *q = p;
  q = put16(q, x); q = put16(q, y); q = put16(q, r); sd_wr_u16(q, color);
  cmd(SD_DRAW_CIRCLE, p, 8);
}

void RemoteDisplay::fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color) {
  uint8_t p[8], *q = p;
  q = put16(q, x); q = put16(q, y); q = put16(q, r); sd_wr_u16(q, color);
  cmd(SD_FILL_CIRCLE, p, 8);
}

void RemoteDisplay::drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
  uint8_t p[12], *q = p;
  q = put16(q, x); q = put16(q, y); q = put16(q, w); q = put16(q, h); q = put16(q, r); sd_wr_u16(q, color);
  cmd(SD_DRAW_RRECT, p, 12);
}

void RemoteDisplay::fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
  uint8_t p[12], *q = p;
  q = put16(q, x); q = put16(q, y); q = put16(q, w); q = put16(q, h); q = put16(q, r); sd_wr_u16(q, color);
  cmd(SD_FILL_RRECT, p, 12);
}

void RemoteDisplay::drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                                 uint16_t color) {
  uint8_t p[14], *q = p;
  q = put16(q, x0); q = put16(q, y0); q = put16(q, x1); q = put16(q, y1); q = put16(q, x2); q = put16(q, y2);
  sd_wr_u16(q, color);
  cmd(SD_DRAW_TRIANGLE, p, 14);
}

void RemoteDisplay::fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                                 uint16_t color) {
  uint8_t p[14], *q = p;
  q = put16(q, x0); q = put16(q, y0); q = put16(q, x1); q = put16(q, y1); q = put16(q, x2); q = put16(q, y2);
  sd_wr_u16(q, color);
  cmd(SD_FILL_TRIANGLE, p, 14);
}

void RemoteDisplay::drawPixel(int16_t x, int16_t y, uint16_t color) {
  if (_pxCount && (color != _pxColor || _pxCount == 255)) flushPixels();
  _pxColor = color;
  uint8_t* q = _px + 2 + _pxCount * 4;
  q = put16(q, x);
  put16(q, y);
  _pxCount++;
}

// ---- clipping ------------------------------------------------------------------
void RemoteDisplay::setClipRect(int16_t x, int16_t y, int16_t w, int16_t h) {
  uint8_t p[8], *q = p;
  q = put16(q, x); q = put16(q, y); q = put16(q, w); put16(q, h);
  cmd(SD_SET_CLIP, p, 8);
}

void RemoteDisplay::clearClipRect() { cmd(SD_CLEAR_CLIP); }

// ---- text ----------------------------------------------------------------------
void RemoteDisplay::setFont(uint8_t fontId) { _font = fontId; _styleDirty = true; }
void RemoteDisplay::setTextSize(uint8_t size) { _size = size ? size : 1; _styleDirty = true; }
void RemoteDisplay::setTextColor(uint16_t fg) { _fg = fg; _drawBg = false; _styleDirty = true; }
void RemoteDisplay::setTextColor(uint16_t fg, uint16_t bg) {
  _fg = fg;
  _bg = bg;
  _drawBg = true;
  _styleDirty = true;
}
void RemoteDisplay::setTextDatum(uint8_t datum) { _datum = datum; _styleDirty = true; }

void RemoteDisplay::sendStyleIfNeeded() {
  if (!_styleDirty) return;
  uint8_t p[8];
  p[0] = _font;
  p[1] = _size;
  sd_wr_u16(p + 2, _fg);
  sd_wr_u16(p + 4, _bg);
  p[6] = _drawBg ? SD_TEXT_DRAW_BG : 0;
  p[7] = _datum;
  cmd(SD_SET_TEXT_STYLE, p, 8);
  _styleDirty = false;
}

// Sends a text command (optional fixed prefix + string), splitting long strings across commands.
void RemoteDisplay::sendText(uint8_t c, const uint8_t* prefix, uint16_t prefixLen, const char* s) {
  sendStyleIfNeeded();
  static uint8_t buf[SD_MAX_PAYLOAD];
  size_t len = strlen(s);
  do {
    size_t chunk = min(len, (size_t)(SD_MAX_PAYLOAD - prefixLen));
    memcpy(buf, prefix, prefixLen);
    memcpy(buf + prefixLen, s, chunk);
    cmd(c, buf, prefixLen + chunk);
    s += chunk;
    len -= chunk;
  } while (len > 0 && c == SD_PRINT);  // drawString draws one line: extra text would overlap, so it is cut
}

void RemoteDisplay::drawString(const char* s, int16_t x, int16_t y) {
  uint8_t pre[4], *q = pre;
  q = put16(q, x);
  put16(q, y);
  sendText(SD_DRAW_TEXT, pre, 4, s);
}

void RemoteDisplay::setCursor(int16_t x, int16_t y) {
  uint8_t p[4], *q = p;
  q = put16(q, x);
  put16(q, y);
  cmd(SD_SET_CURSOR, p, 4);
}

void RemoteDisplay::print(const char* s) {
  if (*s) sendText(SD_PRINT, nullptr, 0, s);
}

void RemoteDisplay::println(const char* s) {
  print(s);
  print("\n");
}

void RemoteDisplay::printf(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  print(buf);
}

// ---- misc ----------------------------------------------------------------------
void RemoteDisplay::setBrightness(uint8_t level) { cmd(SD_SET_BACKLIGHT, &level, 1); }

bool RemoteDisplay::getStatus(SdStatusInfo& out, uint32_t timeoutMs) {
  if (!_connected) return false;
  cmd(SD_GET_STATUS);
  if (!_link.waitFor(SD_STATUS, timeoutMs)) return false;
  out = _link.status;
  return true;
}

// ---- assets (upload part; file loading is in RemoteDisplayImage.cpp) -------------
uint8_t RemoteDisplay::cmdSeq(uint8_t c, const uint8_t* p, uint16_t n) {
  if (_pxCount) flushPixels();
  return _link.send(c, p, n);
}

const char* RemoteDisplay::errName(uint8_t e) {
  switch (e) {
    case SD_ERR_CRC: return "CRC error";
    case SD_ERR_UNKNOWN_CMD: return "unknown command (old C6 firmware?)";
    case SD_ERR_BAD_LEN: return "bad length";
    case SD_ERR_NO_MEM: return "C6 asset pool full";
    case SD_ERR_BAD_ASSET: return "bad or incomplete asset";
    case SD_ERR_BAD_ARG: return "bad argument";
    default: return "error";
  }
}

// Waits for the ACK or NACK of the command sent with `seq`. Other replies are processed meanwhile.
bool RemoteDisplay::waitAck(uint8_t seq, uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (!_link.poll(5)) continue;
    const SdParser& r = _link.reply();
    if ((r.cmd == SD_ACK || r.cmd == SD_NACK) && r.payloadLen >= 2 && r.payload()[0] == seq) {
      if (r.cmd == SD_ACK) return true;
      snprintf(_errBuf, sizeof _errBuf, "C6 refused: %s", errName(r.payloadLen >= 3 ? r.payload()[2] : 0));
      _err = _errBuf;
      return false;
    }
  }
  _err = "no answer from the C6";
  return false;
}

// Uploads a row-major image/sheet. The C6 wants the frames one after another, so the rows are reordered here.
bool RemoteDisplay::sendAsset(uint8_t id, const uint16_t* px, uint16_t w, uint16_t h, uint16_t fw, uint16_t fh,
                              uint16_t key) {
  if (!_connected) {
    _err = "not connected to the C6";
    return false;
  }
  if (id >= SD_MAX_ASSETS) {
    _err = "asset id must be 0..63";
    return false;
  }
  if (!fw) fw = w;
  if (!fh) fh = h;
  if (!w || !h || fw > w || fh > h || w % fw || h % fh) {
    _err = "image size is not a multiple of the frame size";
    return false;
  }
  uint8_t b[12], *q = b;
  *q++ = id;
  *q++ = SD_FMT_RGB565;
  q = sd_wr_u16(q, w); q = sd_wr_u16(q, h); q = sd_wr_u16(q, fw); q = sd_wr_u16(q, fh); sd_wr_u16(q, key);
  if (!waitAck(cmdSeq(SD_ASSET_BEGIN, b, 12), 1000)) return false;

  static uint8_t chunk[SD_MAX_PAYLOAD];
  const uint16_t maxData = (SD_MAX_PAYLOAD - 5) & ~1;
  uint32_t start = 0;  // byte offset (in the asset) of the data in chunk
  uint16_t fill = 0;
  auto flush = [&]() {
    if (!fill) return;
    chunk[0] = id;
    sd_wr_u32(chunk + 1, start);
    _link.send(SD_ASSET_DATA, chunk, 5 + fill);
    start += fill;
    fill = 0;
  };
  const uint16_t cols = w / fw, frames = cols * (h / fh);
  for (uint16_t f = 0; f < frames; ++f) {
    const uint16_t* base = px + (uint32_t)(f / cols) * fh * w + (f % cols) * fw;
    for (uint16_t r = 0; r < fh; ++r) {
      const uint8_t* row = (const uint8_t*)(base + (uint32_t)r * w);  // RGB565 values: little-endian in memory
      uint32_t left = fw * 2;
      while (left) {
        uint16_t n = (uint16_t)min<uint32_t>(left, maxData - fill);
        memcpy(chunk + 5 + fill, row, n);
        fill += n; row += n; left -= n;
        if (fill == maxData) flush();
      }
    }
  }
  flush();
  if (!waitAck(cmdSeq(SD_ASSET_END, &id, 1), 3000)) return false;

  Slot& s = _slots[id];
  s.used = true;
  s.mem = px;
  s.path[0] = 0;
  s.w = w; s.h = h; s.fw = fw; s.fh = fh; s.key = key;
  _err = "";
  return true;
}

bool RemoteDisplay::uploadImage(uint8_t id, const uint16_t* pixels, uint16_t w, uint16_t h, uint16_t frameW,
                                uint16_t frameH, uint16_t transparent) {
  return sendAsset(id, pixels, w, h, frameW, frameH, transparent);
}

void RemoteDisplay::freeAsset(uint8_t id) {
  if (id >= SD_MAX_ASSETS) return;
  cmd(SD_ASSET_FREE, &id, 1);
  _slots[id] = Slot();
}

void RemoteDisplay::freeAllAssets() {
  uint8_t all = 0xFF;
  cmd(SD_ASSET_FREE, &all, 1);
  for (auto& s : _slots) s = Slot();
}

uint16_t RemoteDisplay::frameCount(uint8_t id) const {
  if (!hasAsset(id)) return 0;
  const Slot& s = _slots[id];
  return (s.w / s.fw) * (s.h / s.fh);
}

int32_t RemoteDisplay::assetPoolFree() {
  SdStatusInfo st;
  return getStatus(st) ? (int32_t)st.assetFree : -1;
}

bool RemoteDisplay::restore() { return restoreWithTimeout(4000); }

bool RemoteDisplay::restoreWithTimeout(uint32_t helloTimeoutMs) {
  if (!begin(_rotation, helloTimeoutMs)) return false;
  bool ok = true;
  for (int id = 0; id < SD_MAX_ASSETS; ++id) {
    Slot s = _slots[id];
    if (!s.used) continue;
    bool r = s.path[0] ? loadFile(id, s.path, s.fw, s.fh, s.key) : sendAsset(id, s.mem, s.w, s.h, s.fw, s.fh, s.key);
    ok = ok && r;
  }
  return ok;
}

// ---- sprite drawing ------------------------------------------------------------
void RemoteDisplay::drawSprite(uint8_t id, uint16_t frame, int16_t x, int16_t y, uint8_t flags) {
  if (!hasAsset(id)) return;  // never uploaded: a refusal from the C6 now means it lost the asset
  uint8_t p[8], *q = p;
  *q++ = id;
  q = sd_wr_u16(q, frame); q = put16(q, x); q = put16(q, y);
  *q = flags;
  cmd(SD_BLIT, p, 8);
}

void RemoteDisplay::drawSpriteEx(uint8_t id, uint16_t frame, int16_t cx, int16_t cy, float angleDeg, float scaleX,
                                 float scaleY, uint8_t flags) {
  if (!hasAsset(id)) return;
  float a = fmodf(angleDeg, 360.0f);
  auto sc = [](float v) { return (int16_t)constrain(lroundf(v * 256.0f), -32767L, 32767L); };
  uint8_t p[14], *q = p;
  *q++ = id;
  q = sd_wr_u16(q, frame); q = put16(q, cx); q = put16(q, cy);
  q = put16(q, (int16_t)lroundf(a * 10.0f)); q = put16(q, sc(scaleX)); q = put16(q, sc(scaleY));
  *q = flags & RD_TRANSPARENT;
  cmd(SD_BLIT_EX, p, 14);
}

void RemoteDisplay::pushImage(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t* pixels) {
  if (!w || !h) return;
  static uint8_t b[SD_MAX_PAYLOAD];
  uint16_t rowsPer = (SD_MAX_PAYLOAD - 9) / (w * 2);
  if (!rowsPer) return;  // wider than 507 pixels: not possible on this screen
  for (uint16_t r = 0; r < h; r += rowsPer) {
    uint16_t rows = min<uint16_t>(rowsPer, h - r);
    uint8_t* q = b;
    q = put16(q, x); q = put16(q, y + r); q = sd_wr_u16(q, w); q = sd_wr_u16(q, rows);
    *q++ = SD_FMT_RGB565;
    memcpy(q, pixels + (uint32_t)r * w, (uint32_t)rows * w * 2);
    cmd(SD_BLIT_INLINE, b, 9 + rows * w * 2);
  }
}

void RemoteDisplay::copyRect(int16_t srcX, int16_t srcY, int16_t w, int16_t h, int16_t dstX, int16_t dstY) {
  uint8_t p[12], *q = p;
  q = put16(q, srcX); q = put16(q, srcY); q = put16(q, w); q = put16(q, h); q = put16(q, dstX); put16(q, dstY);
  cmd(SD_COPY_RECT, p, 12);
}
