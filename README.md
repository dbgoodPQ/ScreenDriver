# ScreenDriver

A Seeed XIAO ESP32-C6 used as a graphics coprocessor for the M5Stack Cardputer-ADV. It drives a 2.8"
ILI9341 (320x240). The Cardputer sends small drawing commands over a 5 Mbaud UART with RTS/CTS flow
control. The C6 renders them into its own framebuffer and sends only the changed areas to the panel, so
the Cardputer's CPU stays almost idle.

- `coproc-c6/`: C6 firmware (PlatformIO, pioarduino, LovyanGFX)
- `cardputer/`: Cardputer library `RemoteDisplay` and example apps (PlatformIO, M5Unified/M5Cardputer)
- `shared/`: protocol definitions and the link layer, used by both sides
- `docs/protocol.md`: wiring, frame format and every command
- `tools/make_sample_art.py`: generates the sample sprites for `sprite_demo`

## Wiring (summary; details in `docs/protocol.md`)

| Cardputer EXT | XIAO |
|---|---|
| G13 (TX) | D7 |
| G15 (RX) | D6 |
| G4 (RTS) | D5 |
| G6 (CTS) | D9 |
| GND | GND |
| 5VOUT, through a Schottky diode | 5V (optional, see the note in the protocol doc) |

Panel: D8 SCK, D10 MOSI, D0 CS, D1 DC, D2 RST, D3 backlight, 3V3, GND.

## Building

Build and flash from **PowerShell** (pioarduino refuses Git Bash/MSYS shells).

```
cd coproc-c6;  pio run -e xiao_c6 -t upload      # C6 coprocessor firmware
cd cardputer;  pio run -e ui_demo                # a Cardputer app -> .pio/build/ui_demo/firmware.bin
```

Cardputer apps are copied to the SD card (`firmwares/`) and started from the launcher, instead of being
flashed over USB, so the launcher stays installed.

Both sides must use the same link speed: `SD_LINK_BAUD` in `shared/sd_protocol.h` (5 000 000).
The `xiao_c6_2m` / `link_test_2m` targets are a 2 Mbaud fallback.

## Using RemoteDisplay

```cpp
#include "RemoteDisplay.h"

RemoteDisplay ext;

void setup() {
  M5.begin();
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

- Shapes: `fillScreen`, `fillRect`, `drawRect`, `drawLine`, `drawFastHLine/VLine`, `draw/fillCircle`,
  `draw/fillRoundRect`, `draw/fillTriangle`, `drawPixel` (batched), `setClipRect`, `clearClipRect`.
- Text: `setFont(RD_FONT_*)`, `setTextSize`, `setTextColor(fg[, bg])`, `setTextDatum`, `drawString`,
  plus console-style `setCursor`, `print`, `println`, `printf` (wraps at the right edge).
- Frames: nothing reaches the panel until `endFrame()`. It blocks until the previous frame is done (at most
  one frame in flight). `lastFrame()` gives fps and the C6's render/flush times.
- `wasReset()` tells you if the C6 rebooted (call `begin()` again and redraw). `getStatus()` returns the
  link counters.
- Colors are RGB565: M5GFX's `TFT_*` constants and `RemoteDisplay::color565()` work.

### Sprites and images (milestone 4)

```cpp
ext.loadImage(0, "/screendriver/ship.png");             // PNG or BMP from the SD card, uploaded once
ext.loadSheet(1, "/screendriver/coin.png", 16, 16);     // sprite sheet: 16x16 frames
ext.drawSprite(1, frame, x, y);                          // transparent by default
ext.drawSprite(1, frame, x, y, RD_TRANSPARENT | RD_FLIP_X);
ext.drawSpriteEx(0, 0, 160, 120, angleDeg, 2.0f, 2.0f);  // rotated and scaled around its center
```

- Ids 0–63, 160 KB pool on the C6 (`assetPoolFree()`). `freeAsset(id)` gives the space back.
- PNG transparency becomes the transparent color automatically. For BMP, pixels of the transparent color
  (default magenta `0xF81F`) are transparent. Opaque magenta in a PNG is transparent too.
- The Cardputer needs `w*h*2` bytes of RAM while loading an image (about 100 KB max).
- `uploadImage()` sends pixels from RAM, `pushImage()` draws pixels without storing them,
  `copyRect()` moves a screen area.
- If the C6 reboots (`wasReset()`), `restore()` re-uploads every asset; then redraw.
- Sample art: `python tools/make_sample_art.py <folder>`, then copy the folder to the SD card as
  `/screendriver/`.

### Robustness (milestone 5)

The library recovers by itself; the app never freezes:

- **Link lost** (connector off, wires loose): sends never block. Drawing is dropped while the link is down
  (`connected()` is false) and the app keeps running. The library pings every 250 ms.
- **Link back, C6 rebooted** (reset button, power, watchdog): the C6 is reset, the images are re-uploaded
  (from the SD card or RAM), and the screen is redrawn. `health()` has the counters and timings.
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
call `ext.poll()` in their loop so the checks run.

Tips: draw only what changed each frame; small updates flush in well under a millisecond. Prefer
`drawString` with a background color, or `fillRect` + transparent text, to clear old text.

## Example apps (`cardputer/src/`)

| Target | What it shows |
|---|---|
| `link_test` | Link layer: HELLO, ping, throughput, flow control, error recovery |
| `draw_test` | Every primitive in a labelled box, plus automatic checks |
| `hello_text` | Fonts, sizes, alignment, colors, and a console you type into |
| `ui_demo` | A live UI that redraws only the widgets that change |
| `render_bench` | Timed scenes: fps, C6 render/flush time, Cardputer CPU time |
| `stress_test` | Fuzz, unplug, reboot, corruption, watchdog and long-run tests (keys 1-6, 0 stops) |
| `sprite_demo` | Tiled background, up to 200 animated sprites, a rotating ship (needs `/screendriver/` art) |

Measured (`render_bench`, 5 Mbaud, 80 MHz SPI): full-screen redraw 57–62 fps (flush 15.1 ms);
12 lines of text 39 fps; small 32x32 sprite ~1900 fps; 50 sprites + clear 58 fps, 200 sprites 47 fps;
Cardputer CPU 0.04–1.3 ms per frame.

Benchmarking note: a PC program holding the C6's USB serial port open used to cost the C6 ~57% of its CPU
(an interrupt on every USB poll). The firmware now disables that interrupt; logging still works.

Hardware test apps (`pin_test`, `fc_idle`, `rts_probe`) were used to debug the flow-control wiring.
