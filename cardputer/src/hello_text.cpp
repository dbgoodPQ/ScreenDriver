// Milestone 3 text demo. Pages: fonts, sizes, alignment (datum), colors, console.
// Tab or / = next page, , = previous page. On the console page, typed text is printed on the external screen
// (Enter = new line, Del = clear); Tab leaves it.
#include "demo_common.h"

static RemoteDisplay ext;
static int page = 0;
static const int PAGES = 5;
static const char* const pageNames[PAGES] = {"fonts", "sizes", "alignment", "colors", "console"};

static void header(const char* title) {
  ext.fillScreen(TFT_BLACK);
  ext.fillRect(0, 0, 320, 18, TFT_NAVY);
  ext.setFont(RD_FONT_FONT2);
  ext.setTextSize(1);
  ext.setTextDatum(0);
  ext.setTextColor(TFT_WHITE);
  char s[64];
  snprintf(s, sizeof s, "%d/%d  %s", page + 1, PAGES, title);
  ext.drawString(s, 4, 1);
  ext.setTextDatum(2);
  ext.drawString(page == PAGES - 1 ? "Tab: next" : "<  ,   /  >", 316, 1);
  ext.setTextDatum(0);
}

static void pageFonts() {
  header("fonts");
  struct { uint8_t id; const char* name; } f[] = {
      {RD_FONT_GLCD, "GLCD 6x8: The quick brown fox"}, {RD_FONT_FONT2, "Font2: The quick brown fox"},
      {RD_FONT_FONT4, "Font4: Quick fox"},             {RD_FONT_SANS9, "FreeSans 9pt: The quick fox"},
      {RD_FONT_SANSB12, "FreeSansBold 12: Fox"},
  };
  int y = 26;
  for (auto& e : f) {
    ext.setFont(e.id);
    ext.setTextColor(TFT_WHITE);
    ext.drawString(e.name, 6, y);
    y += e.id == RD_FONT_GLCD ? 14 : e.id == RD_FONT_FONT2 ? 22 : 32;
  }
  ext.setFont(RD_FONT_FONT7);
  ext.setTextColor(TFT_GREEN);
  ext.drawString("12:34", 6, y + 2);
  ext.setFont(RD_FONT_GLCD);
  ext.setTextColor(TFT_DARKGREY);
  ext.drawString("Font7 (7-segment, digits)", 170, y + 20);
}

static void pageSizes() {
  header("text sizes (GLCD x1..x4, Font2 x2)");
  int y = 26;
  for (int s = 1; s <= 4; ++s) {
    ext.setFont(RD_FONT_GLCD);
    ext.setTextSize(s);
    ext.setTextColor(TFT_YELLOW);
    char t[24];
    snprintf(t, sizeof t, "size %d", s);
    ext.drawString(t, 6, y);
    y += 8 * s + 6;
  }
  ext.setFont(RD_FONT_FONT2);
  ext.setTextSize(2);
  ext.setTextColor(TFT_CYAN);
  ext.drawString("Font2 x2", 6, y);
  ext.setTextSize(1);
}

static void pageDatum() {
  header("alignment (datum) around each cross");
  static const struct { uint8_t d; const char* name; } ds[] = {
      {0, "TL"}, {1, "TC"}, {2, "TR"}, {4, "ML"}, {5, "MC"}, {6, "MR"}, {8, "BL"}, {9, "BC"}, {10, "BR"}};
  ext.setFont(RD_FONT_FONT2);
  for (int i = 0; i < 9; ++i) {
    int x = 60 + (i % 3) * 100, y = 60 + (i / 3) * 65;
    ext.drawFastHLine(x - 8, y, 17, TFT_RED);
    ext.drawFastVLine(x, y - 8, 17, TFT_RED);
    ext.setTextDatum(ds[i].d);
    ext.setTextColor(TFT_WHITE);
    ext.drawString(ds[i].name, x, y);
  }
  ext.setTextDatum(0);
}

static void pageColors() {
  header("colors: transparent vs filled background");
  for (int x = 0; x < 320; x += 20) ext.fillRect(x, 22, 10, 218, TFT_DARKGREEN);
  ext.setFont(RD_FONT_FONT4);
  ext.setTextColor(TFT_WHITE);
  ext.drawString("transparent", 10, 40);
  ext.setTextColor(TFT_WHITE, TFT_MAROON);
  ext.drawString("filled bg", 10, 80);
  const uint16_t cs[] = {TFT_RED, TFT_ORANGE, TFT_YELLOW, TFT_GREEN, TFT_CYAN, TFT_BLUE, TFT_MAGENTA};
  ext.setFont(RD_FONT_FONT2);
  int x = 10;
  for (uint16_t c : cs) {
    ext.setTextColor(c, TFT_BLACK);
    ext.drawString("Color", x, 130);
    x += 44;
  }
  ext.setTextColor(RemoteDisplay::color565(255, 128, 0));
  ext.drawString("color565(255,128,0)", 10, 160);
}

static void pageConsole() {
  header("console: type here (Enter, Del = clear)");
  ext.setFont(RD_FONT_FONT2);
  ext.setTextSize(1);
  ext.setTextColor(TFT_GREEN, TFT_BLACK);
  ext.setCursor(0, 22);
  ext.println("print() wraps long lines at the right edge of the screen, like a terminal.");
  ext.printf("printf: millis=%lu, pi=%.3f\n", (unsigned long)millis(), 3.14159);
  ext.setTextColor(TFT_WHITE, TFT_BLACK);
  ext.print("> ");
}

static void drawPage() {
  ext.startFrame();
  switch (page) {
    case 0: pageFonts(); break;
    case 1: pageSizes(); break;
    case 2: pageDatum(); break;
    case 3: pageColors(); break;
    default: pageConsole(); break;
  }
  ext.endFrame();
  localLog(TFT_WHITE, "page %d: %s", page + 1, pageNames[page]);
}

void setup() {
  demoSetup("ScreenDriver text demo");
  connectExternal(ext);
  localLog(TFT_DARKGREY, "Tab or / = next, , = previous");
  drawPage();
}

void loop() {
  Keys k = readKeys();
  if (k.any) {
    bool console = page == PAGES - 1;
    if (k.tab || (!console && k.right)) {
      page = (page + 1) % PAGES;
      drawPage();
    } else if (!console && k.left) {
      page = (page + PAGES - 1) % PAGES;
      drawPage();
    } else if (console) {
      ext.startFrame();
      if (k.del) {
        pageConsole();
      } else {
        if (k.text.length()) ext.print(k.text.c_str());
        if (k.enter) ext.print("\n> ");
      }
      ext.endFrame();
    }
  }
  if (ext.wasReset()) drawPage();  // the library recovered the link: redraw
  delay(5);
}
