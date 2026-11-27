// Host-side link to the ScreenDriver coprocessor (Cardputer-ADV -> XIAO ESP32-C6).
#pragma once
#include <Arduino.h>
#include "sd_link.h"

// Cardputer-ADV EXT header pins (labels as printed; positions counted from the right end)
#define SD_HOST_PIN_TX   13  // G13, lower row 6th -> C6 D7
#define SD_HOST_PIN_RX   15  // G15, lower row 7th <- C6 D6
#define SD_HOST_PIN_RTS   4  // G4,  upper row 2nd -> C6 D5 (CTS)  (above GND)
#define SD_HOST_PIN_CTS   6  // G6,  upper row 3rd <- C6 D9 (RTS)  (above 5VOUT: don't slip a row!)
#define SD_HOST_UART     UART_NUM_1

// UART driver ring buffers on the host. Apps that are short on heap (e.g. a launcher that needs ~40 KB
// contiguous for a TLS handshake) can shrink them; a smaller TX buffer only makes big pushes block sooner.
#ifndef SD_HOST_RX_BUF
#define SD_HOST_RX_BUF 4096
#endif
#ifndef SD_HOST_TX_BUF
#define SD_HOST_TX_BUF 16384
#endif

struct SdHelloInfo {
  uint8_t proto = 0;
  uint16_t fw = 0, width = 0, height = 0;
  uint32_t assetFree = 0;
};

struct SdStatusInfo {
  uint32_t freeHeap = 0, assetFree = 0;
  uint16_t fpsX10 = 0;
  uint32_t rxBytes = 0, rxFrames = 0, crcErrors = 0, lenErrors = 0, fifoOverflows = 0, backpressure = 0;
};

class SdHostLink {
 public:
  bool begin(int baud = SD_LINK_BAUD);

  // Queues a command. Returns the sequence number used. Commands are collected in a local buffer and handed to
  // the UART driver in ~1 KB blocks (one driver call per block instead of per command); poll(), waitFor() and
  // port() send whatever is queued first, and RemoteDisplay::endFrame() calls flushTx().
  uint8_t send(uint8_t cmd, const void* payload = nullptr, uint16_t len = 0);
  void flushTx();

  // Reads and parses incoming replies. Returns true if one reply frame was parsed
  // (available in reply()). Waits up to waitMs for data.
  bool poll(uint32_t waitMs = 0);

  // Polls until a reply with this cmd (and seq, if seq >= 0) arrives, or timeout.
  bool waitFor(uint8_t cmd, uint32_t timeoutMs, int seq = -1);

  // Drops all received data and any partial reply (after the C6 reboots).
  void flushInput() {
    _port.flushInput();
    _parser.reset();
    _p = _end = _buf;
  }

  const SdParser& reply() const { return _parser; }

  // State gathered from replies
  bool helloSeen = false;
  SdHelloInfo hello;
  SdStatusInfo status;
  uint8_t pinD5 = 0, pinRts = 0, pinFc = 0;  // last PINS reply (wire test)
  uint32_t helloCount = 0;                   // HELLOs received (an unexpected one means the C6 rebooted)
  uint32_t framesDone = 0;                   // FRAME_DONE replies received
  uint16_t lastFrameNo = 0;
  uint32_t lastRenderUs = 0, lastFlushUs = 0;
  uint32_t nacks = 0;
  uint8_t lastNackSeq = 0, lastNackCmd = 0, lastNackErr = 0;
  uint32_t rxCrcErrors = 0;
  uint32_t rxBytes = 0;  // raw bytes received from the C6, valid or not
  uint32_t crcNacks = 0;       // NACKs with ERR_CRC (a command was lost)
  uint32_t badAssetNacks = 0;  // NACKs with ERR_BAD_ASSET for a sprite/tilemap draw (the C6 lost an asset)
  uint32_t pongCount = 0;
  uint8_t lastPongSeq = 0;

  // Sending never blocks for long: if the UART buffer has no room for txWaitMs (link unplugged, so the
  // C6 holds our CTS), the queued commands are dropped and txDrops counts it. 0 = drop at once.
  uint32_t txWaitMs = 200;
  uint32_t txDrops = 0;
  // Test only: corrupt the CRC of this many commands per 1000 (the C6 NACKs them with ERR_CRC).
  uint16_t corruptPerMille = 0;
  uint32_t corrupted = 0;

  // Direct access to the UART (sends the queued commands first, so raw bytes keep their order).
  SdUartPort& port() {
    flushTx();
    return _port;
  }

 private:
  void onReply();
  SdUartPort _port;
  SdParser _parser;
  uint8_t _seq = 0;
  uint8_t _buf[1100];
  static const uint16_t TX_BATCH = 1024;
  uint8_t _tx[TX_BATCH + SD_MAX_PAYLOAD + SD_FRAME_OVERHEAD];
  uint16_t _txLen = 0;
  const uint8_t* _p = _buf;
  const uint8_t* _end = _buf;
};
