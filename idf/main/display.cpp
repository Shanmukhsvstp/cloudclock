#include "display.h"
#include <esp_log.h>
#include <cstring>
#include <algorithm>

static const char* TAG = "Display";

// Pin definitions (from your original code)
#define TFT_CLK   12
#define TFT_MOSI  11
#define TFT_DC    9
#define TFT_RST   3
#define TFT_CS   -1  // not used, we'll use software CS or tie low

Display::Display() : line_buffer(nullptr) {}

void Display::init() {
    ESP_LOGI(TAG, "Initializing ST7789 display");
    
    // Initialize SPI bus
    spi_bus_config_t buscfg = {
        .mosi_io_num = TFT_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = TFT_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = WIDTH * HEIGHT * 2 + 8
    };
    
    spi_device_interface_config_t devcfg = {
        .mode = 0,                     // SPI mode 0
        .clock_speed_hz = 40 * 1000 * 1000, // 40 MHz
        .spics_io_num = TFT_CS,
        .queue_size = 7,
        .flags = SPI_DEVICE_HALFDUPLEX
    };
    
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spi));
    
    // Initialize control pins
    gpio_set_direction((gpio_num_t)TFT_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)TFT_RST, GPIO_MODE_OUTPUT);
    
    // Hardware reset
    gpio_set_level((gpio_num_t)TFT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level((gpio_num_t)TFT_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)TFT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    
    // Initialization sequence (simplified, but functional)
    writeCommand(ST7789_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(150));
    writeCommand(ST7789_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(120));
    writeCommand(ST7789_NORON);
    writeCommand(ST7789_INVON);  // ST7789 usually needs inversion on
    writeCommand(ST7789_MADCTL);
    writeData(0x00); // rotation 0 (adjust if needed)
    writeCommand(ST7789_COLMOD);
    writeData(0x55); // 16-bit color
    writeCommand(ST7789_DISPON);
    vTaskDelay(pdMS_TO_TICKS(120));
    
    // Allocate line buffer for fast fills
    line_buffer = (uint16_t*)heap_caps_malloc(WIDTH * sizeof(uint16_t), MALLOC_CAP_DMA);
    assert(line_buffer);
    
    fillScreen(ST77XX_BLACK);
    setTextColor(ST77XX_WHITE);
    setTextSize(1);
    cursor_x = cursor_y = 0;
    wrap = true;
}

void Display::writeCommand(uint8_t cmd) {
    gpio_set_level((gpio_num_t)TFT_DC, 0);
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd
    };
    spi_device_transmit(spi, &t);
}

void Display::writeData(uint8_t data) {
    gpio_set_level((gpio_num_t)TFT_DC, 1);
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &data
    };
    spi_device_transmit(spi, &t);
}
void Display::fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
    int16_t f = 1 - r;
    int16_t ddF_x = 1;
    int16_t ddF_y = -2 * r;
    int16_t x = 0;
    int16_t y = r;
    drawFastVLine(x0, y0 - r, 2 * r + 1, color);
    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;
        drawFastVLine(x0 + x, y0 - y, 2 * y + 1, color);
        drawFastVLine(x0 + y, y0 - x, 2 * x + 1, color);
        drawFastVLine(x0 - x, y0 - y, 2 * y + 1, color);
        drawFastVLine(x0 - y, y0 - x, 2 * x + 1, color);
    }
}

void Display::setRotation(uint8_t m) {
    // ST7789 rotation (adjust as needed)
    writeCommand(ST7789_MADCTL);
    switch (m) {
        case 0: writeData(0x00); break;
        case 1: writeData(0x60); break;
        case 2: writeData(0xC0); break;
        case 3: writeData(0xA0); break;
        default: break;
    }
}
void Display::writeData16(uint16_t data) {
    gpio_set_level((gpio_num_t)TFT_DC, 1);
    uint16_t swapped = __builtin_bswap16(data); // SPI sends MSB first? Actually depends on mode. We'll use direct.
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = &data
    };
    spi_device_transmit(spi, &t);
}

void Display::setAddrWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    writeCommand(ST7789_CASET);
    writeData16(x0);
    writeData16(x1);
    writeCommand(ST7789_RASET);
    writeData16(y0);
    writeData16(y1);
    writeCommand(ST7789_RAMWR);
}

void Display::writeColor(uint16_t color, uint32_t len) {
    if (len == 0) return;
    // Fill line buffer with color
    for (uint32_t i = 0; i < len; i++) {
        line_buffer[i] = color;
    }
    gpio_set_level((gpio_num_t)TFT_DC, 1);
    spi_transaction_t t = {
        .length = len * 16,
        .tx_buffer = line_buffer
    };
    spi_device_transmit(spi, &t);
}

void Display::fillScreen(uint16_t color) {
    setAddrWindow(0, 0, WIDTH-1, HEIGHT-1);
    writeColor(color, WIDTH * HEIGHT);
}

void Display::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    // clip
    if (x >= WIDTH || y >= HEIGHT) return;
    if (x + w - 1 >= WIDTH) w = WIDTH - x;
    if (y + h - 1 >= HEIGHT) h = HEIGHT - y;
    setAddrWindow(x, y, x + w - 1, y + h - 1);
    writeColor(color, w * h);
}

void Display::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
    fillRect(x, y, w, 1, color);
}

void Display::drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
    fillRect(x, y, 1, h, color);
}

void Display::drawPixel(int16_t x, int16_t y, uint16_t color) {
    setAddrWindow(x, y, x, y);
    writeData16(color);
}

void Display::drawBitmap(int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, uint16_t color) {
    // Simple 1-bit bitmap drawing (foreground color only)
    for (int16_t j = 0; j < h; j++) {
        for (int16_t i = 0; i < w; i++) {
            uint8_t byte = pgm_read_byte(bitmap + j * ((w + 7) / 8) + i / 8);
            if (byte & (0x80 >> (i & 7))) {
                drawPixel(x + i, y + j, color);
            }
        }
    }
}

// Basic 5x7 font (Adafruit GFX default)
#include "glcdfont.c" // We'll include the standard font data

void Display::setCursor(int16_t x, int16_t y) {
    cursor_x = x;
    cursor_y = y;
}

void Display::setTextColor(uint16_t c, uint16_t bg) {
    textcolor = c;
    textbgcolor = bg;
}

void Display::setTextSize(uint8_t s) {
    textsize = (s > 0) ? s : 1;
}

void Display::setFont(const void* font) {
    // For now only built-in font; later we can add GFXfont support.
}

void Display::print(const char* text) {
    while (*text) {
        if (*text == '\n') {
            cursor_y += textsize * 8;
            cursor_x = 0;
        } else {
            drawChar(cursor_x, cursor_y, *text, textcolor, textbgcolor, textsize);
            cursor_x += textsize * 6;
            if (wrap && (cursor_x > (WIDTH - textsize * 6))) {
                cursor_y += textsize * 8;
                cursor_x = 0;
            }
        }
        text++;
    }
}

void Display::drawChar(int16_t x, int16_t y, unsigned char c, uint16_t color, uint16_t bg, uint8_t size) {
    if (c < 32) return;
    c -= 32;
    for (int8_t i = 0; i < 5; i++) {
        uint8_t line = font[c * 5 + i];
        for (int8_t j = 0; j < 8; j++, line >>= 1) {
            if (line & 1) {
                if (size == 1) drawPixel(x + i, y + j, color);
                else fillRect(x + i * size, y + j * size, size, size, color);
            } else if (bg != color) {
                if (size == 1) drawPixel(x + i, y + j, bg);
                else fillRect(x + i * size, y + j * size, size, size, bg);
            }
        }
    }
    if (bg != color) {
        if (size == 1) fillRect(x + 5, y, 1, 8, bg);
        else fillRect(x + 5 * size, y, size, 8 * size, bg);
    }
}

void Display::getTextBounds(const char* text, int16_t x, int16_t y, int16_t* x1, int16_t* y1, uint16_t* w, uint16_t* h) {
    uint16_t width = 0;
    uint16_t height = textsize * 8;
    const char* p = text;
    while (*p) {
        if (*p == '\n') {
            // newline not supported in bounds calculation for simplicity
        } else {
            width += textsize * 6;
        }
        p++;
    }
    *x1 = 0;
    *y1 = 0;
    *w = width;
    *h = height;
}

// Instantiate global display object
Display display;