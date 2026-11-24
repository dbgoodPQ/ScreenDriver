# Example: adding the external screen to an existing app

A small stand-alone Cardputer app, set up the way an existing app adds ScreenDriver. It is a separate
PlatformIO project (not a target of `cardputer/`), so its `platformio.ini` shows exactly what an app needs.

## What it shows

- **Second screen** (at start): the external screen shows a temperature and a message counter. The labels
  are drawn once, and only the values that change are redrawn.
- **Mirror** (any key): the app draws into an `M5Canvas` that appears on the Cardputer and, centered, on the
  external screen. Only the rows that changed are sent (about 5 fps when the whole screen changes, so it
  suits menus and text UIs, not games).
- **Works without the screen**: unplug the link or reset the XIAO; the app keeps running and the external
  screen comes back within 1–2 s.

## The setup an app needs

In `platformio.ini`, the library and the protocol headers:

```ini
lib_deps =
    symlink://../../cardputer/lib/RemoteDisplay        ; outside the repo: an absolute path
build_flags =
    -I$PROJECT_DIR/../../shared                      ; outside the repo: -IC:/workspaces/ScreenDriver/shared
```

In the code: `ext.begin(1, 1500)` after `M5Cardputer.begin()`, without a retry loop; drawing between
`ext.startFrame()` and `ext.endFrame()`; `ext.onRedraw(...)` for the static parts; `ext.poll()` when nothing is
drawn. Details are in the comments of `src/main.cpp` and in the main README.

## Build

From PowerShell:

```
cd examples/extscreen_app
pio run
copy .pio\build\extscreen_app\firmware.bin E:\firmwares\screendriver-extscreen-example.bin
```

Start it from the launcher on the SD card; never flash the Cardputer over USB.

Tested on the hardware (2026-11-24): second screen, mirror colors, reconnect after unplugging the link or
resetting the XIAO.
