// ============================================================================
//  CloudClock  —  ILI9488 (320×480) + XPT2046 touch  +  LovyanGFX
//  v3: Sprite-based alarm scroll (no flicker/ghosts) + Android swipe dismiss
// ============================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <HTTPClient.h>
#include <time.h>
#include <SPI.h>
#include <esp_sleep.h>
#include <LovyanGFX.hpp>
#include <XPT2046_Touchscreen.h>

// ── Alarm struct defined here so forward declarations below can reference it ──
#define MAX_ALARMS 20
struct Alarm {
  int  id, hour, minute;
  bool enabled, recurring;
  char recurType[8];
  bool recurDays[7];
};

// ============================================================================
//  TFT  (ILI9488  320×480)
// ============================================================================
class LGFX_ILI9488 : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_SPI        _bus;
public:
  LGFX_ILI9488() {
    { auto cfg = _bus.config();
      cfg.spi_host   = SPI2_HOST;
      cfg.freq_write = 40000000;
      cfg.pin_sclk   = 14;
      cfg.pin_mosi   = 13;
      cfg.pin_miso   = 6;
      cfg.pin_dc     = 8;
      _bus.config(cfg); }
    _panel.setBus(&_bus);
    { auto cfg = _panel.config();
      cfg.pin_cs        = 18;
      cfg.pin_rst       = 9;
      cfg.memory_width  = 320;
      cfg.memory_height = 480;
      cfg.panel_width   = 320;
      cfg.panel_height  = 480;
      _panel.config(cfg); }
    setPanel(&_panel);
  }
};
LGFX_ILI9488 tft;

// ── Sprite for flicker-free alarm list rendering ─────────────────────────────
// We allocate ONE sprite the size of the alarm viewport and redraw into it,
// then push it to screen in a single DMA blit.  This means the display never
// sees a half-painted frame, eliminating all flicker and ghost pixels.
lgfx::LGFX_Sprite alarmSprite(&tft);

#define SCREEN_W 320
#define SCREEN_H 480

// ============================================================================
//  Touch  (XPT2046 on HSPI)
// ============================================================================
static const int TOUCH_CS   = 10;
static const int TOUCH_IRQ  = 11;
static const int TOUCH_SCK  = 2;
static const int TOUCH_MISO = 12;
static const int TOUCH_MOSI = 15;

SPIClass touchSPI(HSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

static const int RAW_X_MIN = 521,  RAW_X_MAX = 3620;
static const int RAW_Y_MIN = 451,  RAW_Y_MAX = 3666;

bool getTouchPixel(int &sx, int &sy) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  if (p.z < 50) return false;
  int rx = constrain((int)p.x, RAW_X_MIN, RAW_X_MAX);
  int ry = constrain((int)p.y, RAW_Y_MIN, RAW_Y_MAX);
  sx = map(rx, RAW_X_MIN, RAW_X_MAX, 0, SCREEN_W - 1);
  sy = map(ry, RAW_Y_MAX, RAW_Y_MIN, 0, SCREEN_H - 1);
  return true;
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
#define C_ACCENT   RGB(0xFF,0xEE,0x55)   // warm yellow
#define C_MUTED    RGB(0x55,0x55,0x77)
#define C_DIM      RGB(0x30,0x30,0x44)
#define C_RED      RGB(0xFF,0x44,0x44)
#define C_GREEN    RGB(0x33,0xDD,0x77)
#define C_ORANGE   RGB(0xFF,0x99,0x22)

// ============================================================================
//  Layout constants
// ============================================================================
#define SB_H       56
#define DATE_Y     65
#define TC_Y       90
#define TC_H       128
#define SEC_BAR_Y  (TC_Y + TC_H - 8)
#define AL_LABEL_Y 272
#define AL_Y       284
#define AL_VIEWPORT_H  166   // alarm list viewport height
#define FOOTER_Y   (SCREEN_H - 30)
#define AL_CARD_H  68
#define AL_CARD_GAP 8

// ============================================================================
//  Pins
// ============================================================================
#define BTN_POWER_PIN   16
#define BTN_DISMISS_PIN 17
#define BUZZER_PIN       5

// ============================================================================
//  App constants / host
// ============================================================================
const char* wsHost = "165.22.208.107";
const int   wsPort = 4002;
const char* wsPath = "/ws?token=eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJkZXZpY2VfaWQiOjMsInN1YiI6ImRldmljZSJ9.FNq9AaDWARN_qkPeP6aEQcz0umbmf5WeRxNaEXMnums";
const int   deviceId            = 1;
const unsigned long ALARM_RING_DURATION = 30000;

// ============================================================================
//  AP / Portal
// ============================================================================
const byte DNS_PORT = 53;
DNSServer  dnsServer;
WebServer  server(80);
Preferences prefs;

String savedSSID, savedPASS;
bool   inSetupMode = false, isConnecting = false;
unsigned long connectStartTime = 0;
const unsigned long CONNECT_TIMEOUT = 15000;
int  scanResult = 0;
bool scanDone = false, scanRequested = false;

// ============================================================================
//  Device mode / pairing
// ============================================================================
enum DeviceMode { MODE_BOOT, MODE_PAIRING, MODE_NORMAL };
DeviceMode currentMode = MODE_BOOT;

char  pairingCode[7]       = "";
unsigned long pairingCodeExpiry = 0;
bool  pairingCodeRequested = false;
const char* pairingURL     = "cloudclock.vercal.app/pair";

// ============================================================================
//  Settings
// ============================================================================
int  hourFormat = 24;
char deviceTimezone[48] = "UTC";   // matches DB default; overwritten by sync_settings on connect

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
  return 19800;
}

// ============================================================================
//  WebSocket
// ============================================================================
WebSocketsClient ws;

// ============================================================================
//  Alarms
// ============================================================================
// (Alarm struct defined at top of file, above forward declarations)
Alarm alarms[MAX_ALARMS];
int   alarmCount      = 0;
volatile bool updatingAlarms = false;

bool triggerCodeFetch    = false;
bool triggerSettingsSave = false;
bool triggerTZApply      = false;   // only true when timezone string actually changed

// ============================================================================
//  Display state / dirty flags
// ============================================================================
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
char lastDrawnCode[7]   = "";

bool statusBarDirty = true;
bool needFullRedraw = false;

// Footer
bool lastFooterActive    = false;
unsigned long lastFooterAnimTime = 0;
int  footerAnimAngle     = 0;

// ── Alarm scroll state ───────────────────────────────────────────────────────
int  alarmScrollOffset  = 0;
int  alarmScrollVel     = 0;
int  touchStartY        = -1;
int  touchLastY         = -1;
bool touchScrolling     = false;
unsigned long touchStartMs = 0;

// ── Scroll redraw throttle ───────────────────────────────────────────────────
static bool          scrollDirty        = false;
static unsigned long lastScrollRedrawMs = 0;
static const unsigned long SCROLL_REDRAW_MS = 30;  // ~33 fps max during scroll

// ============================================================================
//  ANDROID-STYLE SWIPE-TO-DISMISS  state
// ============================================================================
// The alarm banner shows a large ring.  The user touches the inner handle and
// drags it toward the outer ring edge.  As they drag, an arc fills around the
// ring.  When the handle reaches the ring edge (progress ≥ 1.0) the alarm is
// dismissed.  Releasing before that snaps the handle back.

#define DISMISS_CX       (SCREEN_W / 2)   // ring centre x
#define DISMISS_CY       (AL_Y + 100)     // ring centre y  (inside banner)
#define DISMISS_R_OUTER  54               // outer ring radius
#define DISMISS_R_INNER  18               // draggable handle radius
#define DISMISS_R_TRACK  36               // centre of the progress arc

float swipeDragProgress = 0.0f;   // 0..1  (1 = dismiss)
bool  swipeTouching     = false;
int   swipeTouchX       = 0, swipeTouchY = 0;
bool  swipeAnimateSnap  = false;   // true while snapping back
unsigned long swipeSnapStart = 0;
float swipeSnapFrom  = 0.0f;

// Pulse animation for the ring (idle state)
unsigned long alarmAnimMs   = 0;
float         alarmPulse    = 0.0f;   // 0..1 sinusoidal

// ============================================================================
//  Buzzer
// ============================================================================
bool          buzzerActive         = false;
unsigned long buzzerStart          = 0;
int           lastFiredAlarmId     = -1;
int           lastFiredAlarmMinute = -1;
unsigned long lastHeartbeat        = 0;
unsigned long lastAlarmCheck       = 0;

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

// ============================================================================
//  DRAW PRIMITIVES
// ============================================================================

void fillCard(int x, int y, int w, int h, int r, uint16_t bg, uint16_t border=0) {
  tft.fillRoundRect(x, y, w, h, r, bg);
  if (border) tft.drawRoundRect(x, y, w, h, r, border);
}

// fillCard variant for drawing into a sprite
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

// ── Spinner (8-dot fading arc) ───────────────────────────────────────────────
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

void drawHRule(int y, int x0=0, int x1=SCREEN_W, uint16_t col=C_BORDER) {
  tft.drawFastHLine(x0, y, x1-x0, col);
}

// ============================================================================
//  VECTOR ICONS
// ============================================================================

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

void drawIconRepeat(int cx, int cy, uint16_t col) {
  for (int a = 30; a <= 320; a += 6) {
    float r = 0.01745329f * a;
    tft.drawPixel(cx + (int)(cosf(r)*7), cy + (int)(sinf(r)*7), col);
    tft.drawPixel(cx + (int)(cosf(r)*8), cy + (int)(sinf(r)*8), col);
  }
  float ra = 320 * 0.01745329f;
  int ax = cx + (int)(cosf(ra)*7), ay = cy + (int)(sinf(ra)*7);
  tft.drawLine(ax, ay, ax+3, ay-4, col);
  tft.drawLine(ax, ay, ax+4, ay+2, col);
}

// ── Repeat icon drawn into a sprite ──────────────────────────────────────────
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

// ============================================================================
//  STATUS BAR
// ============================================================================
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

// ============================================================================
//  BOOT SCREEN
// ============================================================================
void showBoot(int angle, const char* msg) {
  if (!bootLogoDrawn) {
    tft.fillScreen(C_BG);
    tft.setFont(&fonts::FreeSansBold24pt7b);
    int wCloud = tft.textWidth("Cloud");
    int wClock = tft.textWidth("Clock");
    int sx = (SCREEN_W - wCloud - wClock) / 2;
    tft.setTextColor(C_ACCENT);
    tft.setCursor(sx, 210);
    tft.print("Cloud");
    tft.setTextColor(C_WHITE);
    tft.setCursor(sx + wCloud, 210);
    tft.print("Clock");
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

// ============================================================================
//  NORMAL UI — HELPERS
// ============================================================================
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
  snprintf(buf, sizeof(buf), "%s, %d %s %d",
    DAY_NAMES[t.tm_wday], t.tm_mday, MON_NAMES[t.tm_mon], t.tm_year+1900);
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
  if (filled > 0)
    tft.fillRect(barX, SEC_BAR_Y, filled, 5, C_ACCENT);
}

void drawTimeCard(struct tm& t) {
  drawTimeCardFull(t);
  drawSecondsBar(t.tm_sec);
}

// ============================================================================
//  SPRITE-BASED ALARM LIST — zero flicker, zero ghost pixels
//
//  How it works:
//  1. alarmSprite is SCREEN_W × AL_VIEWPORT_H pixels.
//  2. Before any card drawing we fill the sprite with C_BG — so ALL old pixels
//     are wiped atomically.
//  3. Each alarm card is drawn into the sprite at sprite-local y coords.
//     Because the sprite has finite height, any card pixels outside [0, AL_VIEWPORT_H)
//     are simply never written (natural clip).
//  4. One pushSprite() call DMA-blits the whole viewport to screen.
//     The display sees only the finished frame — no partial redraws visible.
// ============================================================================

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
  int maxOff = max(0, totalH - AL_VIEWPORT_H);
  if (alarmScrollOffset < 0)      alarmScrollOffset = 0;
  if (alarmScrollOffset > maxOff) alarmScrollOffset = maxOff;
}

// Draw one alarm card entirely within the sprite coordinate system.
// spriteY = card top in sprite-local pixels (may be negative or beyond sprite height).
void drawAlarmCardIntoSprite(lgfx::LGFX_Sprite& spr, int spriteY, Alarm& a, bool isNext) {
  // Fully outside sprite? Skip.
  if (spriteY + AL_CARD_H <= 0)          return;
  if (spriteY >= (int)spr.height())      return;

  uint16_t bgCol  = isNext  ? C_SURFACE2 : C_SURFACE;
  uint16_t bdCol  = isNext  ? C_ACCENT   : C_BORDER;
  uint16_t txtCol = a.enabled ? C_WHITE  : C_MUTED;

  // LovyanGFX sprites clip automatically at their boundaries,
  // so partial cards at top/bottom are drawn correctly.
  fillCardS(spr, 14, spriteY, SCREEN_W-28, AL_CARD_H, 12, bgCol, bdCol);

  if (isNext)
    spr.fillRoundRect(14, spriteY, 5, AL_CARD_H, 2, C_ACCENT);

  // Time label
  char tbuf[12];
  formatTimeStr(a.hour, a.minute, tbuf, sizeof(tbuf));
  spr.setFont(&fonts::FreeSansBold12pt7b);
  spr.setTextColor(txtCol, bgCol);
  spr.setCursor(28, spriteY + 28);
  spr.print(tbuf);

  // NEXT badge
  if (isNext) {
    spr.setFont(&fonts::Font0);
    spr.fillRoundRect(SCREEN_W-60, spriteY+8, 34, 14, 4, C_ACCENT);
    spr.setTextColor(C_BG, C_ACCENT);
    int nw = spr.textWidth("NEXT");
    spr.setCursor(SCREEN_W-60 + (34-nw)/2, spriteY+13);
    spr.print("NEXT");
  }

  // Recur info
  if (a.recurring) {
    spr.setFont(&fonts::Font0);
    spr.setTextColor(C_MUTED, bgCol);
    if (strcmp(a.recurType,"daily") == 0) {
      spr.setCursor(28, spriteY + 48);
      spr.print("Every day");
    } else if (strcmp(a.recurType,"weekly") == 0) {
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
    spr.setFont(&fonts::Font0);
    spr.setTextColor(C_DIM, bgCol);
    spr.setCursor(28, spriteY+48);
    spr.print("One-time");
  }

  if (!a.enabled) {
    spr.setFont(&fonts::Font0);
    spr.setTextColor(C_RED, bgCol);
    spr.setCursor(SCREEN_W-70, spriteY+48);
    spr.print("OFF");
  }
}

// Full alarm section redraw via sprite.
void drawAlarmSection(int nextIdx) {
  // ── Clear the label strip above the viewport ──────────────────────────────
  // Use a generous rect so old "X ALARMS" text / scroll arrows can't linger.
  tft.fillRect(0, AL_LABEL_Y - 4, SCREEN_W, AL_Y - (AL_LABEL_Y - 4), C_BG);

  if (alarmCount == 0) {
    // Wipe entire viewport too
    tft.fillRect(0, AL_Y, SCREEN_W, AL_VIEWPORT_H, C_BG);
    fillCard(14, AL_Y + 20, SCREEN_W-28, 60, 12, C_SURFACE, C_BORDER);
    printCentered("No alarms set", AL_Y + 56, &fonts::FreeSans9pt7b, C_MUTED);
    return;
  }

  // ── Section header ─────────────────────────────────────────────────────────
  tft.setFont(&fonts::Font0);
  tft.setTextColor(C_MUTED, C_BG);
  char hdrBuf[16];
  if (alarmCount > 1) snprintf(hdrBuf, sizeof(hdrBuf), "%d ALARMS", alarmCount);
  else strcpy(hdrBuf, "1 ALARM");
  int hw = tft.textWidth(hdrBuf);
  tft.setCursor((SCREEN_W-hw)/2, AL_LABEL_Y);
  tft.print(hdrBuf);

  // Scroll hint arrows drawn directly onto screen (above / below viewport)
  int totalH = alarmCount * (AL_CARD_H + AL_CARD_GAP) - AL_CARD_GAP;
  if (totalH > AL_VIEWPORT_H) {
    tft.setFont(&fonts::Font0);
    tft.setTextColor(C_DIM, C_BG);
    if (alarmScrollOffset > 0)
      printCentered("^", AL_Y - 10, &fonts::Font0, C_DIM);
    if (alarmScrollOffset < totalH - AL_VIEWPORT_H)
      printCentered("v", AL_Y + AL_VIEWPORT_H + 2, &fonts::Font0, C_DIM);
  }

  // ── Render all cards into the sprite, then blit once ──────────────────────
  alarmSprite.fillScreen(C_BG);   // ← wipes every old pixel atomically

  for (int i = 0; i < alarmCount; i++) {
    int spriteY = i * (AL_CARD_H + AL_CARD_GAP) - alarmScrollOffset;
    drawAlarmCardIntoSprite(alarmSprite, spriteY, alarms[i], i == nextIdx);
  }

  // Single blit — display only ever sees the completed frame
  alarmSprite.pushSprite(0, AL_Y);
}

// ============================================================================
//  ANDROID-STYLE SWIPE-TO-DISMISS BANNER
// ============================================================================
//
//  Visual structure (all coords in screen pixels):
//
//   ┌────────────────────────────── banner card ──────────────────────────────┐
//   │  "ALARM"  (text top)                                                     │
//   │                                                                          │
//   │           ○  outer ring (DISMISS_R_OUTER)                               │
//   │         ╱ ╲                                                              │
//   │        │  ● handle (DISMISS_R_INNER) — user drags this outward          │
//   │         ╲ ╱                                                              │
//   │           ╰── arc fills clockwise as handle moves out                   │
//   │                                                                          │
//   │  "Slide out to dismiss"  (text bottom)                                  │
//   └─────────────────────────────────────────────────────────────────────────┘
//
//  Progress is computed as: dist(handle, centre) / (DISMISS_R_OUTER - DISMISS_R_INNER)
//  clamped to [0, 1].  Arc spans 0 → 360° × progress.

// Draw a thick arc in screen coords (LovyanGFX doesn't have thick arcs natively)
// We draw concentric thin arcs to fake thickness.
void drawThickArc(int cx, int cy, int r, int thickness,
                  float startDeg, float endDeg, uint16_t col) {
  if (endDeg <= startDeg) return;
  float step = 1.5f;   // degree step — fine enough to look smooth at these radii
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
  // ── Banner background ──────────────────────────────────────────────────────
  // Cover the entire alarm section area
  tft.fillRect(0, AL_LABEL_Y - 4, SCREEN_W,
               SCREEN_H - (AL_LABEL_Y - 4) - 30, C_BG);

  uint16_t bannerBg = RGB(0x12, 0x08, 0x00);
  fillCard(14, AL_Y - 10, SCREEN_W-28, 190, 18, bannerBg, C_ORANGE);

  // ── "ALARM" heading ────────────────────────────────────────────────────────
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextColor(C_ACCENT, bannerBg);
  int tw = tft.textWidth("ALARM RINGING");
  tft.setCursor((SCREEN_W - tw)/2, AL_Y + 4);
  tft.print("ALARM RINGING");

  // ── Progress arc ───────────────────────────────────────────────────────────
  int cx = DISMISS_CX;
  int cy = DISMISS_CY;

  // Background (dim) full ring
  drawThickArc(cx, cy, DISMISS_R_OUTER, 5, 0, 360, C_DIM);

  // Filled arc — spans progress × 360°, starting from -90° (top) for natural feel
  if (swipeDragProgress > 0.005f) {
    float arcEnd = swipeDragProgress * 360.0f;
    // Blend orange → green as progress increases
    uint8_t rr = (uint8_t)(0xFF - (uint8_t)(0xCC * swipeDragProgress));
    uint8_t gg = (uint8_t)(0x55 + (uint8_t)(0x88 * swipeDragProgress));
    uint8_t bb = 0x22;
    uint16_t arcCol = RGB(rr, gg, bb);
    drawThickArc(cx, cy, DISMISS_R_OUTER, 5, -90, -90 + arcEnd, arcCol);
  }

  // ── Pulsing outer glow (idle animation) ────────────────────────────────────
  if (!swipeTouching) {
    float pulse = (sinf(alarmPulse) + 1.0f) * 0.5f;   // 0..1
    uint8_t alpha = (uint8_t)(40 + 40 * pulse);
    // Draw 2-pixel glow ring just outside outer ring
    drawThickArc(cx, cy, DISMISS_R_OUTER + 6, 3, 0, 360,
                 RGB(alpha, (uint8_t)(alpha * 0.6), 0));
  }

  // ── Inner handle ───────────────────────────────────────────────────────────
  // Position: moves from centre outward toward DISMISS_R_TRACK as progress→1
  float handleDist = swipeDragProgress * (DISMISS_R_TRACK - 2);
  // Direction: toward touch point (or upward if not touching)
  float hAngle = -1.5708f; // -90° = upward default
  if (swipeTouching) {
    float dx = swipeTouchX - cx;
    float dy = swipeTouchY - cy;
    if (fabsf(dx) + fabsf(dy) > 4) hAngle = atan2f(dy, dx);
  }
  int hx = cx + (int)(cosf(hAngle) * handleDist);
  int hy = cy + (int)(sinf(hAngle) * handleDist);

  // Shadow / glow behind handle
  tft.fillCircle(hx, hy, DISMISS_R_INNER + 4, RGB(0x30,0x18,0x00));
  // Handle body — colour shifts orange→green as progress grows
  uint8_t hr2 = (uint8_t)(0xFF - (uint8_t)(0xCC * swipeDragProgress));
  uint8_t hg2 = (uint8_t)(0x88 + (uint8_t)(0x77 * swipeDragProgress));
  tft.fillCircle(hx, hy, DISMISS_R_INNER, RGB(hr2, hg2, 0x22));

  // Arrow chevrons on handle (pointing outward)
  // Draw two small lines indicating "push out"
  {
    float perpA = hAngle + 1.5708f;
    int ax1 = hx + (int)(cosf(hAngle)*4 + cosf(perpA)*5);
    int ay1 = hy + (int)(sinf(hAngle)*4 + sinf(perpA)*5);
    int ax2 = hx + (int)(cosf(hAngle)*9);
    int ay2 = hy + (int)(sinf(hAngle)*9);
    int ax3 = hx + (int)(cosf(hAngle)*4 - cosf(perpA)*5);
    int ay3 = hy + (int)(sinf(hAngle)*4 - sinf(perpA)*5);
    tft.drawLine(ax1, ay1, ax2, ay2, C_WHITE);
    tft.drawLine(ax3, ay3, ax2, ay2, C_WHITE);
  }

  // ── Sub-label ──────────────────────────────────────────────────────────────
  tft.setFont(&fonts::Font0);
  tft.setTextColor(C_MUTED, bannerBg);
  const char* sub = (swipeDragProgress > 0.5f) ? "Release to dismiss!" : "Slide to dismiss";
  int sw2 = tft.textWidth(sub);
  tft.setCursor((SCREEN_W - sw2)/2, AL_Y + 168);
  tft.print(sub);
}

// ── Touch handler for the swipe dismiss interaction ──────────────────────────
void handleSwipeTouch() {
  int tx, ty;
  bool touched = getTouchPixel(tx, ty);

  // Update pulse animation
  alarmPulse += 0.06f;
  if (alarmPulse > 6.2832f) alarmPulse -= 6.2832f;

  if (touched) {
    swipeTouchX = tx;
    swipeTouchY = ty;

    // Accept touch anywhere near the banner area
    if (ty >= AL_Y - 20 && ty <= AL_Y + 200) {
      swipeTouching = true;
      swipeAnimateSnap = false;

      // Compute distance from ring centre
      float dx = tx - DISMISS_CX;
      float dy = ty - DISMISS_CY;
      float dist = sqrtf(dx*dx + dy*dy);

      // Progress = how far toward outer ring edge
      float maxDist = (float)(DISMISS_R_OUTER - DISMISS_R_INNER);
      swipeDragProgress = constrain(dist / maxDist, 0.0f, 1.0f);

      // Dismissed!
      if (swipeDragProgress >= 0.98f) {
        buzzerOff();
        swipeDragProgress = 0.0f;
        swipeTouching = false;
      }
    }
  } else {
    if (swipeTouching) {
      // Finger lifted — snap back unless already dismissed
      swipeTouching = false;
      if (swipeDragProgress > 0.0f) {
        swipeAnimateSnap = true;
        swipeSnapFrom    = swipeDragProgress;
        swipeSnapStart   = millis();
      }
    }

    // Animate snap-back
    if (swipeAnimateSnap) {
      float elapsed = (float)(millis() - swipeSnapStart);
      float t = elapsed / 300.0f;   // 300 ms snap duration
      if (t >= 1.0f) {
        swipeDragProgress = 0.0f;
        swipeAnimateSnap  = false;
      } else {
        // Ease-out cubic snap
        float ease = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        swipeDragProgress = swipeSnapFrom * (1.0f - ease);
      }
    }
  }
}

// ============================================================================
//  TOUCH HANDLER for alarm LIST scroll
// ============================================================================
void handleAlarmTouch() {
  int tx, ty;
  bool touched = getTouchPixel(tx, ty);

  if (touched) {
    if (!touchScrolling) {
      touchStartY    = ty;
      touchLastY     = ty;
      touchStartMs   = millis();
      touchScrolling = true;
      alarmScrollVel = 0;
    } else {
      int dy = touchLastY - ty;
      if (dy != 0) {
        alarmScrollOffset += dy;
        clampAlarmScroll();
        alarmScrollVel = dy;
        touchLastY     = ty;
        scrollDirty    = true;
      }
    }
  } else {
    if (touchScrolling) touchScrolling = false;
    if (alarmScrollVel != 0) {
      alarmScrollOffset += alarmScrollVel;
      clampAlarmScroll();
      alarmScrollVel = (int)(alarmScrollVel * 0.72f);
      if (abs(alarmScrollVel) <= 1) alarmScrollVel = 0;
      scrollDirty = true;
    }
  }
}

// ============================================================================
//  FOOTER
// ============================================================================
void updateFooter() {
  bool need = !DOut.cur.cloud || !DOut.cur.wifi;
  if (need) {
    if (millis() - lastFooterAnimTime > 30) {
      lastFooterAnimTime = millis();
      footerAnimAngle = (footerAnimAngle + 20) % 360;
      if (!lastFooterActive) {
        tft.fillRect(0, FOOTER_Y, SCREEN_W, 30, C_SURFACE);
        drawHRule(FOOTER_Y, 0, SCREEN_W, C_BORDER);
      }
      drawSpinner(20, FOOTER_Y+15, 7, footerAnimAngle, C_SURFACE);
      tft.setFont(&fonts::Font0);
      tft.setTextColor(C_MUTED, C_SURFACE);
      tft.setCursor(38, FOOTER_Y+10);
      if (!DOut.cur.wifi) tft.print("Connecting to WiFi...       ");
      else                tft.print("Connecting to Cloud...      ");
      lastFooterActive = true;
    }
  } else if (lastFooterActive) {
    tft.fillRect(0, FOOTER_Y, SCREEN_W, 30, C_BG);
    lastFooterActive = false;
  }
}

// ============================================================================
//  updateNormalUI  — dirty-region rendering
// ============================================================================
void updateNormalUI() {
  if (needFullRedraw) {
    tft.fillScreen(C_BG);
    needFullRedraw = false;
    statusBarDirty = true;
    currentDisplayedMin  = -1;
    currentDisplayedSec  = -1;
    currentDisplayedAlarmHash = -999;
    currentDisplayedBuzzer = !buzzerActive;
    swipeDragProgress = 0.0f;
    swipeTouching = false;
    swipeAnimateSnap = false;
  }

  if (statusBarDirty ||
      DOut.cur.wifi  != DOut.prev.wifi ||
      DOut.cur.cloud != DOut.prev.cloud) {
    drawStatusBar();
  }

  struct tm t;
  bool timeOK = getLocalTm(&t);

  static int lastDay = -1;
  if (timeOK) {
    if (t.tm_mday != lastDay || currentDisplayedMin == -1) {
      drawDateRow(t);
      lastDay = t.tm_mday;
    }
  }

  if (timeOK) {
    bool minChg = (t.tm_min != currentDisplayedMin) || (currentDisplayedMin == -1);
    bool secChg = (t.tm_sec != currentDisplayedSec);
    if (minChg) {
      drawTimeCardFull(t);
      drawSecondsBar(t.tm_sec);
      currentDisplayedMin = t.tm_min;
      currentDisplayedSec = t.tm_sec;
      currentDisplayedAlarmHash = -999;
    } else if (secChg) {
      drawSecondsBar(t.tm_sec);
      currentDisplayedSec = t.tm_sec;
    }
  }

  // ── Buzzer banner vs alarm section ─────────────────────────────────────────
  if (buzzerActive) {
    // Always update the swipe banner (handles touch + animation)
    handleSwipeTouch();
    // Redraw banner every frame for smooth animation
    // (only the alarm section area; time card untouched)
    drawSwipeDismissBanner();
    currentDisplayedBuzzer = true;
  } else {
    if (currentDisplayedBuzzer) {
      // Buzzer just turned off — force full alarm section redraw
      currentDisplayedAlarmHash = -999;
      currentDisplayedBuzzer = false;
    }

    handleAlarmTouch();

    int nextIdx = getNextUpcomingAlarmIndex();
    int hashNow = computeAlarmHash() ^ (nextIdx + 100);

    bool contentChanged = (hashNow != currentDisplayedAlarmHash);
    bool scrollReady    = scrollDirty &&
                          (millis() - lastScrollRedrawMs >= SCROLL_REDRAW_MS);

    if (contentChanged || scrollReady) {
      drawAlarmSection(nextIdx);
      currentDisplayedAlarmHash = hashNow;
      if (scrollDirty) {
        scrollDirty        = false;
        lastScrollRedrawMs = millis();
      }
    }
  }
}

// ============================================================================
//  PAIRING SCREEN
// ============================================================================
void drawPairingScreen(const char* code) {
  if (strcmp(lastDrawnCode, code) == 0 && pairingScreenDrawn) return;

  tft.fillRect(0, SB_H, SCREEN_W, SCREEN_H - SB_H - 30, C_BG);

  printCentered("PAIR YOUR CLOCK", SB_H + 18, &fonts::Font0, C_MUTED);

  fillCard(18, SB_H+34, SCREEN_W-36, 106, 16, C_SURFACE, C_BORDER);
  tft.setFont(&fonts::FreeSansBold24pt7b);
  tft.setTextColor(C_ACCENT);
  int cw = tft.textWidth(code);
  int fh = tft.fontHeight();
  tft.setCursor((SCREEN_W-cw)/2, SB_H + 34 + (106 + fh)/2 - 6);
  tft.print(code);

  tft.setFont(&fonts::Font0);
  printCentered("Code expires in 10 minutes", SB_H+154, &fonts::Font0, C_MUTED);

  drawHRule(SB_H+168, 20, SCREEN_W-20, C_BORDER);

  fillCard(18, SB_H+180, SCREEN_W-36, 160, 12, C_SURFACE);
  int tx = 36, ty = SB_H + 202;
  tft.setFont(&fonts::Font0);
  tft.setTextColor(C_ACCENT, C_SURFACE); tft.setCursor(tx, ty);      tft.print("1  ");
  tft.setTextColor(C_WHITE,  C_SURFACE); tft.print("Open a browser and go to:");
  tft.setTextColor(C_ACCENT, C_SURFACE);
  int uw = tft.textWidth(pairingURL);
  tft.setCursor((SCREEN_W-uw)/2, ty+18); tft.print(pairingURL);
  tft.setTextColor(C_ACCENT, C_SURFACE); tft.setCursor(tx, ty+38);   tft.print("2  ");
  tft.setTextColor(C_WHITE,  C_SURFACE); tft.print("Enter the 6-digit code above");
  tft.setTextColor(C_MUTED,  C_SURFACE); tft.setCursor(tx, ty+58);
  tft.print("Your clock activates once paired.");

  drawHRule(SB_H+356, 20, SCREEN_W-20, C_DIM);
  tft.setTextColor(C_DIM, C_BG);
  printCentered("Hold POWER 3s = sleep  |  10s = reset", SB_H+368, &fonts::Font0, C_DIM);

  DOut.prev.wifi  = !DOut.cur.wifi;
  DOut.prev.cloud = !DOut.cur.cloud;
  statusBarDirty  = true;
  pairingScreenDrawn = true;
  strncpy(lastDrawnCode, code, 7);
}

void updatePairingUI() {
  if (needFullRedraw) {
    tft.fillScreen(C_BG);
    needFullRedraw = false;
    statusBarDirty = true;
  }
  if (statusBarDirty ||
      DOut.cur.wifi  != DOut.prev.wifi ||
      DOut.cur.cloud != DOut.prev.cloud) {
    drawStatusBar();
  }
  if (pairingCode[0] != 0) {
    drawPairingScreen(pairingCode);
  } else if (!pairingScreenDrawn) {
    tft.fillRect(0, SB_H, SCREEN_W, SCREEN_H-SB_H-30, C_BG);
    printCentered("Requesting pairing code...", SCREEN_H/2, &fonts::Font0, C_MUTED);
    pairingScreenDrawn = true;
  }
}

// ============================================================================
//  UI TASK  (Core 0)
// ============================================================================
void uiTaskCode(void* parameter) {
  for (;;) {
    if      (currentMode == MODE_NORMAL)  updateNormalUI();
    else if (currentMode == MODE_PAIRING) updatePairingUI();
    updateFooter();
    delay(8);
  }
}

// ============================================================================
//  BUTTONS
// ============================================================================
unsigned long btnPowerPressedAt = 0;
bool btnPowerHeld = false;

void handleButtons() {
  static bool lastDismiss = false;
  bool curDismiss = digitalRead(BTN_DISMISS_PIN);
  // Hardware button still works as instant dismiss (no swipe required)
  if (curDismiss && !lastDismiss && buzzerActive) buzzerOff();
  lastDismiss = curDismiss;

  static bool lastPower = false;
  bool curPower = digitalRead(BTN_POWER_PIN);
  if (curPower && !lastPower) { btnPowerPressedAt = millis(); btnPowerHeld = true; }
  if (!curPower && lastPower && btnPowerHeld) {
    unsigned long held = millis() - btnPowerPressedAt;
    if (held >= 10000) {
      tft.fillScreen(C_BG);
      printCentered("Resetting...", SCREEN_H/2, &fonts::FreeSans12pt7b, C_ACCENT);
      delay(500);
      prefs.begin("wifi",false); prefs.clear(); prefs.end();
      prefs.begin("settings",false); prefs.putBool("paired",false); prefs.end();
      delay(500); ESP.restart();
    } else if (held >= 3000) {
      if (buzzerActive) buzzerOff();
      tft.fillScreen(C_BG);
      printCentered("Sleeping...", SCREEN_H/2, &fonts::FreeSans12pt7b, C_MUTED);
      delay(600);
      tft.fillScreen(C_BG);
      esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_POWER_PIN, 1);
      esp_deep_sleep_start();
    }
    btnPowerHeld = false;
  }
  lastPower = curPower;
}

// ============================================================================
//  CAPTIVE PORTAL
// ============================================================================
String generatePage() {
  String html = R"rawhtml(<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CloudClock Setup</title>
<style>
@import url('https://fonts.googleapis.com/css2?family=DM+Sans:wght@300;400;500;600&display=swap');
:root{--accent:#FFEE8C;--bg:#0f0f13;--surface:#1a1a22;--surface2:#23232f;
--border:#2e2e3d;--text:#e8e8f0;--muted:#7a7a96;--radius:14px;}
*{box-sizing:border-box;margin:0;padding:0;}
body{font-family:'DM Sans',sans-serif;background:var(--bg);color:var(--text);
min-height:100vh;display:flex;align-items:center;justify-content:center;padding:24px;}
.card{background:var(--surface);border:1px solid var(--border);border-radius:24px;
padding:40px 36px;width:100%;max-width:420px;}
.logo-row{display:flex;align-items:center;gap:12px;margin-bottom:28px;}
.logo-text{font-size:20px;font-weight:600;}
.logo-text span{color:var(--accent);font-weight:900;}
h2{font-size:14px;color:var(--muted);margin-bottom:28px;}
.field{margin-bottom:16px;}
label{display:block;font-size:11px;font-weight:600;letter-spacing:.8px;
text-transform:uppercase;color:var(--muted);margin-bottom:8px;}
select,input[type="password"]{width:100%;background:var(--surface2);
border:1px solid var(--border);border-radius:var(--radius);color:var(--text);
font-size:15px;padding:13px 16px;outline:none;}
.btn{width:100%;border-radius:var(--radius);font-size:15px;font-weight:600;
padding:14px;cursor:pointer;margin-top:8px;border:none;}
.btn-p{background:var(--accent);color:#111;}
.btn-s{background:transparent;color:var(--muted);border:1px solid var(--border);}
.divider{height:1px;background:var(--border);margin:24px 0;}
</style>)rawhtml";

  if (!scanDone) {
    html += R"rawhtml(<meta http-equiv='refresh' content='2'></head><body>
<div class="card"><div class="logo-row"><div class="logo-text">
<span>Cloud</span>Clock</div></div>
<p style="color:var(--muted);font-size:14px;">Scanning networks...</p>
</div></body></html>)rawhtml";
    return html;
  }
  html += R"rawhtml(</head><body><div class="card">
<div class="logo-row"><div class="logo-text"><span>Cloud</span>Clock</div></div>
<h2>Connect your device to WiFi to enable time sync and cloud features.</h2>
<form action='/save' method='get'>
<div class="field"><label>Network</label><select name='ssid'>)rawhtml";
  for (int i = 0; i < scanResult; i++)
    html += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + "</option>";
  html += R"rawhtml(</select></div>
<div class="field"><label>Password</label>
<input type="password" name="pass" placeholder="Enter WiFi password"></div>
<button type="submit" class="btn btn-p">Connect</button>
</form><div class="divider"></div>
<button class="btn btn-s" onclick="location.href='/rescan'">↻ Rescan</button>
</div></body></html>)rawhtml";
  return html;
}

void handleCaptivePortal(){server.sendHeader("Location","http://192.168.4.1/",true);server.send(302,"text/plain","");}
void handleRoot(){if(!scanRequested&&!scanDone){WiFi.scanNetworks(true);scanRequested=true;}server.send(200,"text/html",generatePage());}
void handleRescan(){WiFi.scanDelete();scanDone=false;scanRequested=false;server.sendHeader("Location","/",true);server.send(302,"text/plain","");}
void handleSave(){
  prefs.begin("wifi",false);prefs.putString("ssid",server.arg("ssid"));prefs.putString("pass",server.arg("pass"));prefs.end();
  server.send(200,"text/html","<html><body style='background:#0f0f13;color:#e8e8f0;font-family:sans-serif;text-align:center;padding:20vh'><h3>Connecting...</h3></body></html>");
  delay(100);WiFi.mode(WIFI_AP_STA);WiFi.begin(server.arg("ssid").c_str(),server.arg("pass").c_str());isConnecting=true;connectStartTime=millis();
}

// ============================================================================
//  WEBSOCKET + BUZZER
// ============================================================================
void setWifiStatus(bool v)  { DOut.cur.wifi  = v; }
void setCloudStatus(bool v) { DOut.cur.cloud = v; }

void sendBuzzerStatus(bool state) {
  StaticJsonDocument<128> doc;
  doc["type"]="buzzer_status"; doc["value"]=state;
  String j; serializeJson(doc,j); ws.sendTXT(j);
}
void buzzerOn()  {
  digitalWrite(BUZZER_PIN,HIGH);
  buzzerActive=true;
  buzzerStart=millis();
  swipeDragProgress=0.0f;
  swipeTouching=false;
  swipeAnimateSnap=false;
  sendBuzzerStatus(true);
}
void buzzerOff() {
  digitalWrite(BUZZER_PIN,LOW);
  buzzerActive=false;
  swipeDragProgress=0.0f;
  swipeTouching=false;
  swipeAnimateSnap=false;
  sendBuzzerStatus(false);
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  Serial.printf("[WS] type=%d\n", type);
  if (payload && length > 0) Serial.printf("  payload: %.*s\n", (int)length, payload);

  switch (type) {
    case WStype_CONNECTED:    setCloudStatus(true);  break;
    case WStype_DISCONNECTED: setCloudStatus(false); break;
    case WStype_TEXT: {
      DynamicJsonDocument doc(4096);
      if (deserializeJson(doc, payload)) return;
      const char* msgType = doc["type"] | "";

      if      (strcmp(msgType,"buzzer")==0)     { (doc["value"]|false)?buzzerOn():buzzerOff(); }
      else if (strcmp(msgType,"alarm_fire")==0) { buzzerOn(); }
      else if (strcmp(msgType,"sync_alarms")==0) {
        if (!doc.containsKey("alarms")) break;
        JsonArray arr = doc["alarms"].as<JsonArray>();
        updatingAlarms = true; alarmCount = 0;
        for (JsonObject obj : arr) {
          if (alarmCount >= MAX_ALARMS) break;
          Alarm& a = alarms[alarmCount];
          a.id=obj["id"]|0; a.enabled=obj["enabled"]|true;
          const char* t2=obj["time"]|"";
          if (sscanf(t2,"%d:%d",&a.hour,&a.minute)!=2) continue;
          a.recurring=obj["recurring"]|false;
          strncpy(a.recurType,obj["recur_type"]|"",sizeof(a.recurType)-1);
          memset(a.recurDays,0,sizeof(a.recurDays));
          if (obj.containsKey("recur_days")&&!obj["recur_days"].isNull()) {
            JsonArray days=obj["recur_days"].as<JsonArray>();
            for (int d:days) if(d>=0&&d<=6) a.recurDays[d]=true;
          }
          alarmCount++;
        }
        updatingAlarms=false;
        alarmScrollOffset=0; alarmScrollVel=0;
        currentDisplayedAlarmHash=-999;
      }
      else if (strcmp(msgType,"sync_settings")==0) {
        int newFmt = doc["hour_format"] | 24;
        const char* newTZ = doc["timezone"] | "UTC";

        bool fmtChanged = (newFmt != hourFormat);
        bool tzChanged  = (strcmp(newTZ, deviceTimezone) != 0);

        if (fmtChanged) {
          hourFormat = newFmt;
          currentDisplayedMin = -1;   // force time card redraw
        }
        if (tzChanged) {
          strncpy(deviceTimezone, newTZ, sizeof(deviceTimezone) - 1);
          deviceTimezone[sizeof(deviceTimezone) - 1] = '\0';
          triggerTZApply = true;      // only set when TZ actually changed
          currentDisplayedMin = -1;
        }
        // Always persist if either changed — but only call configTime if TZ changed
        if (fmtChanged || tzChanged) {
          triggerSettingsSave = true;
        }
      }
      else if (strcmp(msgType,"pairing_complete")==0) {
        prefs.begin("settings",false); prefs.putBool("paired",true); prefs.end();
        currentMode=MODE_NORMAL;
        pairingScreenDrawn=false;
        needFullRedraw=true;
      }
      else if (strcmp(msgType,"request_pairing_code")==0) {
        bool already; prefs.begin("settings",true); already=prefs.getBool("paired",false); prefs.end();
        if (!already) triggerCodeFetch=true;
      }
      break;
    }
    default: break;
  }
}

// ============================================================================
//  ALARM LOGIC
// ============================================================================
void checkAlarms() {
  struct tm t;
  if (!getLocalTm(&t)) return;
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
  struct tm t;
  if (!getLocalTm(&t)) return -1;
  int nowMins=t.tm_hour*60+t.tm_min, minDiff=999999, nextIdx=-1;
  for (int i=0;i<alarmCount;i++){
    Alarm& a=alarms[i];
    if (!a.enabled) continue;
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

// ============================================================================
//  NTP / Timezone  — UTC-only SNTP, software offset
// ============================================================================
//
//  SNTP runs in UTC (offset=0) and is started exactly ONCE.  It is never
//  restarted.  Timezone conversion happens entirely in getLocalTm() by adding
//  the stored offset to the UTC epoch.  This means:
//    • No configTime() calls after boot  →  no lwIP stalls  →  stable WS
//    • Timezone changes take effect instantly without touching the network
// ============================================================================

bool getLocalTm(struct tm* t) {
  time_t now = time(nullptr);
  if (now < 1000000000UL) return false;   // epoch not synced yet
  now += getOffsetForTimezone(deviceTimezone);
  gmtime_r(&now, t);
  return true;
}

void syncNTP() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");  // UTC, once, forever
  struct tm t;
  unsigned long sw = millis();
  while (millis() - sw < 20000) {
    showBoot(spinnerAngle, "Syncing Time...");
    spinnerAngle = (spinnerAngle + 16) % 360;
    if (getLocalTm(&t)) return;
    delay(25);
  }
}

// No-op — kept so existing triggerTZApply call sites compile.
// Timezone is now applied live in getLocalTm(); no SNTP restart needed.
void applyTimezone() {}

// ============================================================================
//  PAIRING CODE FETCH
// ============================================================================
void fetchPairingCode() {
  HTTPClient http;
  String url=String("http://")+wsHost+":"+String(wsPort)+"/api/pair/code";
  http.begin(url);
  http.addHeader("Authorization",
    String("Bearer ")+String(wsPath).substring(String(wsPath).indexOf("token=")+6));
  int code=http.GET();
  if (code!=200){http.end();return;}
  String payload=http.getString(); http.end();
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc,payload)){return;}
  const char* c=doc["code"]|"";
  if (strlen(c)==6){
    strncpy(pairingCode,c,7);
    pairingCodeExpiry=millis()+9*60*1000;
    pairingCodeRequested=true;
    currentMode=MODE_PAIRING;
    pairingScreenDrawn=false;
  }
}

// ============================================================================
//  AP MODE
// ============================================================================
void startAP() {
  inSetupMode=true; scanDone=false; scanRequested=false; isConnecting=false;
  WiFi.disconnect(true); delay(100);
  WiFi.mode(WIFI_AP); WiFi.softAP("CloudClock_Setup"); delay(500);
  IPAddress apIP=WiFi.softAPIP();
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT,"*",apIP);
  server.on("/",HTTP_GET,handleRoot); server.on("/save",HTTP_GET,handleSave);
  server.on("/rescan",HTTP_GET,handleRescan);
  server.on("/generate_204",HTTP_GET,handleCaptivePortal);
  server.onNotFound([](){ handleCaptivePortal(); });
  server.begin();
  WiFi.scanNetworks(true); scanRequested=true;
  bootLogoDrawn=false;
  tft.fillScreen(C_BG);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextColor(C_ACCENT);
  int cw=tft.textWidth("Cloud");
  tft.setFont(&fonts::FreeSans12pt7b);
  int kw=tft.textWidth("Clock");
  int sx=(SCREEN_W-cw-kw)/2;
  tft.setFont(&fonts::FreeSansBold12pt7b); tft.setTextColor(C_ACCENT); tft.setCursor(sx,70); tft.print("Cloud");
  tft.setFont(&fonts::FreeSans12pt7b); tft.setTextColor(C_WHITE); tft.setCursor(sx+cw,70); tft.print("Clock");
  drawHRule(82,20,SCREEN_W-20,C_BORDER);
  printCentered("Setup Required",100,&fonts::FreeSans9pt7b,C_WHITE);
  tft.setFont(&fonts::Font0);
  tft.setTextColor(C_ACCENT); tft.setCursor(30,130); tft.print("1. Connect to WiFi:");
  tft.setTextColor(C_WHITE);  tft.setCursor(30,146); tft.print("   CloudClock_Setup");
  tft.setTextColor(C_ACCENT); tft.setCursor(30,172); tft.print("2. Open Browser:");
  tft.setTextColor(C_WHITE);  tft.setCursor(30,188); tft.print("   http://192.168.4.1");
}

void processAPLoop() {
  while (inSetupMode) {
    dnsServer.processNextRequest(); server.handleClient();
    if (scanRequested&&!scanDone){int r=WiFi.scanComplete();if(r>=0){scanResult=r;scanDone=true;scanRequested=false;}}
    if (isConnecting) {
      showBoot(spinnerAngle,"Testing Connection...");
      spinnerAngle=(spinnerAngle+16)%360; delay(25);
      if (WiFi.status()==WL_CONNECTED){delay(500);WiFi.softAPdisconnect(true);WiFi.mode(WIFI_STA);inSetupMode=false;}
      else if (millis()-connectStartTime>CONNECT_TIMEOUT) startAP();
    } else delay(10);
  }
}

// ============================================================================
//  SETUP
// ============================================================================
void initVariables() {
  DOut.cur.wifi=false; DOut.cur.cloud=false;
  DOut.prev.wifi=true; DOut.prev.cloud=true;
}

void setup() {
  Serial.begin(115200); delay(500);
  if (esp_sleep_get_wakeup_cause()==ESP_SLEEP_WAKEUP_EXT0) {
    while (digitalRead(BTN_POWER_PIN)==HIGH) delay(10); delay(50);
  }
  initVariables();
  pinMode(BTN_POWER_PIN,   INPUT_PULLDOWN);
  pinMode(BTN_DISMISS_PIN, INPUT_PULLDOWN);
  pinMode(BUZZER_PIN,      OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // TFT
  tft.init();
  tft.setRotation(2);
  tft.setBrightness(200);
  tft.fillScreen(C_BG);

  // Allocate alarm sprite — SCREEN_W × AL_VIEWPORT_H, 16-bit colour
  // This is the key to zero-flicker scrolling.
  alarmSprite.setColorDepth(16);
  alarmSprite.createSprite(SCREEN_W, AL_VIEWPORT_H);

  // Touch
  touchSPI.begin(TOUCH_SCK,TOUCH_MISO,TOUCH_MOSI,-1);
  ts.begin(touchSPI);
  ts.setRotation(0);

  // Load settings
  prefs.begin("settings",true);
  hourFormat=prefs.getInt("hour_format",24);
  String tz=prefs.getString("timezone","UTC");   // default UTC matches DB/device default
  bool pairedSaved=prefs.getBool("paired",false);
  prefs.end();
  strncpy(deviceTimezone,tz.c_str(),sizeof(deviceTimezone)-1);

  // WiFi
  prefs.begin("wifi",true);
  savedSSID=prefs.getString("ssid",""); savedPASS=prefs.getString("pass","");
  prefs.end();

  if (savedSSID=="") {
    startAP();
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID.c_str(),savedPASS.c_str());
    WiFi.setSleep(false);
    char msg[64]; snprintf(msg,sizeof(msg),"Connecting to %s...",savedSSID.c_str());
    unsigned long t0=millis();
    while (WiFi.status()!=WL_CONNECTED&&millis()-t0<15000){
      showBoot(spinnerAngle,msg); spinnerAngle=(spinnerAngle+16)%360; delay(25);
    }
    if (WiFi.status()!=WL_CONNECTED) startAP();
  }
  if (inSetupMode) processAPLoop();

  setWifiStatus(true);
  bootLogoDrawn=false;
  tft.fillScreen(C_BG);
  needFullRedraw=true;

  syncNTP();

  ws.begin(wsHost,wsPort,wsPath);
  ws.onEvent(webSocketEvent);
  ws.setReconnectInterval(5000);
  // Relaxed heartbeat: ping every 15s, pong must arrive within 5s, drop after 2 misses.
  // The tight 3/3/3 default caused spurious disconnects when SNTP or the UI task
  // briefly stalled the network stack.
  ws.enableHeartbeat(15000, 5000, 2);

  if (pairedSaved) { currentMode=MODE_NORMAL; }
  else             { currentMode=MODE_PAIRING; triggerCodeFetch=true; }  // handled in loop(), not here

  xTaskCreatePinnedToCore(uiTaskCode,"UI Task",10000,NULL,1,NULL,0);
}

// ============================================================================
//  LOOP  (Core 1)
// ============================================================================
void loop() {
  ws.loop();
  handleButtons();
  setWifiStatus(WiFi.status()==WL_CONNECTED);
  // Cloud status is set authoritatively by webSocketEvent (WStype_CONNECTED /
  // WStype_DISCONNECTED).  We do NOT call setCloudStatus(ws.isConnected()) here
  // because ws.isConnected() queries the raw TCP socket, which can remain true
  // for a moment after the WS session has been torn down, making the icon
  // flicker back to "connected" when it shouldn't.
  // Safety net: if the TCP socket is gone but we still think we're connected, fix it.
  if (DOut.cur.cloud && !ws.isConnected()) {
    setCloudStatus(false);
  }

  if (triggerCodeFetch)    { triggerCodeFetch=false;    fetchPairingCode(); }
  if (triggerSettingsSave) {
    triggerSettingsSave=false;
    prefs.begin("settings",false);
    prefs.putInt("hour_format",hourFormat);
    prefs.putString("timezone",deviceTimezone);
    prefs.end();
    Serial.printf("[APP] Settings saved. TZ=%s fmt=%d\n", deviceTimezone, hourFormat);
  }
  // Apply timezone (calls configTime + SNTP re-init) ONLY when TZ string changed.
  // Doing this on every reconnect/sync_settings would repeatedly restart SNTP,
  // stalling the network stack and causing the WS heartbeat to miss pongs → disconnect loop.
  if (triggerTZApply) {
    triggerTZApply = false;
    applyTimezone();
    Serial.printf("[APP] Timezone applied: %s (offset %ld)\n",
                  deviceTimezone, getOffsetForTimezone(deviceTimezone));
  }

  unsigned long now=millis();
  if (now-lastHeartbeat  >5000)  { ws.sendTXT("{\"type\":\"heartbeat\"}"); lastHeartbeat=now; }
  if (now-lastAlarmCheck >20000) { checkAlarms(); lastAlarmCheck=now; }
  if (buzzerActive&&(now-buzzerStart>ALARM_RING_DURATION)) buzzerOff();
  if (currentMode==MODE_PAIRING&&pairingCodeExpiry>0&&millis()>pairingCodeExpiry-60000)
    fetchPairingCode();
}
