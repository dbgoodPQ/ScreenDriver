// ScreenDriver link protocol, v1. Shared by the Cardputer host library and the C6 coprocessor.
// The reference spec is docs/protocol.md. Keep both in sync.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define SD_PROTO_VERSION 1

#define SD_SYNC0 0xA5
#define SD_SYNC1 0x5A
#define SD_MAX_PAYLOAD 1024
// Bytes around the payload: sync(2) + len(2) + seq(1) + cmd(1) + crc(2)
#define SD_FRAME_OVERHEAD 8

// UART link speed. Both sides must match. Override with -DSD_LINK_BAUD=... on both builds.
#ifndef SD_LINK_BAUD
#define SD_LINK_BAUD 5000000  // verified 2026-11-23: 10/10 link tests, 0 overflows
#endif

#define SD_SCREEN_W 320
#define SD_SCREEN_H 240
#define SD_MAX_ASSETS 64

enum sd_cmd : uint8_t {
  // System
  SD_NOP            = 0x00,
  SD_PING           = 0x01,
  SD_RESET          = 0x02,
  SD_INIT           = 0x03,
  SD_SET_BACKLIGHT  = 0x04,
  SD_GET_STATUS     = 0x05,
  SD_DEBUG_HANG     = 0x0D,  // test only: the C6 stops in an endless loop (exercises its watchdog)
  SD_DEBUG_PINS     = 0x0E,  // test only: drive/read the flow-control wires (reply PINS), see SD_PINS_* actions
  SD_DEBUG_STALL    = 0x0F,  // test only: the C6 stops reading for N ms (exercises flow control)
  // Frame control
  SD_BEGIN_FRAME    = 0x10,
  SD_END_FRAME      = 0x11,
  // Drawing state
  SD_SET_CLIP       = 0x20,
  SD_CLEAR_CLIP     = 0x21,
  SD_SET_TEXT_STYLE = 0x22,
  SD_SET_CURSOR     = 0x23,  // i16 x, y: start position for PRINT
  // Primitives
  SD_FILL_SCREEN    = 0x30,
  SD_FILL_RECT      = 0x31,
  SD_DRAW_RECT      = 0x32,
  SD_DRAW_LINE      = 0x33,
  SD_DRAW_HLINE     = 0x34,
  SD_DRAW_VLINE     = 0x35,
  SD_DRAW_CIRCLE    = 0x36,
  SD_FILL_CIRCLE    = 0x37,
  SD_DRAW_RRECT     = 0x38,
  SD_FILL_RRECT     = 0x39,
  SD_DRAW_TRIANGLE  = 0x3A,
  SD_FILL_TRIANGLE  = 0x3B,
  SD_DRAW_PIXELS    = 0x3C,
  // Text
  SD_DRAW_TEXT      = 0x40,
  SD_PRINT          = 0x41,  // str at the cursor, wraps at the right edge, advances the cursor
  // Assets
  SD_ASSET_BEGIN    = 0x50,
  SD_ASSET_DATA     = 0x51,
  SD_ASSET_END      = 0x52,
  SD_ASSET_FREE     = 0x53,
  SD_TILE_SET       = 0x54,  // u8 map, u16 tx, ty, tile bytes (along the row, continuing on the next rows)
  // Blitting
  SD_BLIT           = 0x60,
  SD_BLIT_INLINE    = 0x61,
  SD_BLIT_EX        = 0x62,  // BLIT with rotation and scale (milestone 4)
  SD_BLIT_BATCH     = 0x63,  // several BLIT payloads (8 bytes each) in one command (milestone 6)
  SD_DRAW_TILEMAP   = 0x64,  // a tilemap layer into a screen rectangle (milestone 6)
  // Misc
  SD_COPY_RECT      = 0x70,
  // Replies (C6 -> host)
  SD_HELLO          = 0x80,
  SD_ACK            = 0x81,
  SD_NACK           = 0x82,
  SD_FRAME_DONE     = 0x83,
  SD_STATUS         = 0x84,
  SD_PONG           = 0x85,
  SD_PINS           = 0x86,  // reply to DEBUG_PINS: u8 d5_level, u8 rts_readback, u8 flow_control
};

enum sd_error : uint8_t {
  SD_ERR_NONE        = 0,
  SD_ERR_CRC         = 1,
  SD_ERR_UNKNOWN_CMD = 2,
  SD_ERR_BAD_LEN     = 3,
  SD_ERR_NO_MEM      = 4,
  SD_ERR_BAD_ASSET   = 5,
  SD_ERR_BAD_ARG     = 6,
};

enum sd_asset_format : uint8_t {
  SD_FMT_RGB565  = 0,
  SD_FMT_INDEXED = 1,  // 8-bit palette image: palette (u16 count) + 1 byte per pixel (milestone 6)
  SD_FMT_TILEMAP = 2,  // tilemap: w x h tile numbers, 1 byte each (milestone 6)
};

// Flag bits
#define SD_INIT_CLEAR        0x01
#define SD_END_FULL_FLUSH    0x01
#define SD_TEXT_DRAW_BG      0x01
#define SD_BLIT_TRANSPARENT  0x01
#define SD_BLIT_FLIP_X       0x02
#define SD_BLIT_FLIP_Y       0x04
#define SD_TILEMAP_WRAP      0x08  // DRAW_TILEMAP: repeat the map in both directions
#define SD_TILE_EMPTY        0xFF  // tile number that is never drawn

// DEBUG_PINS actions (payload: u8 action)
#define SD_PINS_RTS_LOW       0  // drive the C6 RTS pin (D9) LOW as a plain GPIO, flow control off
#define SD_PINS_RTS_HIGH      1  // drive the C6 RTS pin (D9) HIGH as a plain GPIO, flow control off
#define SD_PINS_READ          2  // just read D5, flow control off
#define SD_PINS_RELEASE_FC_OFF 3  // give the RTS pin back to the UART, flow control stays off
#define SD_PINS_RELEASE_FC_ON  4  // give the RTS pin back to the UART, flow control on

#define SD_FONT_GLCD      0
#define SD_FONT_FONT2     1
#define SD_FONT_FONT4     2
#define SD_FONT_FONT7     3
#define SD_FONT_SANS9     4
#define SD_FONT_SANSB12   5

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF.
static inline uint16_t sd_crc16(const uint8_t* data, size_t len, uint16_t crc = 0xFFFF) {
  while (len--) {
    crc ^= (uint16_t)(*data++) << 8;
    for (int i = 0; i < 8; ++i)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// Little-endian helpers
static inline uint16_t sd_rd_u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline int16_t  sd_rd_i16(const uint8_t* p) { return (int16_t)sd_rd_u16(p); }
static inline uint32_t sd_rd_u32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint8_t* sd_wr_u16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; return p + 2; }
static inline uint8_t* sd_wr_i16(uint8_t* p, int16_t v) { return sd_wr_u16(p, (uint16_t)v); }
static inline uint8_t* sd_wr_u32(uint8_t* p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; return p + 4;
}
