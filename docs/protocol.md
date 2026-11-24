# ScreenDriver link protocol — v1

The Cardputer-ADV (ESP32-S3, the **host**) sends drawing commands to the XIAO ESP32-C6
(the **coprocessor**). The coprocessor renders them into its 320×240 RGB565
framebuffer and flushes the changed areas to the ILI9341.

The C/C++ definitions live in `shared/sd_protocol.h`. This document is the reference.

## Physical link

Cardputer-ADV EXT header (2x7), as printed on the board, counted from the right:

```
                 7th   6th   5th   4th   3rd    2nd   1st (right end)
upper row:       G5    G39   G14   G40   G6     G4    G3
lower row:       G15   G13   G9    G8    5VOUT  GND   5VIN
```

| Signal | Cardputer EXT label (position) | XIAO pin (C6 GPIO) |
|---|---|---|
| Host TX → C6 RX | G13 (lower row, 6th from right) | D7 (GPIO17) |
| Host RX ← C6 TX | G15 (lower row, 7th from right) | D6 (GPIO16) |
| Host RTS → C6 CTS | G4 (upper row, 2nd from right) | D5 (GPIO23) |
| Host CTS ← C6 RTS | G6 (upper row, 3rd from right) | D9 (GPIO20). Not D4: see note below |
| 5 V | 5VOUT (lower row, 3rd from right), through a Schottky diode | 5V |
| GND | GND (lower row, 2nd from right) | GND |

The C6 RTS uses D9 (free, because the panel's MISO is not wired). It was on D4 originally and was moved while
debugging; the real fault turned out to be the Cardputer leaving G6 driven as an output (now fixed in
`SdUartPort::begin`, which resets its input pins), so D4 is most likely fine.

Watch out: G6 sits directly above 5VOUT, and G4 directly above GND. A wire one row off puts 5 V on a
C6 pin, which is not 5 V tolerant. Never use 5VIN. The XIAO's 5V pin is wired straight to its USB VBUS,
so use a Schottky diode (1N5817/SS14, anode at the Cardputer, the band toward the XIAO) or unplug that wire
while the XIAO is on USB. With the diode (installed 2026-11-24) the XIAO can stay on USB for flashing and
logging while the Cardputer powers it, or run on Cardputer power alone (both tested).

- UART, 8N1. The baud rate is `SD_LINK_BAUD` in `shared/sd_protocol.h` (default 5 000 000, verified with RTS/CTS and 0 overflows; the `*_2m` build targets are a 2 000 000 fallback).
  Both sides must be built with the same value.
- Hardware RTS/CTS flow control is required. The C6 deasserts RTS when its 128-byte hardware FIFO holds
  more than 100 bytes (the driver moves data on into a 16 KB receive buffer), so the host never overruns it.
  No software credit scheme is used.

## Frame format

All multi-byte fields are **little-endian**.

```
+------+------+--------+-----+-----+-------------+--------+
| 0xA5 | 0x5A | LEN u16| SEQ | CMD | PAYLOAD ... | CRC16  |
+------+------+--------+-----+-----+-------------+--------+
```

- `LEN` is the number of bytes in `SEQ + CMD + PAYLOAD` (that is, payload length + 2).
- `SEQ` is a rolling counter from the sender (0–255). Replies echo it where noted.
- `CRC16` is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no xor-out),
  computed over `LEN`, `SEQ`, `CMD` and `PAYLOAD`.
- The maximum payload is **1024 bytes** (`SD_MAX_PAYLOAD`).
- **Resync:** on a bad CRC or an impossible `LEN`, the receiver drops bytes until it sees
  the next `0xA5 0x5A` and reports `NACK` with `ERR_CRC`.
- **Host resync procedure:** if a frame is truncated (for example the host reset mid-frame), the C6
  parser may sit inside a long body. The host recovers by sending `SD_MAX_PAYLOAD + 8` (or more) zero
  bytes. That completes the bogus frame, which fails its CRC, and returns the parser to sync search.
  The host then sends PING.
- **Boot noise:** the C6 ROM bootloader prints its boot log at 115200 baud on GPIO16, which is our
  link TX. After the C6 resets, the host sees a short burst of garbage before HELLO. The parser
  ignores it.

## Types

- `i16` coordinates, which may be negative (they are clipped).
- `color` is RGB565 as `u16`.
- `str` is UTF-8 bytes that run to the end of the payload, with no terminator.

## Host → C6 commands

Draw commands are fire-and-forget. Only the commands marked **(reply)** produce a response.

### System (0x00–0x0F)

| Op | Name | Payload |
|---|---|---|
| 0x00 | NOP | – |
| 0x01 | PING **(reply PONG)** | any bytes, which are echoed back |
| 0x02 | RESET | – (soft-restarts the C6, which then sends HELLO) |
| 0x03 | INIT | `u8 rotation` (1 or 3: both landscape; others give `ERR_BAD_ARG`), `u8 flags` (bit0 = clear screen) |
| 0x04 | SET_BACKLIGHT | `u8 level (0–255)` |
| 0x05 | GET_STATUS **(reply STATUS)** | – |
| 0x0D | DEBUG_HANG | (none). Test only: the C6 loops forever; its 3 s watchdog reboots it and it sends HELLO. |
| 0x0E | DEBUG_PINS **(reply PINS)** | `u8 action`. Test only (`link_test`'s wire test): 0/1 = drive the C6 RTS pin (D9) low/high as a plain GPIO, 2 = just read D5, 3/4 = give D9 back to the UART with flow control off/on. Actions 0–2 turn flow control off. |
| 0x0F | DEBUG_STALL | `u16 ms` (max 2500, below the 3 s watchdog). The C6 stops reading for this long. Test only, used to exercise RTS/CTS. |

### Frame control (0x10–0x1F)

| Op | Name | Payload |
|---|---|---|
| 0x10 | BEGIN_FRAME | – |
| 0x11 | END_FRAME **(reply FRAME_DONE)** | `u8 flags` (bit0 = force full flush) |

Draw commands go into the framebuffer straight away, but nothing reaches the panel until
`END_FRAME`. Then the dirty rectangles are flushed. Commands sent outside a
BEGIN/END pair are also flushed at the next `END_FRAME`.

Dirty tracking: each draw adds its bounding box (clipped to the clip rectangle and the screen).
The C6 keeps up to 16 rectangles, merging overlapping or nearby ones; when the list is full the
closest pair is merged. If the dirty area exceeds 60 % of the screen, the whole screen is flushed.
The flush is synchronous. While it runs, the UART driver keeps receiving into its 16 KB buffer, so the
next frame's commands arrive meanwhile; RTS holds the host back only if that buffer fills.

Pacing: `FRAME_DONE` is sent after the flush. The host library keeps at most one frame in flight:
`endFrame()` waits for the previous frame's `FRAME_DONE` before sending the next `END_FRAME`.

### Drawing state (0x20–0x2F)

| Op | Name | Payload |
|---|---|---|
| 0x20 | SET_CLIP | `i16 x, y, w, h` |
| 0x21 | CLEAR_CLIP | – |
| 0x22 | SET_TEXT_STYLE | `u8 font`, `u8 size`, `color fg`, `color bg`, `u8 flags` (bit0 = draw bg), `u8 datum` |
| 0x23 | SET_CURSOR | `i16 x, y` (start position for PRINT) |

`font` IDs: 0 = 6×8 GLCD, 1 = Font2 (16 px), 2 = Font4 (26 px), 3 = Font7 (7-segment),
4 = FreeSans9pt, 5 = FreeSansBold12pt. `datum` uses the LovyanGFX/M5GFX `textdatum` values: top 0 left / 1 center / 2 right,
middle 4 / 5 / 6, bottom 8 / 9 / 10, baseline 16 / 17 / 18 (so `MC_DATUM` = 5).

### Primitives (0x30–0x3F)

| Op | Name | Payload |
|---|---|---|
| 0x30 | FILL_SCREEN | `color` |
| 0x31 | FILL_RECT | `i16 x, y, w, h`, `color` |
| 0x32 | DRAW_RECT | `i16 x, y, w, h`, `color` |
| 0x33 | DRAW_LINE | `i16 x0, y0, x1, y1`, `color` |
| 0x34 | DRAW_HLINE | `i16 x, y, w`, `color` |
| 0x35 | DRAW_VLINE | `i16 x, y, h`, `color` |
| 0x36 | DRAW_CIRCLE | `i16 x, y, r`, `color` |
| 0x37 | FILL_CIRCLE | `i16 x, y, r`, `color` |
| 0x38 | DRAW_ROUND_RECT | `i16 x, y, w, h, r`, `color` |
| 0x39 | FILL_ROUND_RECT | `i16 x, y, w, h, r`, `color` |
| 0x3A | DRAW_TRIANGLE | `i16 x0, y0, x1, y1, x2, y2`, `color` |
| 0x3B | FILL_TRIANGLE | `i16 x0, y0, x1, y1, x2, y2`, `color` |
| 0x3C | DRAW_PIXELS | `color`, then N × (`i16 x, i16 y`) |

### Text (0x40–0x4F)

| Op | Name | Payload |
|---|---|---|
| 0x40 | DRAW_TEXT | `i16 x, y`, `str` (uses the current text style, aligned by its `datum`) |
| 0x41 | PRINT | `str` at the cursor, console style: a newline byte (0x0A) starts a new line, long lines wrap at the right edge, and the cursor advances. `datum` is ignored. |

### Assets (0x50–0x5F)

Assets are images (sprites or sprite sheets) and tilemaps stored in C6 RAM and referenced by `id`
(0–63). The pool is 160 KB (HELLO and STATUS report the free bytes). Freeing an asset compacts the
pool, so it never fragments.

| Op | Name | Payload |
|---|---|---|
| 0x50 | ASSET_BEGIN **(reply ACK/NACK)** | `u8 id`, `u8 format`, `u16 w, h`, `u16 frame_w, frame_h`, `u16 transparent`, [`u16 palette_count`: INDEXED only] |
| 0x51 | ASSET_DATA | `u8 id`, `u32 offset`, raw bytes (NACK only on error) |
| 0x52 | ASSET_END **(reply ACK/NACK)** | `u8 id` |
| 0x53 | ASSET_FREE | `u8 id` (0xFF = free all; NACK only on error) |
| 0x54 | TILE_SET | `u8 map`, `u16 tx, ty`, tile bytes: written from (tx, ty) along the row, continuing on the next rows (NACK only on error) |

- `format`:

  | Value | Format | Stored size | `transparent` | ASSET_DATA stream |
  |---|---|---|---|---|
  | 0 | RGB565 | w·h·2 | the transparent color | pixels, RGB565 little-endian |
  | 1 | INDEXED (fw 0600) | count·2 + w·h | a palette index; > 255 = none | the palette (`count` × RGB565 LE), then 1 index byte per pixel |
  | 2 | TILEMAP (fw 0600) | w·h | ignored | tile numbers, 1 byte each, row-major (`w, h` in tiles; `frame_w/h` ignored) |

  Sizes are rounded up to 4 bytes. The host library picks INDEXED by itself for images with ≤ 256 colors.
- A sprite sheet is a `w × h` image split into `frame_w × frame_h` cells, numbered
  row-major from 0. `frame_w`/`frame_h` = 0 means one frame of the whole image.
- ASSET_DATA carries the pixels **in frame order**: all of frame 0 (row by row),
  then frame 1, and so on. The host library reorders the sheet before sending. `offset` is in bytes.
- ASSET_BEGIN is refused with `ERR_NO_MEM` if the asset does not fit, with `ERR_BAD_ARG` for an id above 63,
  an unknown format, a zero size, a palette count outside 1–256 or frames that don't divide the image, and
  with `ERR_BAD_LEN` if the header is short. It replaces an existing asset with the same id. ASSET_END is refused with `ERR_BAD_ASSET` (and the asset freed) if bytes are missing.

### Blitting (0x60–0x6F)

| Op | Name | Payload |
|---|---|---|
| 0x60 | BLIT | `u8 id`, `u16 frame`, `i16 x, y`, `u8 flags` (bit0 = use transparent key, bit1 = flip X, bit2 = flip Y) |
| 0x61 | BLIT_INLINE | `i16 x, y`, `u16 w, h`, `u8 format` (must be 0, RGB565), pixel data (RGB565 LE, row-major; must fit in one frame) |
| 0x62 | BLIT_EX | `u8 id`, `u16 frame`, `i16 cx, cy` (where the frame's center lands), `i16 angle` (0.1°, clockwise), `i16 scale_x, scale_y` (256 = 1.0, negative = flip), `u8 flags` (bit0 = transparent) |
| 0x63 | BLIT_BATCH (fw 0600) | 1–127 BLIT payloads (8 bytes each) back to back |
| 0x64 | DRAW_TILEMAP (fw 0600) | `u8 map`, `u8 tileset`, `i16 scroll_x, scroll_y`, `i16 x, y, w, h` (screen rectangle), `u8 flags` (bit0 = transparent, bit3 = wrap) |

BLIT, BLIT_EX and BLIT_BATCH draw RGB565 and INDEXED images alike. A missing asset (or a tilemap) gives
`ERR_BAD_ASSET`, a frame out of range `ERR_BAD_ARG`. BLIT_BATCH needs a payload length that is a multiple of 8;
it draws every entry and reports the first error, if any. The host library batches consecutive sprites by
itself (up to 127 per command).

DRAW_TILEMAP fills the rectangle with the map's tiles; map pixel (`scroll_x`, `scroll_y`) lands at the rectangle's
top-left. Tile number *n* is frame *n* of the tileset (any image asset, RGB565 or INDEXED); tile 0xFF and numbers
beyond the tileset's frames are not drawn. Without wrap, the parts of the rectangle outside the map are left
unchanged; with wrap the map repeats in both directions. The whole rectangle is marked dirty.
`map` must be a TILEMAP asset and `tileset` an image asset, else `ERR_BAD_ASSET`.

TILE_SET needs a complete TILEMAP asset (`ERR_BAD_ASSET` otherwise); writing outside the map gives
`ERR_BAD_ARG` and changes nothing.

### Misc (0x70–0x7F)

| Op | Name | Payload |
|---|---|---|
| 0x70 | COPY_RECT | `i16 src_x, src_y, w, h, dst_x, dst_y` |

## C6 → host replies (0x80–0x8F)

| Op | Name | Payload |
|---|---|---|
| 0x80 | HELLO | `u8 proto_ver`, `u16 fw_ver`, `u16 width, height`, `u32 asset_pool_free` (sent at boot and after RESET; the host must re-send INIT and its assets) |
| 0x81 | ACK | `u8 acked_seq`, `u8 acked_cmd` |
| 0x82 | NACK | `u8 seq`, `u8 cmd`, `u8 error` |
| 0x83 | FRAME_DONE | `u16 frame_no`, `u32 render_us`, `u32 flush_us` |
| 0x84 | STATUS | `u32 free_heap`, `u32 asset_pool_free`, `u16 fps_x10`, `u32 rx_bytes`, `u32 rx_frames`, `u32 crc_errors`, `u32 len_errors`, `u32 fifo_overflows`, `u32 backpressure` (34 bytes) |
| 0x85 | PONG | echoed bytes from PING. The header `SEQ` equals the PING's `SEQ`. |
| 0x86 | PINS | `u8 d5_level`, `u8 rts_readback`, `u8 flow_control` (reply to DEBUG_PINS) |

- `rx_frames` counts valid frames, including the GET_STATUS that requested this reply.
- `fifo_overflows` means the hardware FIFO overflowed and data was lost. It must stay 0.
- `backpressure` counts the times the C6's receive buffer filled and RTS held the host. This is normal under load and loses no data.

Draw and asset commands without a listed reply answer only on error, with a NACK carrying the command's `SEQ`,
the command and the error code.

### Error codes

| Code | Name |
|---|---|
| 1 | ERR_CRC |
| 2 | ERR_UNKNOWN_CMD |
| 3 | ERR_BAD_LEN |
| 4 | ERR_NO_MEM |
| 5 | ERR_BAD_ASSET |
| 6 | ERR_BAD_ARG |

## Versioning

`SD_PROTO_VERSION` is 1. The host compares it against HELLO and refuses to run if they differ.

New commands are added without changing the protocol version; HELLO's `fw_ver` says what the C6 supports.
The host library requires `fw_ver` ≥ `RD_MIN_C6_FW` (0x0600). Firmware 0x0600 added the INDEXED and TILEMAP
formats, TILE_SET, BLIT_BATCH and DRAW_TILEMAP (marked "fw 0600" above).
