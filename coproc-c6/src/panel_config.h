// LovyanGFX configuration for the ILI9341 wired to the Seeed XIAO ESP32-C6.
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#ifndef PANEL_SPI_HZ
#define PANEL_SPI_HZ 40000000
#endif

// XIAO ESP32-C6 pins -> ILI9341
#define PIN_LCD_SCK   19  // D8
#define PIN_LCD_MOSI  18  // D10
#define PIN_LCD_CS     0  // D0
#define PIN_LCD_DC     1  // D1
#define PIN_LCD_RST    2  // D2
#define PIN_LCD_BL    21  // D3 (drives the module's backlight transistor)

class LGFX_ILI9341 : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;

 public:
  LGFX_ILI9341() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = PANEL_SPI_HZ;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_LCD_SCK;
      cfg.pin_mosi = PIN_LCD_MOSI;
      cfg.pin_miso = -1;  // not wired: we never read from the panel
      cfg.pin_dc = PIN_LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_LCD_CS;
      cfg.pin_rst = PIN_LCD_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = false;      // set true if colors appear inverted
      cfg.rgb_order = false;   // set true if red and blue are swapped
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = PIN_LCD_BL;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 0;  // the C6 has LEDC channels 0-5 only
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};
