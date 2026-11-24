// ScreenDriver link layer, shared by host (Cardputer, ESP32-S3) and coprocessor (ESP32-C6).
//  - SdParser:   byte stream -> frames (sync, length, CRC check, resync)
//  - SdUartPort: ESP-IDF UART driver with hardware RTS/CTS flow control
#pragma once
#include <string.h>
#include "sd_protocol.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ---------------------------------------------------------------------------
// Frame parser
// ---------------------------------------------------------------------------
class SdParser {
 public:
  enum Event : uint8_t { NONE, FRAME, BAD_CRC, BAD_LEN };

  // Valid after FRAME or BAD_CRC (on BAD_CRC the fields are untrusted and only useful for diagnostics)
  uint8_t seq = 0;
  uint8_t cmd = 0;
  uint16_t payloadLen = 0;
  const uint8_t* payload() const { return _body + 2; }

  // Consumes bytes from [p, end) until one event happens or input runs out.
  // Advances p. Call repeatedly until p == end.
  Event push(const uint8_t*& p, const uint8_t* end) {
    while (p < end) {
      switch (_state) {
        case S_SYNC0:
          // fast scan for the first sync byte
          {
            const void* f = memchr(p, SD_SYNC0, end - p);
            if (!f) { p = end; return NONE; }
            p = (const uint8_t*)f + 1;
            _state = S_SYNC1;
          }
          break;
        case S_SYNC1: {
          uint8_t b = *p++;
          if (b == SD_SYNC1) _state = S_LEN0;
          else if (b != SD_SYNC0) _state = S_SYNC0;  // "A5 A5 5A" still syncs
          break;
        }
        case S_LEN0: _len = *p++; _state = S_LEN1; break;
        case S_LEN1:
          _len |= (uint16_t)(*p++) << 8;
          if (_len < 2 || _len > SD_MAX_PAYLOAD + 2) { _state = S_SYNC0; return BAD_LEN; }
          _idx = 0;
          _state = S_BODY;
          break;
        case S_BODY: {
          size_t n = end - p;
          if (n > (size_t)(_len - _idx)) n = _len - _idx;
          memcpy(_body + _idx, p, n);
          _idx += n;
          p += n;
          if (_idx == _len) _state = S_CRC0;
          break;
        }
        case S_CRC0: _crc = *p++; _state = S_CRC1; break;
        case S_CRC1: {
          _crc |= (uint16_t)(*p++) << 8;
          _state = S_SYNC0;
          uint8_t lenLE[2] = {(uint8_t)_len, (uint8_t)(_len >> 8)};
          uint16_t c = sd_crc16(_body, _len, sd_crc16(lenLE, 2));
          seq = _body[0];
          cmd = _body[1];
          payloadLen = _len - 2;
          return (c == _crc) ? FRAME : BAD_CRC;
        }
      }
    }
    return NONE;
  }

  void reset() { _state = S_SYNC0; }

 private:
  enum State : uint8_t { S_SYNC0, S_SYNC1, S_LEN0, S_LEN1, S_BODY, S_CRC0, S_CRC1 };
  State _state = S_SYNC0;
  uint16_t _len = 0, _idx = 0, _crc = 0;
  uint8_t _body[SD_MAX_PAYLOAD + 2];  // seq + cmd + payload
};

// Fills the 6-byte header (sync, len, seq, cmd). Returns the CRC seed covering len/seq/cmd.
static inline uint16_t sd_make_header(uint8_t hdr[6], uint8_t seq, uint8_t cmd, uint16_t payloadLen) {
  uint16_t len = payloadLen + 2;
  hdr[0] = SD_SYNC0;
  hdr[1] = SD_SYNC1;
  hdr[2] = (uint8_t)len;
  hdr[3] = (uint8_t)(len >> 8);
  hdr[4] = seq;
  hdr[5] = cmd;
  return sd_crc16(hdr + 2, 4);
}

// ---------------------------------------------------------------------------
// UART port with hardware flow control
// ---------------------------------------------------------------------------
class SdUartPort {
 public:
  // Counters, updated by pollEvents()
  uint32_t fifoOverflows = 0;   // real data loss: the hardware FIFO overflowed
  uint32_t backpressure = 0;    // ring buffer full: flow control held the sender (no data loss)
  uint32_t lineErrors = 0;      // framing or parity errors (noise or baud mismatch)

  // Pin state found by begin() before it reset the input pins (oe = 1 means something was driving it)
  gpio_io_config_t ctsPinBefore = {}, rxPinBefore = {};

  bool begin(uart_port_t port, int txPin, int rxPin, int rtsPin, int ctsPin, int baud,
             int rxBufSize = 16384, int txBufSize = 8192) {
    _port = port;
    _ctsPin = ctsPin;
    _rtsPin = rtsPin;
    uart_config_t cfg = {};
    cfg.baud_rate = baud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS;
    cfg.rx_flow_ctrl_thresh = 100;  // RTS deasserts when the 128-byte HW FIFO holds > 100 bytes
    cfg.source_clk = UART_SCLK_DEFAULT;
    if (uart_driver_install(_port, rxBufSize, txBufSize, 512, &_events, 0) != ESP_OK) return false;
    if (uart_param_config(_port, &cfg) != ESP_OK) return false;
    // Another library (e.g. M5 board auto-detection) may have left our input pins driven as outputs or
    // attached to another peripheral. uart_set_pin() does not turn such an output off, so the pin would
    // fight the other side's driver (seen on Cardputer-ADV G6). Record the state for diagnostics, then reset.
    if (ctsPin >= 0) gpio_get_io_config((gpio_num_t)ctsPin, &ctsPinBefore);
    gpio_get_io_config((gpio_num_t)rxPin, &rxPinBefore);
    gpio_reset_pin((gpio_num_t)rxPin);
    if (ctsPin >= 0) gpio_reset_pin((gpio_num_t)ctsPin);
    if (uart_set_pin(_port, txPin, rxPin, rtsPin, ctsPin) != ESP_OK) return false;
    // Pull-up on CTS: a missing wire reads HIGH ("not ready") instead of floating at random.
    if (ctsPin >= 0) gpio_pullup_en((gpio_num_t)ctsPin);
    // Drain the FIFO into the ring buffer well before the RTS threshold, so RTS only
    // deasserts when the ring buffer itself is full (that is, the application is behind).
    uart_set_rx_full_threshold(_port, 64);
    uart_set_rx_timeout(_port, 10);
    return true;
  }

  // Sends one complete frame. Blocks only while the TX ring buffer is full.
  void sendFrame(uint8_t seq, uint8_t cmd, const void* payload, uint16_t len) {
    uint8_t hdr[6];
    uint16_t crc = sd_make_header(hdr, seq, cmd, len);
    if (len) crc = sd_crc16((const uint8_t*)payload, len, crc);
    uint8_t tail[2] = {(uint8_t)crc, (uint8_t)(crc >> 8)};
    if (len <= 248) {  // small frame: one write
      uint8_t f[256];
      memcpy(f, hdr, 6);
      if (len) memcpy(f + 6, payload, len);
      memcpy(f + 6 + len, tail, 2);
      uart_write_bytes(_port, f, len + 8);
    } else {
      uart_write_bytes(_port, hdr, 6);
      uart_write_bytes(_port, payload, len);
      uart_write_bytes(_port, tail, 2);
    }
  }

  // Raw bytes (tests only)
  void sendRaw(const void* data, size_t n) { uart_write_bytes(_port, data, n); }

  // Free space in the driver's transmit buffer. Writing at most this much never blocks.
  size_t txFree() {
    size_t n = 0;
    uart_get_tx_buffer_free_size(_port, &n);
    return n;
  }

  // Reads what is available, up to max bytes. If nothing is buffered, waits up to waitMs for the first byte.
  int read(uint8_t* dst, size_t max, uint32_t waitMs) {
    size_t avail = 0;
    uart_get_buffered_data_len(_port, &avail);
    int got = 0;
    if (avail == 0) {
      if (waitMs == 0) return 0;
      int n = uart_read_bytes(_port, dst, 1, pdMS_TO_TICKS(waitMs) ? pdMS_TO_TICKS(waitMs) : 1);
      if (n <= 0) return 0;
      got = 1;
      uart_get_buffered_data_len(_port, &avail);
    }
    if (avail > max - got) avail = max - got;
    if (avail) {
      int n = uart_read_bytes(_port, dst + got, avail, 0);
      if (n > 0) got += n;
    }
    return got;
  }

  // Discards everything received so far (FIFO and ring buffer) and restarts the receiver.
  // Used after the other side reboots, to drop its boot log noise.
  void flushInput() { uart_flush_input(_port); }

  // Baud rate the hardware actually achieved (clock divider rounding)
  uint32_t actualBaud() const {
    uint32_t b = 0;
    uart_get_baudrate(_port, &b);
    return b;
  }

  void waitTxDone(uint32_t timeoutMs = 1000) { uart_wait_tx_done(_port, pdMS_TO_TICKS(timeoutMs)); }

  // True if everything queued has left the wire within timeoutMs. False means TX is stuck,
  // which with flow control on means our CTS input is not being asserted by the other side.
  bool txIdle(uint32_t timeoutMs) { return uart_wait_tx_done(_port, pdMS_TO_TICKS(timeoutMs)) == ESP_OK; }

  // Diagnostics: turn hardware RTS/CTS on or off at runtime.
  void setFlowControl(bool on) {
    uart_set_hw_flow_ctrl(_port, on ? UART_HW_FLOWCTRL_CTS_RTS : UART_HW_FLOWCTRL_DISABLE, 100);
    _flowControl = on;
  }
  bool flowControl() const { return _flowControl; }

  // Diagnostics: take the RTS pin away from the UART and drive it as a plain GPIO (level 0 or 1),
  // or give it back to the UART (level < 0). Turn flow control off first, or our own TX may block.
  void forceRts(int level) {
    if (_rtsPin < 0) return;
    gpio_num_t pin = (gpio_num_t)_rtsPin;
    if (level < 0) {
      uart_set_pin(_port, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, _rtsPin, UART_PIN_NO_CHANGE);
      return;
    }
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);  // output, with read-back
    gpio_set_level(pin, level);
  }
  // Pad level of the RTS pin (meaningful while forced with forceRts).
  int rtsLevel() const { return _rtsPin >= 0 ? gpio_get_level((gpio_num_t)_rtsPin) : -1; }

  // Level on our CTS input pin: 0 = the other side is ready (RTS asserted), 1 = not ready or wire missing.
  int ctsLevel() const { return _ctsPin >= 0 ? gpio_get_level((gpio_num_t)_ctsPin) : -1; }

  // Drains the driver's event queue and updates the counters. Call regularly.
  void pollEvents() {
    uart_event_t ev;
    while (xQueueReceive(_events, &ev, 0) == pdTRUE) {
      switch (ev.type) {
        case UART_FIFO_OVF: fifoOverflows++; break;
        case UART_BUFFER_FULL: backpressure++; break;
        case UART_FRAME_ERR:
        case UART_PARITY_ERR: lineErrors++; break;
        default: break;
      }
    }
  }

 private:
  uart_port_t _port = UART_NUM_1;
  QueueHandle_t _events = nullptr;
  int _ctsPin = -1;
  int _rtsPin = -1;
  bool _flowControl = true;
};
