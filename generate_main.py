import os

cpp_code = """
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_websocket_client.h"
#include "esp_http_client.h"
#include "esp_sntp.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_netif.h"
#include "mdns.h"

// Define LovyanGFX for ESP-IDF
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

static const char *TAG = "CloudClock";

// ============================================================================
//  App constants / host
// ============================================================================
const char* wsHost = "165.22.208.107";
const int   wsPort = 4002;
const char* wsPath = "/ws?token=******";
const int   deviceId            = 1;
const uint32_t ALARM_RING_DURATION = 30000;

// ============================================================================
//  TFT  (ILI9488  320x480)
// ============================================================================
class LGFX_ILI9488 : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Touch_XPT2046 _touch;
public:
  LGFX_ILI9488() {
    {
      auto cfg = _bus.config();
      cfg.spi_host   = SPI2_HOST;
      cfg.freq_write = 40000000;
      cfg.pin_sclk   = 14;
      cfg.pin_mosi   = 13;
      cfg.pin_miso   = 6;
      cfg.pin_dc     = 8;
      _bus.config(cfg);
    }
    _panel.setBus(&_bus);
    {
      auto cfg = _panel.config();
      cfg.pin_cs        = 18;
      cfg.pin_rst       = 9;
      cfg.memory_width  = 320;
      cfg.memory_height = 480;
      cfg.panel_width   = 320;
      cfg.panel_height  = 480;
      _panel.config(cfg);
    }
    {
      auto cfg = _touch.config();
      cfg.x_min      = 521;
      cfg.x_max      = 3620;
      cfg.y_min      = 451;
      cfg.y_max      = 3666;
      cfg.pin_int    = 11;
      cfg.spi_host   = SPI3_HOST;
      cfg.pin_sclk   = 2;
      cfg.pin_mosi   = 15;
      cfg.pin_miso   = 12;
      cfg.pin_cs     = 10;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};
LGFX_ILI9488 tft;
lgfx::LGFX_Sprite alarmSprite(&tft);

#define SCREEN_W 320
#define SCREEN_H 480

bool getTouchPixel(int &sx, int &sy) {
  int32_t x, y;
  if (tft.getTouch(&x, &y)) {
    sx = x;
    sy = y;
    return true;
  }
  return false;
}

// ============================================================================
//  Colour palette
// ============================================================================
static inline uint16_t RGB(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}
#define C_BG       RGB(0x0A,0x0A,0x10)
#define C_SURFACE  RGB(0x16,0x16,0x22)
#define C_SURFACE2 RGB(0x1E,0x1E,0x2C)
#define C_BORDER   RGB(0x30,0x30,0x48)
#define C_WHITE    RGB(0xFF,0xFF,0xFF)
#define C_BLACK    RGB(0x00,0x00,0x00)
#define C_ACCENT   RGB(0xFF,0xEE,0x55)
#define C_MUTED    RGB(0x55,0x55,0x77)
#define C_DIM      RGB(0x30,0x30,0x44)
#define C_RED      RGB(0xFF,0x44,0x44)
#define C_GREEN    RGB(0x33,0xDD,0x77)
#define C_ORANGE   RGB(0xFF,0x99,0x22)

#define SB_H       56
#define DATE_Y     65
#define TC_Y       90
#define TC_H       128
#define SEC_BAR_Y  (TC_Y + TC_H - 8)
#define AL_LABEL_Y 272
#define AL_Y       284
#define AL_VIEWPORT_H  166
#define FOOTER_Y   (SCREEN_H - 30)
#define AL_CARD_H  68
#define AL_CARD_GAP 8

#define BTN_POWER_PIN   GPIO_NUM_16
#define BTN_DISMISS_PIN GPIO_NUM_17
#define BUZZER_PIN      GPIO_NUM_5

// ============================================================================
//  Alarms
// ============================================================================
#define MAX_ALARMS 20
struct Alarm {
  int  id, hour, minute;
  bool enabled, recurring;
  char recurType[16];
  bool recurDays[7];
};
Alarm alarms[MAX_ALARMS];
int   alarmCount      = 0;
volatile bool updatingAlarms = false;

// ============================================================================
//  Global State
// ============================================================================
enum DeviceMode { MODE_BOOT, MODE_PAIRING, MODE_NORMAL };
DeviceMode currentMode = MODE_BOOT;

char savedSSID[64] = "";
char savedPASS[64] = "";
bool inSetupMode = false;
bool isConnecting = false;
uint32_t connectStartTime = 0;
const uint32_t CONNECT_TIMEOUT = 15000;
int scanResult = 0;
bool scanDone = false;
bool scanRequested = false;

char pairingCode[8] = "";
uint32_t pairingCodeExpiry = 0;
bool pairingCodeRequested = false;
const char* pairingURL = "cloudclock.vercal.app/pair";

int hourFormat = 24;
char deviceTimezone[64] = "UTC";

struct TZEntry { const char* name; long offset; };
const TZEntry TZ_MAP[] = {
  {"UTC",0},{"Asia/Kolkata",19800},{"Asia/Dubai",14400},
  {"Asia/Singapore",28800},{"Asia/Tokyo",32400},
  {"Europe/London",0},{"Europe/Paris",3600},{"Europe/Berlin",3600},
  {"America/New_York",-18000},{"America/Chicago",-21600},
  {"America/Denver",-25200},{"America/Los_Angeles",-28800},
  {"America/Sao_Paulo",-10800},{"Australia/Sydney",36000},
};
const int TZ_MAP_SIZE = sizeof(TZ_MAP)/sizeof(TZ_MAP[0]);

long getOffsetForTimezone(const char* tz) {
  for (int i = 0; i < TZ_MAP_SIZE; i++)
    if (strcmp(TZ_MAP[i].name, tz) == 0) return TZ_MAP[i].offset;
  return 19800; // default
}

esp_websocket_client_handle_t ws_client = NULL;
httpd_handle_t web_server = NULL;

bool triggerCodeFetch    = false;
bool triggerSettingsSave = false;
bool triggerTZApply      = false;

struct ConnStatus { bool cloud, wifi; };
volatile struct { ConnStatus prev, cur; } DOut;

bool bootLogoDrawn  = false;
int  spinnerAngle   = 0;
char lastBootMsg[64]= "";

int  currentDisplayedMin       = -1;
int  currentDisplayedSec       = -1;
int  currentDisplayedAlarmHash = -999;
bool currentDisplayedBuzzer    = false;

bool pairingScreenDrawn = false;
char lastDrawnCode[8]   = "";

bool statusBarDirty = true;
bool needFullRedraw = false;

bool lastFooterActive    = false;
uint32_t lastFooterAnimTime = 0;
int  footerAnimAngle     = 0;

int  alarmScrollOffset  = 0;
int  alarmScrollVel     = 0;
int  touchStartY        = -1;
int  touchLastY         = -1;
bool touchScrolling     = false;
uint32_t touchStartMs = 0;

static bool          scrollDirty        = false;
static uint32_t lastScrollRedrawMs = 0;
static const uint32_t SCROLL_REDRAW_MS = 30;

#define DISMISS_CX       (SCREEN_W / 2)
#define DISMISS_CY       (AL_Y + 100)
#define DISMISS_R_OUTER  54
#define DISMISS_R_INNER  18
#define DISMISS_R_TRACK  36

float swipeDragProgress = 0.0f;
bool  swipeTouching     = false;
int   swipeTouchX       = 0, swipeTouchY = 0;
bool  swipeAnimateSnap  = false;
uint32_t swipeSnapStart = 0;
float swipeSnapFrom  = 0.0f;

uint32_t alarmAnimMs   = 0;
float         alarmPulse    = 0.0f;

bool          buzzerActive         = false;
uint32_t buzzerStart          = 0;
int           lastFiredAlarmId     = -1;
int           lastFiredAlarmMinute = -1;
uint32_t lastAlarmCheck       = 0;

wifi_ap_record_t ap_records[20];

// ============================================================================
//  Forward declarations
// ============================================================================
void buzzerOff();
void buzzerOn();
void drawAlarmCardIntoSprite(lgfx::LGFX_Sprite& spr, int cardSpriteY, Alarm& a, bool isNext);
void drawAlarmSection(int nextIdx);
void drawTimeCardFull(struct tm& t);
void drawSecondsBar(int sec);
int  getNextUpcomingAlarmIndex();
void checkAlarms();
void fetchPairingCode();
void applyTimezone();
void startAP();
void drawSwipeDismissBanner();
void handleSwipeTouch();
bool getLocalTm(struct tm* t);
void setCloudStatus(bool status);
void setWifiStatus(bool status);

// Helpers
uint32_t millis() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}
void delay(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}
"""

with open("generate_main.py", "w") as f:
    pass
