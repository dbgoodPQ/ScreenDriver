// Shared helpers for the milestone 3 example apps: Cardputer setup, keyboard, local log, connecting to the C6.
#pragma once
#include <M5Cardputer.h>
#include <stdarg.h>
#include "RemoteDisplay.h"

// Keyboard snapshot of one key press. The Cardputer's arrow keys are ; (up) . (down) , (left) / (right).
struct Keys {
  bool any = false;
  bool enter = false, del = false, tab = false;
  bool up = false, down = false, left = false, right = false;
  String text;  // printable characters typed
};

inline Keys readKeys() {
  Keys k;
  M5Cardputer.update();
  if (!(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())) return k;
  Keyboard_Class::KeysState s = M5Cardputer.Keyboard.keysState();
  k.any = true;
  k.enter = s.enter;
  k.del = s.del;
  k.tab = s.tab;
  for (char c : s.word) {
    if (c == ';') k.up = true;
    else if (c == '.') k.down = true;
    else if (c == ',') k.left = true;
    else if (c == '/') k.right = true;
    k.text += c;
  }
  return k;
}

// Prints a line on the Cardputer's own screen and the USB serial log.
inline void localLog(uint16_t color, const char* fmt, ...) {
  char s[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s, sizeof s, fmt, ap);
  va_end(ap);
  Serial.println(s);
  M5Cardputer.Display.setTextColor(color, TFT_BLACK);
  M5Cardputer.Display.println(s);
}

inline void demoSetup(const char* title) {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);  // true: enable the keyboard
  Serial.begin(115200);
  auto& d = M5Cardputer.Display;  // M5GFX already sets it up as 240x135 landscape: don't rotate it
  d.setFont(&fonts::Font0);
  d.setTextScroll(true);
  d.fillScreen(TFT_BLACK);
  d.setCursor(0, 0);
  localLog(TFT_CYAN, "%s", title);
}

// Connects to the C6, retrying until it answers. Shows progress on the Cardputer screen.
inline void connectExternal(RemoteDisplay& ext) {
  for (;;) {
    localLog(TFT_WHITE, "connecting to the C6...");
    if (ext.begin()) {
      localLog(TFT_GREEN, "C6 fw %04X, %dx%d", ext.link().hello.fw, ext.width(), ext.height());
      return;
    }
    localLog(TFT_RED, "failed: %s", ext.lastError());
    delay(1000);
  }
}
