#include "SdHostLink.h"

bool SdHostLink::begin(int baud) {
  return _port.begin(SD_HOST_UART, SD_HOST_PIN_TX, SD_HOST_PIN_RX, SD_HOST_PIN_RTS, SD_HOST_PIN_CTS, baud,
                     SD_HOST_RX_BUF, SD_HOST_TX_BUF);
}

uint8_t SdHostLink::send(uint8_t cmd, const void* payload, uint16_t len) {
  uint8_t s = _seq++;
  if (len > SD_MAX_PAYLOAD) len = SD_MAX_PAYLOAD;
  uint8_t* f = _tx + _txLen;  // always fits: _tx has room for TX_BATCH + one maximum frame
  uint16_t crc = sd_make_header(f, s, cmd, len);
  if (len) {
    memcpy(f + 6, payload, len);
    crc = sd_crc16(f + 6, len, crc);
  }
  if (corruptPerMille && esp_random() % 1000 < corruptPerMille) {
    crc ^= 0x5555;
    corrupted++;
  }
  f[6 + len] = (uint8_t)crc;
  f[7 + len] = (uint8_t)(crc >> 8);
  _txLen += len + SD_FRAME_OVERHEAD;
  if (_txLen >= TX_BATCH) flushTx();
  return s;
}

void SdHostLink::flushTx() {
  if (!_txLen) return;
  uint32_t start = millis();
  while (_port.txFree() < _txLen) {
    if (millis() - start >= txWaitMs) {  // the C6 isn't taking data: drop rather than freeze the app
      txDrops++;
      _txLen = 0;
      return;
    }
    delay(1);
  }
  _port.sendRaw(_tx, _txLen);
  _txLen = 0;
}

bool SdHostLink::poll(uint32_t waitMs) {
  flushTx();  // anything we wait for may depend on queued commands
  for (;;) {
    if (_p == _end) {
      int n = _port.read(_buf, sizeof _buf, waitMs);
      _port.pollEvents();
      if (n <= 0) return false;
      rxBytes += n;
      _p = _buf;
      _end = _buf + n;
      waitMs = 0;
    }
    switch (_parser.push(_p, _end)) {
      case SdParser::FRAME: onReply(); return true;
      case SdParser::BAD_CRC: rxCrcErrors++; break;
      default: break;
    }
  }
}

bool SdHostLink::waitFor(uint8_t cmd, uint32_t timeoutMs, int seq) {
  uint32_t start = millis();
  for (;;) {
    uint32_t el = millis() - start;
    if (el >= timeoutMs) return false;
    if (poll(timeoutMs - el) && _parser.cmd == cmd && (seq < 0 || _parser.seq == (uint8_t)seq)) return true;
  }
}

void SdHostLink::onReply() {
  const uint8_t* p = _parser.payload();
  const uint16_t n = _parser.payloadLen;
  switch (_parser.cmd) {
    case SD_HELLO:
      if (n >= 11) {
        helloSeen = true;
        helloCount++;
        hello.proto = p[0];
        hello.fw = sd_rd_u16(p + 1);
        hello.width = sd_rd_u16(p + 3);
        hello.height = sd_rd_u16(p + 5);
        hello.assetFree = sd_rd_u32(p + 7);
      }
      break;
    case SD_NACK:
      if (n >= 3) {
        nacks++;
        lastNackSeq = p[0];
        lastNackCmd = p[1];
        lastNackErr = p[2];
        if (p[2] == SD_ERR_CRC) crcNacks++;
        if (p[2] == SD_ERR_BAD_ASSET && (p[1] == SD_BLIT || p[1] == SD_BLIT_EX || p[1] == SD_BLIT_BATCH || p[1] == SD_DRAW_TILEMAP)) badAssetNacks++;
        if (p[1] == SD_END_FRAME) framesDone++;  // a refused END_FRAME never gets a FRAME_DONE
      }
      break;
    case SD_PONG:
      pongCount++;
      lastPongSeq = _parser.seq;
      break;
    case SD_FRAME_DONE:
      if (n >= 10) {
        framesDone++;
        lastFrameNo = sd_rd_u16(p);
        lastRenderUs = sd_rd_u32(p + 2);
        lastFlushUs = sd_rd_u32(p + 6);
      }
      break;
    case SD_PINS:
      if (n >= 3) {
        pinD5 = p[0];
        pinRts = p[1];
        pinFc = p[2];
      }
      break;
    case SD_STATUS:
      if (n >= 34) {
        status.freeHeap = sd_rd_u32(p);
        status.assetFree = sd_rd_u32(p + 4);
        status.fpsX10 = sd_rd_u16(p + 8);
        status.rxBytes = sd_rd_u32(p + 10);
        status.rxFrames = sd_rd_u32(p + 14);
        status.crcErrors = sd_rd_u32(p + 18);
        status.lenErrors = sd_rd_u32(p + 22);
        status.fifoOverflows = sd_rd_u32(p + 26);
        status.backpressure = sd_rd_u32(p + 30);
      }
      break;
    default:
      break;
  }
}
