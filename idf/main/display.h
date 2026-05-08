#pragma once
#include <cstring>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>


// ST7789 commands
#define ST7789_SWRESET 0x01
#define ST7789_SLPOUT 0x11
#define ST7789_NORON 0x13
#define ST7789_INVON 0x21
#define ST7789_DISPON 0x29
#define ST7789_CASET 0x2A
#define ST7789_RASET 0x2B
#define ST7789_RAMWR 0x2C
#define ST7789_MADCTL 0x36
#define ST7789_COLMOD 0x3A

// Color definitions (RGB565)
#define ST77XX_BLACK 0x0000
#define ST77XX_WHITE 0xFFFF
#define ST77XX_RED 0xF800
#define ST77XX_GREEN 0x07E0
#define ST77XX_BLUE 0x001F
#define ST77XX_CYAN 0x07FF
#define ST77XX_MAGENTA 0xF81F
#define ST77XX_YELLOW 0xFFE0
#define ST77XX_ORANGE 0xFD20
#define ST77XX_LIGHTGREY 0x8410
#define ST77XX_ACCENT 0xFF8E // #FFEE8C in RGB565

class Display {
public:
  Display();
  void init();
  void fillScreen(uint16_t color);
  void drawPixel(int16_t x, int16_t y, uint16_t color);
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color);
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color);
  void drawBitmap(int16_t x, int16_t y, const uint8_t *bitmap, int16_t w,
                  int16_t h, uint16_t color);
  void setCursor(int16_t x, int16_t y);
  void setTextColor(uint16_t c, uint16_t bg = ST77XX_BLACK);
  void setTextSize(uint8_t s);
  void setFont(const void *font = nullptr); // we'll implement basic fonts later
  void print(const char *text);
  void getTextBounds(const char *text, int16_t x, int16_t y, int16_t *x1,
                     int16_t *y1, uint16_t *w, uint16_t *h);
  void fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);
  void setRotation(uint8_t r);
  static const int WIDTH = 240;
  static const int HEIGHT = 240;

private:
  spi_device_handle_t spi;
  uint16_t *line_buffer; // for faster rendering
  void writeCommand(uint8_t cmd);
  void writeData(uint8_t data);
  void writeData16(uint16_t data);
  void setAddrWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
  void writeColor(uint16_t color, uint32_t len);

  // Text rendering state
  int16_t cursor_x, cursor_y;
  uint16_t textcolor, textbgcolor;
  uint8_t textsize;
  bool wrap;
};

extern Display display;