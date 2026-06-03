#include <stdlib.h>
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
#include "lwip/dns.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "mdns.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

static const char *TAG = "CloudClock";

const char* wsHost = "165.22.208.107";
const int   wsPort = 4002;
const char* wsPath = "/ws?token=******.eyJkZXZpY2VfaWQiOjMsInN1YiI6ImRldmljZSJ9.FNq9AaDWARN_qkPeP6aEQcz0umbmf5WeRxNaEXMnums";
const int   deviceId            = 1;
const uint32_t ALARM_RING_DURATION = 30000;

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
    sx = x; sy = y;
    return true;
  }
  return false;
}

static inline uint16_t RGB(uint8_t r, uint8_t g, uint8_t b) { return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3); }
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

enum DeviceMode { MODE_BOOT, MODE_PAIRING, MODE_NORMAL };
DeviceMode currentMode = MODE_BOOT;

char savedSSID[64] = "";
char savedPASS[64] = "";
bool inSetupMode = false;
bool isConnecting = false;
uint32_t connectStartTime = 0;
const uint32_t CONNECT_TIMEOUT = 15000;
uint16_t scanResult = 0;
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
  for (int i = 0; i < TZ_MAP_SIZE; i++) if (strcmp(TZ_MAP[i].name, tz) == 0) return TZ_MAP[i].offset;
  return 19800;
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

uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000ULL); }
void delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void fillCard(int x, int y, int w, int h, int r, uint16_t bg, uint16_t border=0) {
  tft.fillRoundRect(x, y, w, h, r, bg);
  if (border) tft.drawRoundRect(x, y, w, h, r, border);
}
void fillCardS(lgfx::LGFX_Sprite& s, int x, int y, int w, int h, int r, uint16_t bg, uint16_t border=0) {
  s.fillRoundRect(x, y, w, h, r, bg);
  if (border) s.drawRoundRect(x, y, w, h, r, border);
}
void printCentered(const char* text, int y, const lgfx::IFont* font, uint16_t color, uint16_t bg=0) {
  tft.setFont(font ? font : &fonts::Font0);
  tft.setTextColor(color, bg ? bg : C_BG);
  int w = tft.textWidth(text);
  tft.setCursor((SCREEN_W - w) / 2, y);
  tft.print(text);
}
void drawSpinner(int cx, int cy, int radius, int angle, uint16_t bg) {
  tft.fillCircle(cx, cy, radius + 4, bg);
  for (int i = 0; i < 8; i++) {
    float a = (angle + i * 45) * 0.01745329f;
    float bright = (float)i / 8.0f;
    uint8_t r = (uint8_t)(0xFF * bright);
    uint8_t g = (uint8_t)(0xEE * bright);
    uint8_t b = (uint8_t)(0x55 * bright);
    uint16_t col = RGB(r, g, b);
    int px = cx + (int)(cosf(a) * radius);
    int py = cy + (int)(sinf(a) * radius);
    tft.fillCircle(px, py, 2 + (i >= 6 ? 1 : 0), col);
  }
}
void drawHRule(int y, int x0=0, int x1=SCREEN_W, uint16_t col=C_BORDER) { tft.drawFastHLine(x0, y, x1-x0, col); }

void drawIconWifi(int cx, int cy, bool connected) {
  uint16_t col = connected ? C_WHITE : C_MUTED;
  uint16_t dot = connected ? C_ACCENT : C_RED;
  tft.fillCircle(cx, cy+10, 3, dot);
  if (connected) {
    for (int a = 215; a <= 325; a += 2) {
      float r = 0.01745329f * a;
      tft.drawPixel(cx + (int)(cosf(r)*8),  cy + (int)(sinf(r)*8),  col);
      tft.drawPixel(cx + (int)(cosf(r)*9),  cy + (int)(sinf(r)*9),  col);
    }
    for (int a = 215; a <= 325; a += 2) {
      float r = 0.01745329f * a;
      tft.drawPixel(cx + (int)(cosf(r)*13), cy + (int)(sinf(r)*13), col);
      tft.drawPixel(cx + (int)(cosf(r)*14), cy + (int)(sinf(r)*14), col);
    }
    for (int a = 215; a <= 325; a += 2) {
      float r = 0.01745329f * a;
      tft.drawPixel(cx + (int)(cosf(r)*18), cy + (int)(sinf(r)*18), col);
      tft.drawPixel(cx + (int)(cosf(r)*19), cy + (int)(sinf(r)*19), col);
    }
  } else {
    for (int a = 215; a <= 325; a += 3) {
      float r = 0.01745329f * a;
      tft.drawPixel(cx + (int)(cosf(r)*8),  cy + (int)(sinf(r)*8),  C_DIM);
      tft.drawPixel(cx + (int)(cosf(r)*13), cy + (int)(sinf(r)*13), C_DIM);
    }
    tft.drawLine(cx-6, cy-6, cx+6, cy+6, C_RED);
    tft.drawLine(cx+6, cy-6, cx-6, cy+6, C_RED);
  }
}
void drawIconCloud(int cx, int cy, bool connected) {
  uint16_t bodyCol = connected ? RGB(0x33,0x88,0xFF) : C_DIM;
  tft.fillCircle(cx-5, cy+2,  8, bodyCol);
  tft.fillCircle(cx+5, cy+2,  8, bodyCol);
  tft.fillCircle(cx,   cy-3,  9, bodyCol);
  tft.fillRect(cx-13, cy+2, 26, 9, bodyCol);
  if (connected) {
    uint16_t tc = C_WHITE;
    tft.drawLine(cx-5, cy+3, cx-1, cy+7, tc);
    tft.drawLine(cx-5, cy+4, cx-1, cy+8, tc);
    tft.drawLine(cx-1, cy+7, cx+6, cy-1, tc);
    tft.drawLine(cx-1, cy+8, cx+6, cy,   tc);
  } else {
    tft.drawLine(cx-4, cy-2, cx+4, cy+6, C_RED);
    tft.drawLine(cx-4, cy-1, cx+4, cy+7, C_RED);
    tft.drawLine(cx+4, cy-2, cx-4, cy+6, C_RED);
    tft.drawLine(cx+4, cy-1, cx-4, cy+7, C_RED);
  }
}
void drawIconRepeatS(lgfx::LGFX_Sprite& s, int cx, int cy, uint16_t col) {
  for (int a = 30; a <= 320; a += 6) {
    float r = 0.01745329f * a;
    s.drawPixel(cx + (int)(cosf(r)*7), cy + (int)(sinf(r)*7), col);
    s.drawPixel(cx + (int)(cosf(r)*8), cy + (int)(sinf(r)*8), col);
  }
  float ra = 320 * 0.01745329f;
  int ax = cx + (int)(cosf(ra)*7), ay = cy + (int)(sinf(ra)*7);
  s.drawLine(ax, ay, ax+3, ay-4, col);
  s.drawLine(ax, ay, ax+4, ay+2, col);
}

void drawStatusBar() {
  tft.fillRect(0, 0, SCREEN_W, SB_H, C_SURFACE);
  drawHRule(SB_H-1, 0, SCREEN_W, C_BORDER);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextColor(C_ACCENT);
  tft.setCursor(14, 20);
  tft.print("Cloud");
  int cloudW = tft.textWidth("Cloud");
  tft.setFont(&fonts::FreeSans12pt7b);
  tft.setTextColor(C_WHITE);
  tft.setCursor(14 + cloudW + 1, 20);
  tft.print("Clock");
  drawIconCloud(SCREEN_W - 54, SB_H/2, DOut.cur.cloud);
  drawIconWifi (SCREEN_W - 20, SB_H/2, DOut.cur.wifi);
  DOut.prev.cloud = DOut.cur.cloud;
  DOut.prev.wifi  = DOut.cur.wifi;
  statusBarDirty  = false;
}

void showBoot(int angle, const char* msg) {
  if (!bootLogoDrawn) {
    tft.fillScreen(C_BG);
    tft.setFont(&fonts::FreeSansBold24pt7b);
    int wCloud = tft.textWidth("Cloud");
    int wClock = tft.textWidth("Clock");
    int sx = (SCREEN_W - wCloud - wClock) / 2;
    tft.setTextColor(C_ACCENT); tft.setCursor(sx, 210); tft.print("Cloud");
    tft.setTextColor(C_WHITE);  tft.setCursor(sx + wCloud, 210); tft.print("Clock");
    tft.setFont(&fonts::Font0);
    tft.setTextColor(C_DIM);
    const char* tag = "Smart Alarm  •  Always in Sync";
    int tw = tft.textWidth(tag);
    tft.setCursor((SCREEN_W - tw)/2, 228);
    tft.print(tag);
    drawHRule(248, 30, SCREEN_W-30, C_BORDER);
    bootLogoDrawn = true;
    strcpy(lastBootMsg, "");
  }
  drawSpinner(SCREEN_W/2, 278, 16, angle, C_BG);
  if (strcmp(lastBootMsg, msg) != 0) {
    tft.fillRect(0, 302, SCREEN_W, 16, C_BG);
    tft.setFont(&fonts::Font0);
    tft.setTextColor(C_MUTED, C_BG);
    int tw = tft.textWidth(msg);
    tft.setCursor((SCREEN_W - tw)/2, 304);
    tft.print(msg);
    strcpy(lastBootMsg, msg);
  }
}

static const char* DAY_NAMES[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
static const char* MON_NAMES[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

void formatTimeStr(int hour, int minute, char* buf, int bufSize) {
  if (hourFormat == 12) {
    int h = hour % 12; if (h==0) h=12;
    snprintf(buf, bufSize, "%d:%02d %s", h, minute, hour>=12?"PM":"AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour, minute);
  }
}

void drawDateRow(struct tm& t) {
  tft.fillRect(0, SB_H, SCREEN_W, TC_Y - SB_H, C_BG);
  char buf[32];
  snprintf(buf, sizeof(buf), "%s, %d %s %d", DAY_NAMES[t.tm_wday], t.tm_mday, MON_NAMES[t.tm_mon], t.tm_year+1900);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(C_MUTED, C_BG);
  int w = tft.textWidth(buf);
  tft.setCursor((SCREEN_W - w)/2, DATE_Y);
  tft.print(buf);
}

void drawTimeCardFull(struct tm& t) {
  fillCard(14, TC_Y, SCREEN_W-28, TC_H, 16, C_SURFACE, C_BORDER);
  char hms[10];
  if (hourFormat == 12) {
    int h = t.tm_hour%12; if(h==0) h=12;
    snprintf(hms, sizeof(hms), "%d:%02d", h, t.tm_min);
  } else {
    snprintf(hms, sizeof(hms), "%02d:%02d", t.tm_hour, t.tm_min);
  }
  tft.setFont(&fonts::FreeSansBold24pt7b);
  int tw = tft.textWidth(hms);
  int textY = TC_Y + (TC_H - 34) / 2;
  tft.setTextColor(C_WHITE, C_SURFACE);
  tft.setCursor((SCREEN_W - tw)/2, textY);
  tft.print(hms);
  if (hourFormat == 12) {
    const char* ap = t.tm_hour>=12 ? "PM" : "AM";
    tft.setFont(&fonts::FreeSans9pt7b);
    tft.setTextColor(C_ACCENT, C_SURFACE);
    tft.setCursor(SCREEN_W - 14 - 14 - (int)tft.textWidth(ap), TC_Y + TC_H - 28);
    tft.print(ap);
  }
}

void drawSecondsBar(int sec) {
  int barX   = 14 + 8;
  int barW   = SCREEN_W - 28 - 16;
  int filled = (int)((float)barW * sec / 59.0f);
  tft.fillRect(barX, SEC_BAR_Y, barW, 5, C_DIM);
  if (filled > 0) tft.fillRect(barX, SEC_BAR_Y, filled, 5, C_ACCENT);
}

int computeAlarmHash() {
  if (updatingAlarms) return -1;
  int h = alarmCount * 31337;
  for (int i = 0; i < alarmCount; i++) {
    h ^= alarms[i].id + alarms[i].hour*100 + alarms[i].minute + (alarms[i].enabled?1:0);
  }
  return h;
}

void clampAlarmScroll() {
  int totalH = alarmCount * (AL_CARD_H + AL_CARD_GAP) - AL_CARD_GAP;
  int maxOff = totalH > AL_VIEWPORT_H ? totalH - AL_VIEWPORT_H : 0;
  if (alarmScrollOffset < 0)      alarmScrollOffset = 0;
  if (alarmScrollOffset > maxOff) alarmScrollOffset = maxOff;
}

void drawAlarmCardIntoSprite(lgfx::LGFX_Sprite& spr, int spriteY, Alarm& a, bool isNext) {
  if (spriteY + AL_CARD_H <= 0) return;
  if (spriteY >= (int)spr.height()) return;
  uint16_t bgCol  = isNext  ? C_SURFACE2 : C_SURFACE;
  uint16_t bdCol  = isNext  ? C_ACCENT   : C_BORDER;
  uint16_t txtCol = a.enabled ? C_WHITE  : C_MUTED;
  fillCardS(spr, 14, spriteY, SCREEN_W-28, AL_CARD_H, 12, bgCol, bdCol);
  if (isNext) spr.fillRoundRect(14, spriteY, 5, AL_CARD_H, 2, C_ACCENT);
  char tbuf[12]; formatTimeStr(a.hour, a.minute, tbuf, sizeof(tbuf));
  spr.setFont(&fonts::FreeSansBold12pt7b);
  spr.setTextColor(txtCol, bgCol);
  spr.setCursor(28, spriteY + 28);
  spr.print(tbuf);
  if (isNext) {
    spr.setFont(&fonts::Font0);
    spr.fillRoundRect(SCREEN_W-60, spriteY+8, 34, 14, 4, C_ACCENT);
    spr.setTextColor(C_BG, C_ACCENT);
    int nw = spr.textWidth("NEXT");
    spr.setCursor(SCREEN_W-60 + (34-nw)/2, spriteY+13);
    spr.print("NEXT");
  }
  if (a.recurring) {
    spr.setFont(&fonts::Font0); spr.setTextColor(C_MUTED, bgCol);
    if (strcmp(a.recurType,"daily") == 0) { spr.setCursor(28, spriteY + 48); spr.print("Every day"); }
    else if (strcmp(a.recurType,"weekly") == 0) {
      const char* dn[] = {"S","M","T","W","T","F","S"};
      int pw = 18, gap = 3;
      for (int d = 0; d < 7; d++) {
        int px = 28 + d*(pw+gap);
        uint16_t pbg = a.recurDays[d] ? C_ACCENT : C_DIM;
        uint16_t pfc = a.recurDays[d] ? C_BLACK  : C_MUTED;
        spr.fillRoundRect(px, spriteY+46, pw, 14, 3, pbg);
        spr.setTextColor(pfc, pbg);
        int dw2 = spr.textWidth(dn[d]);
        spr.setCursor(px+(pw-dw2)/2, spriteY+50);
        spr.print(dn[d]);
      }
    }
    drawIconRepeatS(spr, SCREEN_W-30, spriteY+20, C_MUTED);
  } else {
    spr.setFont(&fonts::Font0); spr.setTextColor(C_DIM, bgCol);
    spr.setCursor(28, spriteY+48); spr.print("One-time");
  }
  if (!a.enabled) {
    spr.setFont(&fonts::Font0); spr.setTextColor(C_RED, bgCol);
    spr.setCursor(SCREEN_W-70, spriteY+48); spr.print("OFF");
  }
}

void drawAlarmSection(int nextIdx) {
  tft.fillRect(0, AL_LABEL_Y - 4, SCREEN_W, AL_Y - (AL_LABEL_Y - 4), C_BG);
  if (alarmCount == 0) {
    tft.fillRect(0, AL_Y, SCREEN_W, AL_VIEWPORT_H, C_BG);
    fillCard(14, AL_Y + 20, SCREEN_W-28, 60, 12, C_SURFACE, C_BORDER);
    printCentered("No alarms set", AL_Y + 56, &fonts::FreeSans9pt7b, C_MUTED);
    return;
  }
  tft.setFont(&fonts::Font0); tft.setTextColor(C_MUTED, C_BG);
  char hdrBuf[16]; if (alarmCount > 1) snprintf(hdrBuf, sizeof(hdrBuf), "%d ALARMS", alarmCount); else strcpy(hdrBuf, "1 ALARM");
  int hw = tft.textWidth(hdrBuf); tft.setCursor((SCREEN_W-hw)/2, AL_LABEL_Y); tft.print(hdrBuf);

  int totalH = alarmCount * (AL_CARD_H + AL_CARD_GAP) - AL_CARD_GAP;
  if (totalH > AL_VIEWPORT_H) {
    tft.setFont(&fonts::Font0); tft.setTextColor(C_DIM, C_BG);
    if (alarmScrollOffset > 0) printCentered("^", AL_Y - 10, &fonts::Font0, C_DIM);
    if (alarmScrollOffset < totalH - AL_VIEWPORT_H) printCentered("v", AL_Y + AL_VIEWPORT_H + 2, &fonts::Font0, C_DIM);
  }

  alarmSprite.fillScreen(C_BG);
  for (int i = 0; i < alarmCount; i++) {
    int spriteY = i * (AL_CARD_H + AL_CARD_GAP) - alarmScrollOffset;
    drawAlarmCardIntoSprite(alarmSprite, spriteY, alarms[i], i == nextIdx);
  }
  alarmSprite.pushSprite(0, AL_Y);
}

void drawThickArc(int cx, int cy, int r, int thickness, float startDeg, float endDeg, uint16_t col) {
  if (endDeg <= startDeg) return;
  float step = 1.5f;
  for (float a = startDeg; a <= endDeg; a += step) {
    float rad = a * 0.01745329f;
    float cs = cosf(rad), sn = sinf(rad);
    for (int t = 0; t < thickness; t++) {
      int rr = r - thickness/2 + t;
      tft.drawPixel(cx + (int)(cs * rr), cy + (int)(sn * rr), col);
    }
  }
}

void drawSwipeDismissBanner() {
  tft.fillRect(0, AL_LABEL_Y - 4, SCREEN_W, SCREEN_H - (AL_LABEL_Y - 4) - 30, C_BG);
  uint16_t bannerBg = RGB(0x12, 0x08, 0x00);
  fillCard(14, AL_Y - 10, SCREEN_W-28, 190, 18, bannerBg, C_ORANGE);
  tft.setFont(&fonts::FreeSansBold12pt7b); tft.setTextColor(C_ACCENT, bannerBg);
  int tw = tft.textWidth("ALARM RINGING"); tft.setCursor((SCREEN_W - tw)/2, AL_Y + 4); tft.print("ALARM RINGING");

  int cx = DISMISS_CX, cy = DISMISS_CY;
  drawThickArc(cx, cy, DISMISS_R_OUTER, 5, 0, 360, C_DIM);
  if (swipeDragProgress > 0.005f) {
    float arcEnd = swipeDragProgress * 360.0f;
    uint8_t rr = (uint8_t)(0xFF - (uint8_t)(0xCC * swipeDragProgress));
    uint8_t gg = (uint8_t)(0x55 + (uint8_t)(0x88 * swipeDragProgress));
    uint16_t arcCol = RGB(rr, gg, 0x22);
    drawThickArc(cx, cy, DISMISS_R_OUTER, 5, -90, -90 + arcEnd, arcCol);
  }

  if (!swipeTouching) {
    float pulse = (sinf(alarmPulse) + 1.0f) * 0.5f;
    uint8_t alpha = (uint8_t)(40 + 40 * pulse);
    drawThickArc(cx, cy, DISMISS_R_OUTER + 6, 3, 0, 360, RGB(alpha, (uint8_t)(alpha * 0.6), 0));
  }

  float handleDist = swipeDragProgress * (DISMISS_R_TRACK - 2);
  float hAngle = -1.5708f;
  if (swipeTouching) {
    float dx = swipeTouchX - cx; float dy = swipeTouchY - cy;
    if (fabsf(dx) + fabsf(dy) > 4) hAngle = atan2f(dy, dx);
  }
  int hx = cx + (int)(cosf(hAngle) * handleDist), hy = cy + (int)(sinf(hAngle) * handleDist);
  tft.fillCircle(hx, hy, DISMISS_R_INNER + 4, RGB(0x30,0x18,0x00));
  uint8_t hr2 = (uint8_t)(0xFF - (uint8_t)(0xCC * swipeDragProgress));
  uint8_t hg2 = (uint8_t)(0x88 + (uint8_t)(0x77 * swipeDragProgress));
  tft.fillCircle(hx, hy, DISMISS_R_INNER, RGB(hr2, hg2, 0x22));

  {
    float perpA = hAngle + 1.5708f;
    int ax1 = hx + (int)(cosf(hAngle)*4 + cosf(perpA)*5), ay1 = hy + (int)(sinf(hAngle)*4 + sinf(perpA)*5);
    int ax2 = hx + (int)(cosf(hAngle)*9), ay2 = hy + (int)(sinf(hAngle)*9);
    int ax3 = hx + (int)(cosf(hAngle)*4 - cosf(perpA)*5), ay3 = hy + (int)(sinf(hAngle)*4 - sinf(perpA)*5);
    tft.drawLine(ax1, ay1, ax2, ay2, C_WHITE); tft.drawLine(ax3, ay3, ax2, ay2, C_WHITE);
  }

  tft.setFont(&fonts::Font0); tft.setTextColor(C_MUTED, bannerBg);
  const char* sub = (swipeDragProgress > 0.5f) ? "Release to dismiss!" : "Slide to dismiss";
  int sw2 = tft.textWidth(sub); tft.setCursor((SCREEN_W - sw2)/2, AL_Y + 168); tft.print(sub);
}

void handleSwipeTouch() {
  int tx, ty; bool touched = getTouchPixel(tx, ty);
  alarmPulse += 0.06f; if (alarmPulse > 6.2832f) alarmPulse -= 6.2832f;
  if (touched) {
    swipeTouchX = tx; swipeTouchY = ty;
    if (ty >= AL_Y - 20 && ty <= AL_Y + 200) {
      swipeTouching = true; swipeAnimateSnap = false;
      float dx = tx - DISMISS_CX, dy = ty - DISMISS_CY;
      float dist = sqrtf(dx*dx + dy*dy);
      swipeDragProgress = dist / (DISMISS_R_OUTER - DISMISS_R_INNER);
      if (swipeDragProgress > 1.0f) swipeDragProgress = 1.0f;
      if (swipeDragProgress >= 0.98f) { buzzerOff(); swipeDragProgress = 0.0f; swipeTouching = false; }
    }
  } else {
    if (swipeTouching) {
      swipeTouching = false;
      if (swipeDragProgress > 0.0f) { swipeAnimateSnap = true; swipeSnapFrom = swipeDragProgress; swipeSnapStart = millis(); }
    }
    if (swipeAnimateSnap) {
      float elapsed = (float)(millis() - swipeSnapStart);
      float t = elapsed / 300.0f;
      if (t >= 1.0f) { swipeDragProgress = 0.0f; swipeAnimateSnap = false; }
      else { float ease = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); swipeDragProgress = swipeSnapFrom * (1.0f - ease); }
    }
  }
}

void handleAlarmTouch() {
  int tx, ty; bool touched = getTouchPixel(tx, ty);
  if (touched) {
    if (!touchScrolling) {
      touchStartY = ty; touchLastY = ty; touchStartMs = millis();
      touchScrolling = true; alarmScrollVel = 0;
    } else {
      int dy = touchLastY - ty;
      if (dy != 0) { alarmScrollOffset += dy; clampAlarmScroll(); alarmScrollVel = dy; touchLastY = ty; scrollDirty = true; }
    }
  } else {
    if (touchScrolling) touchScrolling = false;
    if (alarmScrollVel != 0) {
      alarmScrollOffset += alarmScrollVel; clampAlarmScroll();
      alarmScrollVel = (int)(alarmScrollVel * 0.72f);
      if (abs(alarmScrollVel) <= 1) alarmScrollVel = 0;
      scrollDirty = true;
    }
  }
}

void updateFooter() {
  bool need = !DOut.cur.cloud || !DOut.cur.wifi;
  if (need) {
    if (millis() - lastFooterAnimTime > 30) {
      lastFooterAnimTime = millis(); footerAnimAngle = (footerAnimAngle + 20) % 360;
      if (!lastFooterActive) { tft.fillRect(0, FOOTER_Y, SCREEN_W, 30, C_SURFACE); drawHRule(FOOTER_Y, 0, SCREEN_W, C_BORDER); }
      drawSpinner(20, FOOTER_Y+15, 7, footerAnimAngle, C_SURFACE);
      tft.setFont(&fonts::Font0); tft.setTextColor(C_MUTED, C_SURFACE); tft.setCursor(38, FOOTER_Y+10);
      if (!DOut.cur.wifi) tft.print("Connecting to WiFi...       "); else tft.print("Connecting to Cloud...      ");
      lastFooterActive = true;
    }
  } else if (lastFooterActive) { tft.fillRect(0, FOOTER_Y, SCREEN_W, 30, C_BG); lastFooterActive = false; }
}

void updateNormalUI() {
  if (needFullRedraw) {
    tft.fillScreen(C_BG); needFullRedraw = false; statusBarDirty = true;
    currentDisplayedMin = -1; currentDisplayedSec = -1; currentDisplayedAlarmHash = -999;
    currentDisplayedBuzzer = !buzzerActive; swipeDragProgress = 0.0f; swipeTouching = false; swipeAnimateSnap = false;
  }
  if (statusBarDirty || DOut.cur.wifi != DOut.prev.wifi || DOut.cur.cloud != DOut.prev.cloud) drawStatusBar();
  struct tm t; bool timeOK = getLocalTm(&t);
  static int lastDay = -1;
  if (timeOK) {
    if (t.tm_mday != lastDay || currentDisplayedMin == -1) { drawDateRow(t); lastDay = t.tm_mday; }
    bool minChg = (t.tm_min != currentDisplayedMin) || (currentDisplayedMin == -1);
    bool secChg = (t.tm_sec != currentDisplayedSec);
    if (minChg) {
      drawTimeCardFull(t); drawSecondsBar(t.tm_sec);
      currentDisplayedMin = t.tm_min; currentDisplayedSec = t.tm_sec; currentDisplayedAlarmHash = -999;
    } else if (secChg) { drawSecondsBar(t.tm_sec); currentDisplayedSec = t.tm_sec; }
  }
  if (buzzerActive) {
    handleSwipeTouch(); drawSwipeDismissBanner(); currentDisplayedBuzzer = true;
  } else {
    if (currentDisplayedBuzzer) { currentDisplayedAlarmHash = -999; currentDisplayedBuzzer = false; }
    handleAlarmTouch();
    int nextIdx = getNextUpcomingAlarmIndex();
    int hashNow = computeAlarmHash() ^ (nextIdx + 100);
    bool contentChanged = (hashNow != currentDisplayedAlarmHash);
    bool scrollReady = scrollDirty && (millis() - lastScrollRedrawMs >= SCROLL_REDRAW_MS);
    if (contentChanged || scrollReady) {
      drawAlarmSection(nextIdx); currentDisplayedAlarmHash = hashNow;
      if (scrollDirty) { scrollDirty = false; lastScrollRedrawMs = millis(); }
    }
  }
}

void drawPairingScreen(const char* code) {
  if (strcmp(lastDrawnCode, code) == 0 && pairingScreenDrawn) return;
  tft.fillRect(0, SB_H, SCREEN_W, SCREEN_H - SB_H - 30, C_BG);
  printCentered("PAIR YOUR CLOCK", SB_H + 18, &fonts::Font0, C_MUTED);
  fillCard(18, SB_H+34, SCREEN_W-36, 106, 16, C_SURFACE, C_BORDER);
  tft.setFont(&fonts::FreeSansBold24pt7b); tft.setTextColor(C_ACCENT);
  int cw = tft.textWidth(code), fh = tft.fontHeight();
  tft.setCursor((SCREEN_W-cw)/2, SB_H + 34 + (106 + fh)/2 - 6); tft.print(code);
  tft.setFont(&fonts::Font0); printCentered("Code expires in 10 minutes", SB_H+154, &fonts::Font0, C_MUTED);
  drawHRule(SB_H+168, 20, SCREEN_W-20, C_BORDER);
  fillCard(18, SB_H+180, SCREEN_W-36, 160, 12, C_SURFACE);
  int tx = 36, ty = SB_H + 202;
  tft.setFont(&fonts::Font0); tft.setTextColor(C_ACCENT, C_SURFACE); tft.setCursor(tx, ty); tft.print("1  ");
  tft.setTextColor(C_WHITE, C_SURFACE); tft.print("Open a browser and go to:");
  tft.setTextColor(C_ACCENT, C_SURFACE); int uw = tft.textWidth(pairingURL); tft.setCursor((SCREEN_W-uw)/2, ty+18); tft.print(pairingURL);
  tft.setTextColor(C_ACCENT, C_SURFACE); tft.setCursor(tx, ty+38); tft.print("2  ");
  tft.setTextColor(C_WHITE, C_SURFACE); tft.print("Enter the 6-digit code above");
  tft.setTextColor(C_MUTED, C_SURFACE); tft.setCursor(tx, ty+58); tft.print("Your clock activates once paired.");
  drawHRule(SB_H+356, 20, SCREEN_W-20, C_DIM);
  tft.setTextColor(C_DIM, C_BG); printCentered("Hold POWER 3s = sleep  |  10s = reset", SB_H+368, &fonts::Font0, C_DIM);
  DOut.prev.wifi = !DOut.cur.wifi; DOut.prev.cloud = !DOut.cur.cloud; statusBarDirty = true;
  pairingScreenDrawn = true; strncpy(lastDrawnCode, code, 7);
}

void updatePairingUI() {
  if (needFullRedraw) { tft.fillScreen(C_BG); needFullRedraw = false; statusBarDirty = true; }
  if (statusBarDirty || DOut.cur.wifi != DOut.prev.wifi || DOut.cur.cloud != DOut.prev.cloud) drawStatusBar();
  if (pairingCode[0] != 0) drawPairingScreen(pairingCode);
  else if (!pairingScreenDrawn) {
    tft.fillRect(0, SB_H, SCREEN_W, SCREEN_H-SB_H-30, C_BG);
    printCentered("Requesting pairing code...", SCREEN_H/2, &fonts::Font0, C_MUTED);
    pairingScreenDrawn = true;
  }
}

void uiTaskCode(void* parameter) {
  for (;;) {
    if      (currentMode == MODE_NORMAL)  updateNormalUI();
    else if (currentMode == MODE_PAIRING) updatePairingUI();
    updateFooter();
    delay(20);
  }
}


uint32_t btnPowerPressedAt = 0;
bool btnPowerHeld = false;
void handleButtons() {
  static bool lastDismiss = false;
  bool curDismiss = gpio_get_level(BTN_DISMISS_PIN);
  if (curDismiss && !lastDismiss && buzzerActive) buzzerOff();
  lastDismiss = curDismiss;

  static bool lastPower = false;
  bool curPower = gpio_get_level(BTN_POWER_PIN);
  if (curPower && !lastPower) { btnPowerPressedAt = millis(); btnPowerHeld = true; }
  if (!curPower && lastPower && btnPowerHeld) {
    uint32_t held = millis() - btnPowerPressedAt;
    if (held >= 10000) {
      tft.fillScreen(C_BG); printCentered("Resetting...", SCREEN_H/2, &fonts::FreeSans12pt7b, C_ACCENT); delay(500);
      nvs_handle_t my_handle;
      if(nvs_open("wifi", NVS_READWRITE, &my_handle) == ESP_OK) { nvs_erase_all(my_handle); nvs_commit(my_handle); nvs_close(my_handle); }
      if(nvs_open("settings", NVS_READWRITE, &my_handle) == ESP_OK) { nvs_set_u8(my_handle, "paired", 0); nvs_commit(my_handle); nvs_close(my_handle); }
      delay(500); esp_restart();
    } else if (held >= 3000) {
      if (buzzerActive) buzzerOff();
      tft.fillScreen(C_BG); printCentered("Sleeping...", SCREEN_H/2, &fonts::FreeSans12pt7b, C_MUTED); delay(600);
      tft.fillScreen(C_BG);
      esp_sleep_enable_ext0_wakeup(BTN_POWER_PIN, 1);
      esp_deep_sleep_start();
    }
    btnPowerHeld = false;
  }
  lastPower = curPower;
}

static esp_err_t captive_portal_handler(httpd_req_t *req) {
  if (!scanDone) {
    const char* html = "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='2'><title>Scanning</title></head>"
                       "<body style='background:#0f0f13;color:#e8e8f0;font-family:sans-serif;text-align:center;padding:20vh'>"
                       "<h3>Scanning networks...</h3></body></html>";
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  char html[2048];
  snprintf(html, sizeof(html),
    "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{background:#0f0f13;color:#e8e8f0;font-family:sans-serif;padding:24px;} "
    ".card{background:#1a1a22;border-radius:24px;padding:40px 36px;width:100%%;max-width:420px;margin:auto;} "
    "select,input{width:100%%;background:#23232f;border:1px solid #2e2e3d;border-radius:14px;color:#e8e8f0;padding:13px;margin-bottom:16px;outline:none;} "
    ".btn{width:100%%;border-radius:14px;font-size:15px;padding:14px;cursor:pointer;border:none;} "
    ".btn-p{background:#FFEE8C;color:#111;} "
    ".btn-s{background:transparent;color:#7a7a96;border:1px solid #2e2e3d;margin-top:24px;}</style></head>"
    "<body><div class='card'><h2>Connect your device to WiFi</h2><form action='/save' method='get'>"
    "<label>Network</label><select name='ssid'>");
  httpd_resp_send_chunk(req, html, strlen(html));
  
  for (int i = 0; i < scanResult; i++) {
    char opt[128]; snprintf(opt, sizeof(opt), "<option value='%s'>%s</option>", ap_records[i].ssid, ap_records[i].ssid);
    httpd_resp_send_chunk(req, opt, strlen(opt));
  }
  
  const char* form_end = "</select><label>Password</label><input type='password' name='pass' placeholder='Enter WiFi password'>"
                         "<button type='submit' class='btn btn-p'>Connect</button></form>"
                         "<button class='btn btn-s' onclick=\"location.href='/rescan'\">↻ Rescan</button></div></body></html>";
  httpd_resp_send_chunk(req, form_end, strlen(form_end));
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t rescan_handler(httpd_req_t *req) {
  scanDone = false; scanRequested = false;
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t save_handler(httpd_req_t *req) {
  char buf[256];
  if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
    char ssid[64] = ""; char pass[64] = "";
    if (httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid)) == ESP_OK &&
        httpd_query_key_value(buf, "pass", pass, sizeof(pass)) == ESP_OK) {
        
        // URL decode components if necessary (omitted for brevity, assume simple chars or use built-in)
        nvs_handle_t my_handle;
        if(nvs_open("wifi", NVS_READWRITE, &my_handle) == ESP_OK) {
          nvs_set_str(my_handle, "ssid", ssid);
          nvs_set_str(my_handle, "pass", pass);
          nvs_commit(my_handle); nvs_close(my_handle);
        }
        strncpy(savedSSID, ssid, sizeof(savedSSID));
        strncpy(savedPASS, pass, sizeof(savedPASS));
        
        const char* resp = "<html><body style='background:#0f0f13;color:#e8e8f0;font-family:sans-serif;text-align:center;padding:20vh'><h3>Connecting...</h3></body></html>";
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        
        isConnecting = true; connectStartTime = millis();
        return ESP_OK;
    }
  }
  httpd_resp_send_404(req);
  return ESP_FAIL;
}

static esp_err_t generate_204_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static void dns_server_task(void *pvParameters) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) { ESP_LOGE(TAG, "Unable to create socket"); vTaskDelete(NULL); }
  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(53);
  bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
  
  uint8_t rx_buffer[128];
  while (inSetupMode) {
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);
    struct timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &socklen);
    if (len > 0) {
      if (rx_buffer[2] & 0x80) continue; // It's a response
      uint8_t tx_buffer[128];
      memcpy(tx_buffer, rx_buffer, len);
      tx_buffer[2] |= 0x80; // set response flag
      // Simply point A record to our AP IP (192.168.4.1)
      uint8_t a_record[] = {
        0xc0, 0x0c, // pointer to name
        0x00, 0x01, // type A
        0x00, 0x01, // class IN
        0x00, 0x00, 0x00, 0x3c, // TTL
        0x00, 0x04, // length
        192, 168, 4, 1
      };
      // We need to set answer count to 1 (byte 7)
      tx_buffer[7] = 1;
      if (len + sizeof(a_record) <= sizeof(tx_buffer)) {
        memcpy(tx_buffer + len, a_record, sizeof(a_record));
        sendto(sock, tx_buffer, len + sizeof(a_record), 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
      }
    }
  }
  close(sock);
  vTaskDelete(NULL);
}

void setWifiStatus(bool v)  { DOut.cur.wifi  = v; }
void setCloudStatus(bool v) { DOut.cur.cloud = v; }

void sendBuzzerStatus(bool state) {
  if (!ws_client || !esp_websocket_client_is_connected(ws_client)) return;
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "type", "buzzer_status");
  cJSON_AddBoolToObject(root, "value", state);
  char *json = cJSON_PrintUnformatted(root);
  esp_websocket_client_send_text(ws_client, json, strlen(json), portMAX_DELAY);
  free(json); cJSON_Delete(root);
}
void buzzerOn()  { gpio_set_level(BUZZER_PIN, 1); buzzerActive=true; buzzerStart=millis(); swipeDragProgress=0.0f; swipeTouching=false; swipeAnimateSnap=false; sendBuzzerStatus(true); }
void buzzerOff() { gpio_set_level(BUZZER_PIN, 0); buzzerActive=false; swipeDragProgress=0.0f; swipeTouching=false; swipeAnimateSnap=false; sendBuzzerStatus(false); }

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      setCloudStatus(true); break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      setCloudStatus(false); break;
    case WEBSOCKET_EVENT_DATA: {
      if (data->op_code == 1 && data->data_len > 0) {
        char *payload = (char*)malloc(data->data_len + 1);
        memcpy(payload, data->data_ptr, data->data_len);
        payload[data->data_len] = 0;
        
        cJSON *root = cJSON_Parse(payload);
        if (root) {
          cJSON *type = cJSON_GetObjectItem(root, "type");
          if (type && type->valuestring) {
            const char* msgType = type->valuestring;
            if (strcmp(msgType, "buzzer") == 0) {
              cJSON *val = cJSON_GetObjectItem(root, "value");
              if (val && cJSON_IsTrue(val)) buzzerOn(); else buzzerOff();
            }
            else if (strcmp(msgType, "alarm_fire") == 0) { buzzerOn(); }
            else if (strcmp(msgType, "sync_alarms") == 0) {
              cJSON *alarms_arr = cJSON_GetObjectItem(root, "alarms");
              if (cJSON_IsArray(alarms_arr)) {
                updatingAlarms = true; alarmCount = 0;
                int n = cJSON_GetArraySize(alarms_arr);
                for (int i=0; i<n && alarmCount < MAX_ALARMS; i++) {
                  cJSON *obj = cJSON_GetArrayItem(alarms_arr, i);
                  Alarm& a = alarms[alarmCount];
                  cJSON *id = cJSON_GetObjectItem(obj, "id"); a.id = id ? id->valueint : 0;
                  cJSON *en = cJSON_GetObjectItem(obj, "enabled"); a.enabled = en ? cJSON_IsTrue(en) : true;
                  cJSON *tm = cJSON_GetObjectItem(obj, "time");
                  if (tm && tm->valuestring) sscanf(tm->valuestring, "%d:%d", &a.hour, &a.minute);
                  cJSON *rec = cJSON_GetObjectItem(obj, "recurring"); a.recurring = rec ? cJSON_IsTrue(rec) : false;
                  cJSON *rt = cJSON_GetObjectItem(obj, "recur_type");
                  strncpy(a.recurType, rt && rt->valuestring ? rt->valuestring : "", sizeof(a.recurType)-1);
                  memset(a.recurDays, 0, sizeof(a.recurDays));
                  cJSON *rd = cJSON_GetObjectItem(obj, "recur_days");
                  if (cJSON_IsArray(rd)) {
                    int dn = cJSON_GetArraySize(rd);
                    for (int j=0; j<dn; j++) {
                      cJSON *di = cJSON_GetArrayItem(rd, j);
                      if (di && di->valueint >= 0 && di->valueint <= 6) a.recurDays[di->valueint] = true;
                    }
                  }
                  alarmCount++;
                }
                updatingAlarms = false; alarmScrollOffset=0; alarmScrollVel=0; currentDisplayedAlarmHash=-999;
              }
            }
            else if (strcmp(msgType, "sync_settings") == 0) {
              cJSON *hf = cJSON_GetObjectItem(root, "hour_format"); int newFmt = hf ? hf->valueint : 24;
              cJSON *tz = cJSON_GetObjectItem(root, "timezone"); const char* newTZ = tz && tz->valuestring ? tz->valuestring : "UTC";
              bool fmtChanged = (newFmt != hourFormat);
              bool tzChanged = (strcmp(newTZ, deviceTimezone) != 0);
              if (fmtChanged) { hourFormat = newFmt; currentDisplayedMin = -1; }
              if (tzChanged) {
                strncpy(deviceTimezone, newTZ, sizeof(deviceTimezone)-1);
                deviceTimezone[sizeof(deviceTimezone)-1] = '\0';
                triggerTZApply = true; currentDisplayedMin = -1;
              }
              if (fmtChanged || tzChanged) triggerSettingsSave = true;
            }
            else if (strcmp(msgType, "pairing_complete") == 0) {
              nvs_handle_t my_handle;
              if (nvs_open("settings", NVS_READWRITE, &my_handle) == ESP_OK) {
                nvs_set_u8(my_handle, "paired", 1); nvs_commit(my_handle); nvs_close(my_handle);
              }
              currentMode = MODE_NORMAL; pairingScreenDrawn = false; needFullRedraw = true;
            }
            else if (strcmp(msgType, "request_pairing_code") == 0) {
              nvs_handle_t my_handle; uint8_t paired = 0;
              if (nvs_open("settings", NVS_READONLY, &my_handle) == ESP_OK) { nvs_get_u8(my_handle, "paired", &paired); nvs_close(my_handle); }
              if (!paired) triggerCodeFetch = true;
            }
          }
          cJSON_Delete(root);
        }
        free(payload);
      }
      break;
    }
    default: break;
  }
}

void checkAlarms() {
  struct tm t; if (!getLocalTm(&t)) return;
  int minuteNow = t.tm_hour*60+t.tm_min;
  for (int i=0;i<alarmCount;i++) {
    Alarm& a=alarms[i];
    if (!a.enabled||a.hour!=t.tm_hour||a.minute!=t.tm_min) continue;
    if (a.id==lastFiredAlarmId&&minuteNow==lastFiredAlarmMinute) continue;
    bool fire=false;
    if (!a.recurring||strcmp(a.recurType,"daily")==0) fire=true;
    else if (strcmp(a.recurType,"weekly")==0) fire=a.recurDays[t.tm_wday];
    if (fire){lastFiredAlarmId=a.id;lastFiredAlarmMinute=minuteNow;buzzerOn();break;}
  }
}

int getNextUpcomingAlarmIndex() {
  if (updatingAlarms) return -1;
  struct tm t; if (!getLocalTm(&t)) return -1;
  int nowMins=t.tm_hour*60+t.tm_min, minDiff=999999, nextIdx=-1;
  for (int i=0;i<alarmCount;i++){
    Alarm& a=alarms[i]; if (!a.enabled) continue;
    int alMins=a.hour*60+a.minute;
    if (!a.recurring||strcmp(a.recurType,"daily")==0){
      int diff=alMins-nowMins; if(diff<=0) diff+=1440;
      if(diff<minDiff){minDiff=diff;nextIdx=i;}
    } else if (strcmp(a.recurType,"weekly")==0){
      for(int off=0;off<7;off++){
        int checkDay=(t.tm_wday+off)%7;
        if(a.recurDays[checkDay]){
          int diff=alMins-nowMins+(off*1440);
          if(diff>0&&diff<minDiff){minDiff=diff;nextIdx=i;break;}
          else if(off==0&&diff<=0){int nd=diff+10080;if(nd<minDiff){minDiff=nd;nextIdx=i;}}
        }
      }
    }
  }
  return nextIdx;
}

bool getLocalTm(struct tm* t) {
  time_t now = time(nullptr);
  if (now < 1000000000UL) return false;
  now += getOffsetForTimezone(deviceTimezone);
  gmtime_r(&now, t);
  return true;
}

void syncNTP() {
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_setservername(1, "time.nist.gov");
  esp_sntp_init();
  struct tm t;
  uint32_t sw = millis();
  while (millis() - sw < 20000) {
    showBoot(spinnerAngle, "Syncing Time...");
    spinnerAngle = (spinnerAngle + 16) % 360;
    if (getLocalTm(&t)) return;
    delay(25);
  }
}
void applyTimezone() {}

void fetchPairingCode() {
  esp_http_client_config_t config = {};
  config.url = "http://165.22.208.107:4002/api/pair/code";
  esp_http_client_handle_t client = esp_http_client_init(&config);
  char auth[128];
  snprintf(auth, sizeof(auth), "Bearer %s", strstr(wsPath, "token=") + 6);
  esp_http_client_set_header(client, "Authorization", auth);
  esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK && esp_http_client_get_status_code(client) == 200) {
    int len = esp_http_client_get_content_length(client);
    if (len > 0) {
      char *buf = (char*)malloc(len + 1);
      esp_http_client_read(client, buf, len);
      buf[len] = 0;
      cJSON *root = cJSON_Parse(buf);
      if (root) {
        cJSON *code = cJSON_GetObjectItem(root, "code");
        if (code && code->valuestring && strlen(code->valuestring) == 6) {
          strncpy(pairingCode, code->valuestring, 7);
          pairingCodeExpiry = millis() + 9*60*1000;
          pairingCodeRequested = true;
          currentMode = MODE_PAIRING; pairingScreenDrawn = false;
        }
        cJSON_Delete(root);
      }
      free(buf);
    }
  }
  esp_http_client_cleanup(client);
}

void startAP() {
  inSetupMode = true; scanDone = false; scanRequested = false; isConnecting = false;
  esp_wifi_disconnect(); delay(100);
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  wifi_config_t ap_config = {};
  strcpy((char*)ap_config.ap.ssid, "CloudClock_Setup");
  ap_config.ap.max_connection = 4;
  ap_config.ap.authmode = WIFI_AUTH_OPEN;
  esp_wifi_set_config(WIFI_IF_AP, &ap_config);
  
  xTaskCreate(dns_server_task, "dns", 4096, NULL, 5, NULL);
  
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_open_sockets = 7;
  config.lru_purge_enable = true;
  if (httpd_start(&web_server, &config) == ESP_OK) {
    httpd_uri_t uri_root = {}; uri_root.uri = "/"; uri_root.method = HTTP_GET; uri_root.handler = captive_portal_handler;
    httpd_uri_t uri_rescan = {}; uri_rescan.uri = "/rescan"; uri_rescan.method = HTTP_GET; uri_rescan.handler = rescan_handler;
    httpd_uri_t uri_save = {}; uri_save.uri = "/save"; uri_save.method = HTTP_GET; uri_save.handler = save_handler;
    httpd_uri_t uri_204 = {}; uri_204.uri = "/generate_204"; uri_204.method = HTTP_GET; uri_204.handler = generate_204_handler;
    httpd_register_uri_handler(web_server, &uri_root);
    httpd_register_uri_handler(web_server, &uri_rescan);
    httpd_register_uri_handler(web_server, &uri_save);
    httpd_register_uri_handler(web_server, &uri_204);
  }
  
  esp_wifi_scan_start(NULL, true); scanRequested = true;
  bootLogoDrawn = false; tft.fillScreen(C_BG);
  tft.setFont(&fonts::FreeSansBold12pt7b); tft.setTextColor(C_ACCENT); int cw=tft.textWidth("Cloud");
  tft.setFont(&fonts::FreeSans12pt7b); int kw=tft.textWidth("Clock"); int sx=(SCREEN_W-cw-kw)/2;
  tft.setFont(&fonts::FreeSansBold12pt7b); tft.setTextColor(C_ACCENT); tft.setCursor(sx,70); tft.print("Cloud");
  tft.setFont(&fonts::FreeSans12pt7b); tft.setTextColor(C_WHITE); tft.setCursor(sx+cw,70); tft.print("Clock");
  drawHRule(82,20,SCREEN_W-20,C_BORDER);
  printCentered("Setup Required",100,&fonts::FreeSans9pt7b,C_WHITE);
  tft.setFont(&fonts::Font0); tft.setTextColor(C_ACCENT); tft.setCursor(30,130); tft.print("1. Connect to WiFi:");
  tft.setTextColor(C_WHITE); tft.setCursor(30,146); tft.print("   CloudClock_Setup");
  tft.setTextColor(C_ACCENT); tft.setCursor(30,172); tft.print("2. Open Browser:");
  tft.setTextColor(C_WHITE); tft.setCursor(30,188); tft.print("   http://192.168.4.1");
}

void processAPLoop() {
  while (inSetupMode) {
    if (scanRequested && !scanDone) {
      uint16_t number = 20;
      esp_wifi_scan_get_ap_records(&number, ap_records);
      scanResult = number; scanDone = true; scanRequested = false;
    }
    if (isConnecting) {
      showBoot(spinnerAngle, "Testing Connection..."); spinnerAngle = (spinnerAngle+16)%360; delay(25);
      wifi_ap_record_t info;
      if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        delay(500); esp_wifi_set_mode(WIFI_MODE_STA); inSetupMode = false;
        if(web_server) { httpd_stop(web_server); web_server = NULL; }
      }
      else if (millis()-connectStartTime > CONNECT_TIMEOUT) {
        esp_wifi_disconnect();
        startAP();
      }
    } else delay(10);
  }
}

extern "C" void app_main() {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase(); nvs_flash_init();
  }
  esp_netif_init(); mdns_init(); mdns_hostname_set("cloudclock"); esp_event_loop_create_default();
  esp_netif_create_default_wifi_sta();
  esp_netif_create_default_wifi_ap();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);

  gpio_set_direction(BTN_POWER_PIN, GPIO_MODE_INPUT); gpio_set_pull_mode(BTN_POWER_PIN, GPIO_PULLDOWN_ONLY);
  gpio_set_direction(BTN_DISMISS_PIN, GPIO_MODE_INPUT); gpio_set_pull_mode(BTN_DISMISS_PIN, GPIO_PULLDOWN_ONLY);
  gpio_set_direction(BUZZER_PIN, GPIO_MODE_OUTPUT); gpio_set_level(BUZZER_PIN, 0);

  tft.init(); tft.setRotation(2); tft.setBrightness(200); tft.fillScreen(C_BG);
  alarmSprite.setColorDepth(16); alarmSprite.createSprite(SCREEN_W, AL_VIEWPORT_H);

  nvs_handle_t my_handle;
  uint8_t pairedSaved = 0;
  if (nvs_open("settings", NVS_READONLY, &my_handle) == ESP_OK) {
    nvs_get_i32(my_handle, "hour_format", &hourFormat);
    size_t len = sizeof(deviceTimezone);
    nvs_get_str(my_handle, "timezone", deviceTimezone, &len);
    nvs_get_u8(my_handle, "paired", &pairedSaved);
    nvs_close(my_handle);
  }

  if (nvs_open("wifi", NVS_READONLY, &my_handle) == ESP_OK) {
    size_t len = sizeof(savedSSID); nvs_get_str(my_handle, "ssid", savedSSID, &len);
    len = sizeof(savedPASS); nvs_get_str(my_handle, "pass", savedPASS, &len);
    nvs_close(my_handle);
  }

  esp_wifi_start();

  if (strlen(savedSSID) == 0) {
    startAP();
  } else {
    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_config_t sta_config = {};
    strncpy((char*)sta_config.sta.ssid, savedSSID, sizeof(sta_config.sta.ssid));
    strncpy((char*)sta_config.sta.password, savedPASS, sizeof(sta_config.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    esp_wifi_connect();
    
    char msg[64]; snprintf(msg, sizeof(msg), "Connecting to %s...", savedSSID);
    uint32_t t0 = millis();
    wifi_ap_record_t info;
    while (esp_wifi_sta_get_ap_info(&info) != ESP_OK && millis() - t0 < 15000) {
      showBoot(spinnerAngle, msg); spinnerAngle = (spinnerAngle + 16) % 360; delay(25);
    }
    if (esp_wifi_sta_get_ap_info(&info) != ESP_OK) startAP();
  }
  if (inSetupMode) processAPLoop();

  setWifiStatus(true); bootLogoDrawn = false; tft.fillScreen(C_BG); needFullRedraw = true;

  syncNTP();

  char ws_uri[128]; snprintf(ws_uri, sizeof(ws_uri), "ws://%s:%d%s", wsHost, wsPort, wsPath);
  esp_websocket_client_config_t ws_cfg = {};
  ws_cfg.uri = ws_uri;
  // Let the esp_websocket_client handle heartbeats organically rather than explicit JSON heartbeat.
  // The default behavior is 10 sec ping interval, which fixes the problem with custom heartbeat messages overflowing.
  ws_cfg.ping_interval_sec = 15;
  ws_cfg.ping_timeout_sec = 5;

  ws_client = esp_websocket_client_init(&ws_cfg);
  esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
  esp_websocket_client_start(ws_client);

  if (pairedSaved) { currentMode = MODE_NORMAL; } else { currentMode = MODE_PAIRING; triggerCodeFetch = true; }

  xTaskCreatePinnedToCore(uiTaskCode, "ui", 10000, NULL, 1, NULL, 0);

  while (1) {
    handleButtons();
    wifi_ap_record_t info; setWifiStatus(esp_wifi_sta_get_ap_info(&info) == ESP_OK);
    
    if (DOut.cur.cloud && (!ws_client || !esp_websocket_client_is_connected(ws_client))) {
      setCloudStatus(false);
    }

    if (triggerCodeFetch) { triggerCodeFetch = false; fetchPairingCode(); }
    if (triggerSettingsSave) {
      triggerSettingsSave = false;
      if (nvs_open("settings", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_i32(my_handle, "hour_format", hourFormat);
        nvs_set_str(my_handle, "timezone", deviceTimezone);
        nvs_commit(my_handle); nvs_close(my_handle);
      }
    }
    if (triggerTZApply) { triggerTZApply = false; applyTimezone(); }

    uint32_t now = millis();
    if (now - lastAlarmCheck > 20000) { checkAlarms(); lastAlarmCheck = now; }
    if (buzzerActive && (now - buzzerStart > ALARM_RING_DURATION)) buzzerOff();
    if (currentMode == MODE_PAIRING && pairingCodeExpiry > 0 && now > pairingCodeExpiry - 60000) fetchPairingCode();

    delay(20);
  }
}
