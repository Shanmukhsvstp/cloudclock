#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <math.h>
#include "display.h"
#include "icons.h"

static const char* TAG = "CloudClock";

// UI state
static int spinnerAngle = 0;
static bool bootLogoDrawn = false;
static char lastBootMsg[64] = "";

// Forward declarations
void drawLogo(int y);
void printCentered(const char* text, int y, const void* font, uint16_t color);
void showBoot(int angle, const char* statusMsg);
void uiTask(void* pvParameters);

extern "C" void app_main() {
    ESP_LOGI(TAG, "CloudClock starting...");
    
    // Initialize display
    display.init();
    display.setRotation(2); // Add rotation method
    
    // Create UI task on core 0
    xTaskCreatePinnedToCore(uiTask, "ui", 4096, nullptr, 1, nullptr, 0);
    
    // For now, just loop showing boot screen
    while (1) {
        showBoot(spinnerAngle, "Starting up...");
        spinnerAngle = (spinnerAngle + 16) % 360;
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

void drawLogo(int y) {
    // Using default font for now; later we'll integrate GFX fonts
    display.setFont(nullptr);
    display.setTextSize(2);
    display.setTextColor(ST77XX_ACCENT);
    display.setCursor((Display::WIDTH - 10*6)/2, y); // approximate
    display.print("Cloud");
    display.setTextColor(ST77XX_WHITE);
    display.print("Clock");
}

void printCentered(const char* text, int y, const void* font, uint16_t color) {
    display.setFont(font);
    display.setTextColor(color);
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
    display.setCursor((Display::WIDTH - w)/2, y);
    display.print(text);
}

void showBoot(int angle, const char* statusMsg) {
    if (!bootLogoDrawn) {
        display.fillScreen(ST77XX_BLACK);
        drawLogo(100);
        bootLogoDrawn = true;
        lastBootMsg[0] = '\0';
    }
    // Clear spinner area
    display.fillRect(104, 134, 32, 32, ST77XX_BLACK);
    // Draw spinner dots
    for (int i = 0; i < 90; i += 18) {
        float rad = (angle + i) * 0.0174533f;
        int x = 120 + (int)(cosf(rad) * 12);
        int y = 150 + (int)(sinf(rad) * 12);
        display.fillCircle(x, y, 2, ST77XX_ACCENT);
    }
    // Update status text if changed
    if (strcmp(lastBootMsg, statusMsg) != 0) {
        display.fillRect(0, 180, 240, 30, ST77XX_BLACK);
        display.setFont(nullptr);
        display.setTextSize(1);
        display.setTextColor(ST77XX_LIGHTGREY);
        int16_t x1, y1;
        uint16_t w, h;
        display.getTextBounds(statusMsg, 0, 0, &x1, &y1, &w, &h);
        display.setCursor((Display::WIDTH - w)/2, 190);
        display.print(statusMsg);
        strcpy(lastBootMsg, statusMsg);
    }
}

void uiTask(void* pvParameters) {
    // In future, this will handle different modes
    while (1) {
        // Nothing yet; we'll expand later
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}