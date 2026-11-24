# ScreenDriver

A Seeed XIAO ESP32-C6 used as a graphics coprocessor for the M5Stack Cardputer-ADV. It drives a 2.8"
ILI9341 (320x240). The Cardputer sends small drawing commands over a 5 Mbaud UART with RTS/CTS flow
control. The C6 renders them into its own framebuffer and sends only the changed areas to the panel, so
the Cardputer's CPU stays almost idle (well under 1 ms per frame in most scenes).

```
 Cardputer-ADV (ESP32-S3)                    XIAO ESP32-C6                          ILI9341 320x240
+---------------------------+  UART 5 Mbaud  +--------------------------------+  SPI  +-------------+
| your app                  |  RTS/CTS       | parser -> renderer             | 80MHz |             |
|   RemoteDisplay library --+--------------->|   framebuffer (320x240 RGB565) +------>|   panel     |
|   (commands, batching,    |<---------------+   asset pool 160 KB (images,   |       |             |
|    uploads, recovery)     |  FRAME_DONE,   |   palettes, tilemaps)          |       +-------------+
| SD card: art, apps        |  ACK/NACK, ... |   dirty rectangles -> flush    |
+---------------------------+                +--------------------------------+
```

- `coproc-c6/`: C6 firmware (PlatformIO, pioarduino, LovyanGFX)
- `cardputer/`: the `RemoteDisplay` library (`lib/RemoteDisplay`) and example apps (`src/`) (PlatformIO,
  M5Unified/M5Cardputer)
- `shared/`: protocol definitions and the link layer, used by both sides
- `docs/protocol.md`: wiring, frame format and every command
- `tools/make_sample_art.py`: generates the sample art; `tools/readser.py`: records the C6's USB log

## Wiring

Cardputer-ADV EXT header (2x7), as printed on the board, counted from the right end:

```
                 7th   6th   5th   4th   3rd    2nd   1st (right end)
upper row:       G5    G39   G14   G40   G6     G4    G3
lower row:       G15   G13   G9    G8    5VOUT  GND   5VIN
```

| Cardputer EXT | XIAO ESP32-C6 | Signal |
|---|---|---|
| G13 | D7 (GPIO17) | Cardputer TX → C6 RX |
| G15 | D6 (GPIO16) | C6 TX → Cardputer RX |
| G4 | D5 (GPIO23) | Cardputer RTS → C6 CTS |
| G6 | D9 (GPIO20) | C6 RTS → Cardputer CTS |
| GND | GND | ground |
| 5VOUT, through a Schottky diode | 5V | power (see below) |

Panel (ILI9341 module) on the XIAO: D8 SCK, D10 MOSI (SDI), D0 CS, D1 DC, D2 RESET, D3 backlight (LED), 3V3,
GND. The panel's MISO (SDO) is not wired.

Watch out: G6 sits directly above 5VOUT and G4 directly above GND. A wire one row off puts 5 V on a C6 pin,
which is not 5 V tolerant. Never use 5VIN.

### Power: the 5 V wire

The XIAO is powered from the Cardputer, with a wire from **5VOUT** to the XIAO's **5V** pin through a
**Schottky diode** (1N5817 or SS14):

```
Cardputer 5VOUT ---->|---- XIAO 5V
                 anode  cathode (the band on the diode faces the XIAO)
```

Why the diode: the XIAO's 5V pin is connected straight to its USB VBUS. Without the diode, the XIAO's USB
supply and the Cardputer's 5VOUT are tied together whenever the XIAO is also on USB (for flashing or
logging). That is what made the XIAO drop off USB when a plain wire was tried. The diode lets current flow
only from the Cardputer to the XIAO, so the PC's 5 V never feeds back into the Cardputer. It costs about
0.3–0.45 V, which the XIAO's 3.3 V regulator does not mind.

Installed 2026-11-24. All the tests and measurements in this README (draw_test, stress_test, render_bench,
tilemap_demo) ran with this wire connected and the XIAO also plugged into USB at the same time: the XIAO stayed
on USB for flashing and logging, and nothing reset.

## Building and flashing

Build and flash from **PowerShell** (pioarduino refuses Git Bash/MSYS shells).

**C6 coprocessor**, over its USB-C port:

```
cd coproc-c6
pio run -e xiao_c6 -t upload --upload-port COM13
```

| C6 target | What it is |
|---|---|
| `xiao_c6` | the normal firmware (5 Mbaud link, 80 MHz panel SPI) |
| `xiao_c6_2m` | the same with the link at 2 Mbaud (pair with the Cardputer's `link_test_2m`) |
| `xiao_c6_40mhz` | the same with the panel SPI at 40 MHz (for long or noisy panel wires) |
| `selftest` | rendering self-test: logs C6 timings to USB every 5 s, no Cardputer needed |

**Cardputer apps** are not flashed over USB: that would overwrite the launcher. Build them, copy the
binary to the SD card's `firmwares/` folder, and start it from the launcher:

```
cd cardputer
pio run -e render_bench
copy .pio\build\render_bench\firmware.bin E:\firmwares\screendriver-render-bench.bin
```

| App target | File on the SD card | What it shows |
|---|---|---|
| `link_test` | `screendriver-link-test.bin` | link layer: HELLO, ping, throughput, flow control, error recovery |
| `draw_test` | `screendriver-draw-test.bin` | every primitive in a labelled box, plus automatic checks (key m: the M6 page) |
| `hello_text` | `screendriver-hello-text.bin` | fonts, sizes, alignment, colors, and a console you type into |
| `ui_demo` | `screendriver-ui-demo.bin` | a live UI that redraws only the widgets that change |
| `render_bench` | `screendriver-render-bench.bin` | timed scenes; results also appended to `/screendriver/bench.txt` |
| `sprite_demo` | `screendriver-sprite-demo.bin` | tiled background, up to 200 animated sprites, a rotating ship |
| `stress_test` | `screendriver-stress-test.bin` | fuzz, unplug, reboot, corruption, watchdog and long-run tests |
| `tilemap_demo` | `screendriver-tilemap-demo.bin` | a scrolling world 4 screens wide in one command per frame |

`sprite_demo` and `tilemap_demo` need the sample art in `/screendriver/` on the SD card (see below).

Both sides must use the same link speed: `SD_LINK_BAUD` in `shared/sd_protocol.h` (5 000 000). The
library refuses a C6 with a different protocol version or a firmware older than `RD_MIN_C6_FW` (0x0600).

## Using RemoteDisplay

```cpp
#include <M5Cardputer.h>
#include "RemoteDisplay.h"

RemoteDisplay ext;

void setup() {
  M5Cardputer.begin();
  if (!ext.begin()) Serial.println(ext.lastError());  // resets the C6 and waits for it
}

void loop() {
  ext.startFrame();
  ext.fillRect(10, 10, 100, 40, TFT_BLUE);
  ext.setFont(RD_FONT_FONT4);
  ext.setTextColor(TFT_WHITE, TFT_BLUE);
  ext.drawString("Hello", 20, 18);
  ext.endFrame();  // waits for the previous frame, then the C6 flushes the changed areas
}
```

Colors are RGB565: M5GFX's `TFT_*` constants and `RemoteDisplay::color565(r, g, b)` work.

### Frames

- Nothing reaches the panel until `endFrame()`. It blocks until the previous frame is done (at most one
  frame in flight). `endFrame(true)` flushes the whole screen.
- `lastFrame()` gives `fps`, the C6's `renderUs` and `flushUs`, and `waitUs` (how long `endFrame()` waited).
- `begin(rotation)`: 1 or 3, both landscape. `setBrightness(level)` sets the backlight.

### Drawing

- Shapes: `fillScreen`, `fillRect`, `drawRect`, `drawLine`, `drawFastHLine`, `drawFastVLine`,
  `drawCircle`, `fillCircle`, `drawRoundRect`, `fillRoundRect`, `drawTriangle`, `fillTriangle`.
- `drawPixel`: consecutive pixels of one color are sent as one command (up to 255).
- Clipping: `setClipRect(x, y, w, h)`, `clearClipRect()`. Applies to everything, including sprites and tilemaps.
- Text: `setFont(RD_FONT_*)`, `setTextSize`, `setTextColor(fg)` (transparent background) or
  `setTextColor(fg, bg)`, `setTextDatum` (M5GFX values such as `MC_DATUM`), `drawString`.
  Console style: `setCursor`, `print`, `println`, `printf` (wraps at the right edge).
- Fonts: `RD_FONT_GLCD` (6x8), `RD_FONT_FONT2` (16 px), `RD_FONT_FONT4` (26 px), `RD_FONT_FONT7`
  (7-segment digits, 48 px), `RD_FONT_SANS9`, `RD_FONT_SANSB12`.
- `pushImage(x, y, w, h, pixels)` draws pixels from RAM without storing them; `copyRect` moves a screen area.

### Images and sprites

```cpp
ext.loadImage(0, "/screendriver/ship.png");             // PNG or BMP from the SD card, uploaded once
ext.loadSheet(1, "/screendriver/coin.png", 16, 16);     // sprite sheet: 16x16 frames, numbered row by row
ext.drawSprite(1, frame, x, y);                          // transparent by default
ext.drawSprite(1, frame, x, y, RD_TRANSPARENT | RD_FLIP_X);
ext.drawSpriteEx(0, 0, 160, 120, angleDeg, 2.0f, 2.0f);  // rotated and scaled around its center
```

- Ids 0–63 share a 160 KB pool on the C6 (`assetPoolFree()`). `freeAsset(id)` and `freeAllAssets()` give
  the space back; the pool compacts, so it never fragments.
- `uploadImage(id, pixels, w, h[, frameW, frameH, transparent])` uploads RGB565 pixels from RAM or flash.
  Keep the array alive: `restore()` re-sends it after a C6 reboot.
- Transparency: PNG transparency becomes the transparent color automatically. Otherwise pixels of the
  transparent color (default magenta `0xF81F`) are transparent, so opaque magenta in a PNG is too.
- The Cardputer needs `w*h*2` bytes of free RAM while loading an image (about 100 KB max).
- Queries: `hasAsset`, `frameCount`, `frameWidth`, `frameHeight`, `isIndexed`, `assetBytes`.
- **Sprite batches:** consecutive `drawSprite()` calls go out as one command (up to 127 sprites); any other
  command sends the batch first, so the drawing order is kept. `setSpriteBatch(n)` lowers the batch size
  (1 = one command per sprite). There is normally no reason to change it.

### Palette images (`setImageFormat`)

Images are stored in one of two formats on the C6. `setImageFormat()` applies to the uploads that follow:

| Format | Stored as | Use |
|---|---|---|
| `RD_FORMAT_AUTO` (default) | palette if the image has ≤ 256 colors, else RGB565 | most art |
| `RD_FORMAT_RGB565` | 2 bytes per pixel | the fastest drawing |
| `RD_FORMAT_INDEXED` | a palette (≤ 256 colors) + 1 byte per pixel; refused above 256 colors | the least memory |

A palette image takes about half the pool space, but each pixel costs a color lookup on the C6. Use
RGB565 for large opaque images drawn every frame (backgrounds, tilesets): a full screen of tiles renders in
3.4 ms from RGB565 tiles and 4.9 ms from palette tiles.

```cpp
ext.setImageFormat(RD_FORMAT_RGB565);
ext.loadSheet(0, "/screendriver/tiles.png", 16, 16);
ext.setImageFormat(RD_FORMAT_AUTO);
```

### Tilemaps

A tilemap is a grid of tile numbers (1 byte each) stored on the C6. Any image or sheet is its tileset:
tile *n* is frame *n*; `RD_TILE_EMPTY` (0xFF) is never drawn. One command draws a whole layer.

```cpp
static uint8_t world[80 * 20];                          // the app keeps the array alive
ext.loadSheet(0, "/screendriver/tiles.png", 16, 16);    // tileset
ext.uploadTilemap(1, world, 80, 20);                    // map: 80 x 20 tiles
ext.setTile(1, tx, ty, 3);                              // change one tile (updates the C6 and `world`)
ext.drawTilemap(1, 0, scrollX, scrollY, 0, 16, 320, 224, RD_WRAP);  // map, tileset, scroll, screen rect, flags
```

- Map pixel (`scrollX`, `scrollY`) lands at the top-left of the screen rectangle. Negative scroll is fine.
- Flags: `RD_WRAP` repeats the map in both directions; `RD_TRANSPARENT` skips the tileset's transparent
  pixels (for a layer over another). Without `RD_WRAP`, areas outside the map are left as they are.
- `getTile(map, tx, ty)` reads the app's copy.

### Robustness

The library recovers by itself; the app never freezes:

- **Link lost** (connector off, wires loose): sends never block. Drawing is dropped while the link is down
  (`connected()` is false) and the app keeps running. The library pings every 250 ms.
- **Link back, C6 rebooted** (reset button, power, watchdog): the C6 is reset, every image and tilemap is
  re-uploaded (from the SD card or RAM), and the screen is redrawn. `health()` has the counters and timings.
- **Corrupted command** (CRC error): the redraw callback repairs the screen at the next frame.
- **C6 hang**: its watchdog reboots it after 3 s, then the above applies.

```cpp
ext.onRedraw([] {          // called inside startFrame() after a recovery or a lost command
  ext.fillScreen(TFT_BLACK);
  drawBackgroundAndLabels();
});
```

Apps that redraw the whole screen every frame don't need a callback. `wasReset()` returns true once after a
recovery, for apps that draw outside `startFrame()`/`endFrame()`. Apps that don't draw every frame should
call `ext.poll()` in their loop so the checks run. `getStatus()` returns the C6's link counters.

Tips: draw only what changed each frame; small updates flush in well under a millisecond. Prefer
`drawString` with a background color, or `fillRect` + transparent text, to clear old text.

## Art pipeline

```
python tools/make_sample_art.py sample_art
```

writes the sample art (all original, simple shapes): `tile.png`, `coin.png` (8 frames), `slime.png`
(4 frames), `ship.png`, `ball.bmp` and `tiles.png` (8 tiles: sky, grass, dirt, brick, stone, water, cloud,
flower). It needs Pillow (`pip install pillow`). Copy the folder to the SD card as `/screendriver/`.

For your own art:

- PNG (with or without transparency) or BMP (16 or 24-bit).
- Sprite sheets: frames of equal size, numbered row by row from the top-left; the sheet size must be a
  multiple of the frame size.
- Pixel art with ≤ 256 colors is stored as a palette image automatically (half the memory).
- Keep each image under ~100 KB of RGB565 (w*h*2) so the Cardputer can load it.

## Performance

Measured with `render_bench` (5 Mbaud link, 80 MHz panel SPI, C6 firmware 0x0600, 2026-11-24):

| Scene | fps | C6 render + flush (ms) | Cardputer CPU (ms) |
|---|---|---|---|
| full-screen fill | 61 | 0.7 + 15.1 | 0.35 |
| 100 random rectangles | 57 | 1.3 + 15.1 | 0.69 |
| 12 lines of text with background | 39 | 9.7 + 15.1 | 0.77 |
| small moving 32x32 shape | ~1990 | 0.1 + 0.3 | 0.09 |
| 1000 pixels | 59 | 0.7 + 15.2 | 1.33 |
| clear + 50 sprites | 59 | 1.4 + 15.1 | 0.41 |
| clear + 200 sprites (palette) | 49.5 | 4.3 + 15.1 | 0.63 |
| clear + 200 sprites (RGB565) | 53.5 | 2.8 + 15.1 | 0.61 |
| full-screen tilemap (palette tiles) | 49 | 4.9 + 15.1 | 0.44 |
| full-screen tilemap (RGB565 tiles) | 52.6 | 3.4 + 15.1 | 0.41 |
| `tilemap_demo` (tiles + HUD + sprites) | 49 | | |

- **The panel sets the ceiling.** Sending a full screen to the ILI9341 takes 15.1 ms (320x240x2 bytes at
  80 MHz SPI), so full-screen updates top out around 60 fps. Rendering and flushing run one after the other
  (there is no RAM for a second framebuffer), so 50 fps needs a render time under ~4.9 ms. Small updates
  flush only their area and are much faster.
- Frame time = C6 render + flush + ~0.7 ms. The Cardputer is never the bottleneck.
- Sprite batches matter: 200 sprites sent one command each run at 46.5 fps and cost the Cardputer 50 % more
  CPU.

## Troubleshooting

- **The C6 screen says "waiting for the Cardputer..."**: it shows live link counters until the first command.
  "Cardputer ready (D5): no" means the Cardputer app isn't running or the G4 → D5 wire is off.
- **`begin()` fails / no HELLO**: run `link_test`; it checks HELLO, ping, throughput and both flow-control
  wires, and says which wire is wrong. "flow control: OFF" on the C6 screen means its CTS input never went
  low (G4 → D5 missing).
- **"protocol version mismatch" / "C6 firmware ... is too old"**: flash the C6 with the firmware from this
  repo.
- **C6 log**: `python tools/readser.py SER=90:64:9B:AB:49:08 600` (or `COM13 600`) records the USB log; it
  is safe to leave attached while benchmarking. Every 2 s it prints the link counters (`crc`, `ovf` must stay
  0; `bp` counts flow-control pauses, which lose nothing) and the link wait per frame: high means the C6 waits
  for the Cardputer, low means the C6 is the bottleneck.
- **Is the C6 rendering slow?** Flash the `selftest` target: it times fills, text, sprites (palette and
  RGB565, batched and single) and tilemaps on the C6 alone, and checks the tile paths pixel for pixel.
  Flash `xiao_c6` again afterwards.
- **Benchmark results**: `render_bench` appends every run to `/screendriver/bench.txt` on the SD card.
- **Garbled colors or a frozen panel** with long panel wires: try `xiao_c6_40mhz`.
- **Frequent link errors**: try the 2 Mbaud pair `xiao_c6_2m` + `link_test_2m` to tell wiring from speed.
