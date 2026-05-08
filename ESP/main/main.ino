#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <HTTPClient.h>
#include <time.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// --- Modern Built-in Fonts ---
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 240

#define TFT_CLK   12
#define TFT_MOSI  11
#define TFT_DC    9
#define TFT_RST   3
#define ST77XX_LIGHTGREY 0xC618
#define TFT_CS    -1

// --- High-Res 24x24 Icons ---
const unsigned char wifiIcon_24x24[] PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x7f, 0xc0, 0x03, 0xff, 0xf0, 0x0f, 0xc0, 0xf8, 0x1f, 0x00, 0x3c, 0x3c, 
  0x00, 0x0e, 0x78, 0x3c, 0x0f, 0x60, 0xff, 0x03, 0x03, 0xc3, 0x80, 0x07, 0x00, 0xc0, 0x0e, 0x00, 
  0xe0, 0x0c, 0x18, 0x70, 0x00, 0x3c, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x66, 0x00, 0x00, 0xc3, 0x00, 
  0x00, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x3c, 0x00, 0x00, 
  0x18, 0x00, 0x00, 0x00, 0x00
};

const unsigned char wifiOffIcon_24x24[] PROGMEM = {
  0x00, 0x00, 0x06, 0x00, 0x7f, 0xce, 0x03, 0xff, 0xf8, 0x0f, 0xc1, 0xf0, 0x1f, 0x03, 0xbc, 0x3c, 
  0x07, 0x0e, 0x78, 0x3e, 0x07, 0x60, 0xfc, 0x03, 0x03, 0xd8, 0x80, 0x07, 0xb0, 0xc0, 0x0f, 0x60, 
  0xe0, 0x0e, 0x18, 0x70, 0x1c, 0x3c, 0x00, 0x38, 0x7e, 0x00, 0x70, 0x66, 0x00, 0xe0, 0xc3, 0x01, 
  0xc0, 0x81, 0x03, 0x80, 0x00, 0x07, 0x00, 0x18, 0x0e, 0x00, 0x3c, 0x1c, 0x00, 0x3c, 0x38, 0x00, 
  0x18, 0x70, 0x00, 0x00, 0xe0
};

const unsigned char cloudWarnIcon_24x24[] PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3e, 0x00, 0x00, 0x7f, 0x00, 0x00, 0xe3, 0x80, 0x01, 
  0xc1, 0xc0, 0x0f, 0x81, 0xe0, 0x3f, 0x00, 0xf0, 0x7e, 0x18, 0x78, 0xf8, 0x18, 0x3c, 0xf0, 0x18, 
  0x1e, 0xf0, 0x18, 0x0f, 0xf0, 0x18, 0x07, 0xf0, 0x18, 0x07, 0xf0, 0x00, 0x07, 0xf8, 0x18, 0x0f, 
  0x7c, 0x18, 0x1e, 0x3f, 0x00, 0xfc, 0x0f, 0xff, 0xf0, 0x03, 0xff, 0xc0, 0x00, 0x00, 0x00, 0x00, 
  0x00, 0x00, 0x00, 0x00, 0x00
};

Adafruit_ST7789 display = Adafruit_ST7789(&SPI, TFT_CS, TFT_DC, TFT_RST);

const char* ssid = "Excitel_2.4G_50784975";
const char* password = "12345678";

// UPDATE THIS TO YOUR NEW IP ADDRESS!
const char* host = "192.168.1.35"; 
const int port = 8080;
const char* path = "/ws?token=eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJkZXZpY2VfaWQiOjF9.W_fyTwdf3mmXql27QnJD5voFm_MGqQKxlr4PkVRmftU";

const int deviceId = 1;
const int buzzerPin = 6;

const unsigned long ALARM_RING_DURATION = 30000;
bool isReady = false;
int spinnerAngle = 0;
WebSocketsClient ws;

#define MAX_ALARMS 20

struct Alarm {
  int id;
  int hour;
  int minute;
  bool enabled;
  bool recurring;
  char recurType[8];
  bool recurDays[7];
};

struct Connections {
  bool cloud;
  bool wifi;
  bool alarmsUpcoming;
};

struct DisplayOutput {
  Connections prevConnections;
  Connections connections;
};

DisplayOutput DOut;
Alarm alarms[MAX_ALARMS];
int alarmCount = 0;
volatile bool updatingAlarms = false; // Thread-safety flag

unsigned long lastHeartbeat = 0;
unsigned long lastAlarmCheck = 0;

bool buzzerActive = false;
unsigned long buzzerStart = 0;
int lastFiredAlarmId = -1;
int lastFiredAlarmMinute = -1;

// --- State Tracking for Flicker-Free UI ---
int currentDisplayedMin = -1;
int currentDisplayedAlarmIdx = -2; 
bool currentDisplayedBuzzer = false;

// --- Footer Animation Tracking ---
bool lastFooterActive = false;
unsigned long lastFooterAnimTime = 0;
int footerAnimAngle = 0;
char lastBootMsg[64] = "";

// --- Helper Functions ---
void initDisplay() {
  display.init(240, 240, SPI_MODE3);
  display.setRotation(2);
  display.fillScreen(ST77XX_BLACK);
  Serial.println("[TFT] Display initialized.");
}

void printModernText(const char* text, int y, const GFXfont* font, uint16_t color) {
  display.setFont(font);
  display.setTextColor(color);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, y);
  display.print(text);
}

void showBoot(int angle, const char* statusMsg) {
  display.fillRect(100, 130, 40, 40, ST77XX_BLACK);
  printModernText("CloudClock", 100, &FreeSans12pt7b, ST77XX_CYAN);
  
  for (int i = 0; i < 90; i += 6) { 
    float rad = (angle + i) * 0.0174533;
    int x = 120 + cos(rad) * 16;
    int y = 150 + sin(rad) * 16;
    display.drawPixel(x, y, ST77XX_WHITE);
  }

  if (strcmp(lastBootMsg, statusMsg) != 0) {
    display.fillRect(0, 180, 240, 30, ST77XX_BLACK); 
    display.setFont(NULL); 
    display.setTextSize(1);
    display.setTextColor(ST77XX_LIGHTGREY);
    
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(statusMsg, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 190);
    display.print(statusMsg);
    
    strcpy(lastBootMsg, statusMsg);
  }
}

void syncNTP() {
  Serial.print("[NTP] Syncing time with NTP server...");
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov"); 
  struct tm t;
  
  unsigned long startWait = millis();
  while (millis() - startWait < 20000) { 
    showBoot(spinnerAngle, "Syncing NTP Time...");
    spinnerAngle += 16;
    if (spinnerAngle >= 360) spinnerAngle = 0;

    if (getLocalTime(&t, 10)) {
      char buf[32];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
      Serial.printf(" OK -> %s\n", buf);
      return;
    }
    delay(25); 
  }
  Serial.println(" FAILED - Alarms will not fire accurately!");
}

void setWifiStatus(bool newStatus) {
  DOut.connections.wifi = newStatus;
}
void setCloudStatus(bool newStatus) {
  DOut.connections.cloud = newStatus;
}

void sendBuzzerStatus(bool state) {
  StaticJsonDocument<128> doc;
  doc["type"] = "buzzer_status";
  doc["value"] = state;
  String json;
  serializeJson(doc, json);
  ws.sendTXT(json);
  Serial.printf("[WS] Sent buzzer status: %s\n", state ? "ON" : "OFF");
}

void buzzerOn() {
  digitalWrite(buzzerPin, HIGH);
  buzzerActive = true;
  buzzerStart = millis();
  sendBuzzerStatus(true);
  Serial.println("[BUZZER] Turned ON");
}

void buzzerOff() {
  digitalWrite(buzzerPin, LOW);
  buzzerActive = false;
  sendBuzzerStatus(false);
  Serial.println("[BUZZER] Turned OFF");
}

void fetchAlarms() {
  HTTPClient http;
  String url = String("http://") + host + ":" + String(port) + "/api/alarms/" + String(deviceId);
  
  http.begin(url);
  int code = http.GET();

  if (code != 200) {
    Serial.printf("[HTTP] Alarm fetch failed with code: %d\n", code);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) return;

  JsonArray arr = doc.as<JsonArray>();

  // Thread safety lock so UI core doesn't read while writing
  updatingAlarms = true; 
  alarmCount = 0;

  for (JsonObject obj : arr) {
    if (alarmCount >= MAX_ALARMS) break;

    Alarm& a = alarms[alarmCount];
    a.id = obj["id"] | 0;
    a.enabled = obj["enabled"] | true;

    const char* t = obj["time"] | "";
    if (sscanf(t, "%d:%d", &a.hour, &a.minute) != 2) continue;

    a.recurring = obj["recurring"] | false;
    const char* rt = obj["recur_type"] | "";
    strncpy(a.recurType, rt, sizeof(a.recurType) - 1);
    a.recurType[sizeof(a.recurType) - 1] = '\0';

    memset(a.recurDays, 0, sizeof(a.recurDays));
    if (obj.containsKey("recur_days") && !obj["recur_days"].isNull()) {
      JsonArray days = obj["recur_days"].as<JsonArray>();
      for (int d : days) {
        if (d >= 0 && d <= 6) a.recurDays[d] = true;
      }
    }
    alarmCount++;
  }
  
  updatingAlarms = false; // Unlock
  currentDisplayedAlarmIdx = -2; // Force UI update
  Serial.printf("[ALARM] Fetch complete. Total alarms: %d\n", alarmCount);
}

void checkAlarms() {
  struct tm t;
  if (!getLocalTime(&t)) return;

  int minuteNow = t.tm_hour * 60 + t.tm_min;

  for (int i = 0; i < alarmCount; i++) {
    Alarm& a = alarms[i];
    
    if (!a.enabled || a.hour != t.tm_hour || a.minute != t.tm_min) continue;
    if (a.id == lastFiredAlarmId && minuteNow == lastFiredAlarmMinute) continue;

    bool shouldFire = false;
    if (!a.recurring || strcmp(a.recurType, "daily") == 0) {
      shouldFire = true;
    } else if (strcmp(a.recurType, "weekly") == 0) {
      shouldFire = a.recurDays[t.tm_wday]; 
    }

    if (shouldFire) {
      Serial.printf("[ALARM] >>> TRIGGERING ALARM ID=%d TIME=%02d:%02d <<<\n", a.id, a.hour, a.minute);
      lastFiredAlarmId = a.id;
      lastFiredAlarmMinute = minuteNow;
      buzzerOn();
      break;
    }
  }
}

int getNextUpcomingAlarmIndex() {
  if (updatingAlarms) return -1; // Protect against thread crashing during fetch

  struct tm t;
  if (!getLocalTime(&t)) return -1;
  
  int nowMins = t.tm_hour * 60 + t.tm_min;
  int minDiff = 999999; 
  int nextIdx = -1;

  for (int i = 0; i < alarmCount; i++) {
    Alarm& a = alarms[i];
    if (!a.enabled) continue;
    
    int alarmMins = a.hour * 60 + a.minute;
    
    if (!a.recurring || strcmp(a.recurType, "daily") == 0) {
        int diff = alarmMins - nowMins;
        if (diff <= 0) diff += 24 * 60; 
        
        if (diff < minDiff) {
            minDiff = diff;
            nextIdx = i;
        }
    } 
    else if (strcmp(a.recurType, "weekly") == 0) {
        for (int dayOffset = 0; dayOffset < 7; dayOffset++) {
            int checkDay = (t.tm_wday + dayOffset) % 7;
            if (a.recurDays[checkDay]) {
                int diff = alarmMins - nowMins + (dayOffset * 24 * 60);
                if (diff > 0) {
                    if (diff < minDiff) {
                        minDiff = diff;
                        nextIdx = i;
                    }
                    break;
                } else if (dayOffset == 0 && diff <= 0) {
                    int nextWeekDiff = diff + (7 * 24 * 60);
                    if (nextWeekDiff < minDiff) {
                        minDiff = nextWeekDiff;
                        nextIdx = i;
                    }
                }
            }
        }
    }
  }
  return nextIdx;
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[WS] WebSocket Connected!");
      setCloudStatus(true);
      break;
    case WStype_DISCONNECTED:
      Serial.println("[WS] WebSocket Disconnected.");
      setCloudStatus(false);
      break;
    case WStype_TEXT:
      {
        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, payload)) return;

        const char* msgType = doc["type"] | "";
        if (strcmp(msgType, "buzzer") == 0) {
          bool state = doc["value"] | false;
          if (state) buzzerOn();
          else buzzerOff();
        } else if (strcmp(msgType, "alarm_fire") == 0) {
          buzzerOn();
        } else if (strcmp(msgType, "sync_alarms") == 0) {
          fetchAlarms();
        }
        break;
      }
    default: break;
  }
}

// -----------------------------------------------------------------
// --- DISPLAY TASK: Runs on Core 0 (Protects UI from freezing!) ---
// -----------------------------------------------------------------
void updateFooter() {
  bool needsFooter = !DOut.connections.cloud || !DOut.connections.wifi;

  if (needsFooter) {
    if (millis() - lastFooterAnimTime > 25) { 
      lastFooterAnimTime = millis();
      footerAnimAngle += 16;
      if (footerAnimAngle >= 360) footerAnimAngle = 0;

      if (!lastFooterActive) {
        display.fillRect(0, 220, 240, 20, ST77XX_BLACK); 
      }

      display.fillRect(10, 220, 20, 20, ST77XX_BLACK);

      uint16_t color = !DOut.connections.wifi ? ST77XX_RED : ST77XX_ORANGE;
      
      for (int i = 0; i < 90; i += 15) { 
        float rad = (footerAnimAngle + i) * 0.0174533;
        int x = 20 + cos(rad) * 6;
        int y = 230 + sin(rad) * 6;
        display.drawPixel(x, y, color);
        display.drawPixel(x+1, y, color);
        display.drawPixel(x, y+1, color);
        display.drawPixel(x+1, y+1, color);
      }

      display.setFont(NULL); 
      display.setTextSize(1);
      display.setTextColor(ST77XX_LIGHTGREY, ST77XX_BLACK);
      display.setCursor(35, 226);
      
      if (!DOut.connections.wifi) {
        display.print("Connecting to WiFi...   ");
      } else {
        display.print("Connecting to Cloud...  ");
      }
      
      lastFooterActive = true;
    }
  } else if (lastFooterActive) {
    display.fillRect(0, 220, 240, 20, ST77XX_BLACK);
    lastFooterActive = false;
  }
}

void updateUI() {
  if (DOut.connections.wifi != DOut.prevConnections.wifi || DOut.connections.cloud != DOut.prevConnections.cloud) {
    display.fillRect(170, 5, 70, 30, ST77XX_BLACK); 
    if (DOut.connections.wifi) display.drawBitmap(210, 10, wifiIcon_24x24, 24, 24, ST77XX_GREEN);
    else display.drawBitmap(210, 10, wifiOffIcon_24x24, 24, 24, ST77XX_RED);
    if (!DOut.connections.cloud && DOut.connections.wifi) display.drawBitmap(180, 10, cloudWarnIcon_24x24, 24, 24, ST77XX_ORANGE);
    DOut.prevConnections = DOut.connections;
  }

  struct tm t;
  if (getLocalTime(&t, 10)) { 
    if (t.tm_min != currentDisplayedMin || currentDisplayedMin == -1) {
      display.fillRect(0, 60, 240, 60, ST77XX_BLACK); 
      char timeStr[16];
      sprintf(timeStr, "%02d:%02d", t.tm_hour, t.tm_min);
      printModernText(timeStr, 110, &FreeSansBold24pt7b, ST77XX_WHITE);
      currentDisplayedMin = t.tm_min;
      currentDisplayedAlarmIdx = -2; 
    }
  }

  if (buzzerActive != currentDisplayedBuzzer) {
    display.fillRect(0, 130, 240, 80, ST77XX_BLACK); 
    if (buzzerActive) printModernText("BUZZER ON", 180, &FreeSans12pt7b, ST77XX_RED);
    else currentDisplayedAlarmIdx = -2; 
    currentDisplayedBuzzer = buzzerActive;
  }

  if (!buzzerActive) {
    int nextAlarmIdx = getNextUpcomingAlarmIndex();
    if (nextAlarmIdx != currentDisplayedAlarmIdx) {
      display.fillRect(0, 130, 240, 80, ST77XX_BLACK); 
      if (nextAlarmIdx != -1) {
        printModernText("Next Alarm", 160, &FreeSans9pt7b, ST77XX_CYAN);
        char aTime[16];
        sprintf(aTime, "%02d:%02d", alarms[nextAlarmIdx].hour, alarms[nextAlarmIdx].minute);
        printModernText(aTime, 200, &FreeSans12pt7b, ST77XX_YELLOW);
      } else {
        printModernText("No Alarms", 180, &FreeSans12pt7b, 0x7BEF); 
      }
      currentDisplayedAlarmIdx = nextAlarmIdx;
    }
  }
}

// Dedicated FreeRTOS thread for Display only
void uiTaskCode(void * parameter) {
  for(;;) {
    updateUI();
    updateFooter();
    delay(10); // Small yield to prevent watchdog resets
  }
}

void initVariables() {
  DOut.connections.alarmsUpcoming = false;
  DOut.connections.wifi = false;
  DOut.connections.cloud = false;
  DOut.prevConnections = DOut.connections;
}

// -----------------------------------------------------------------
// --- SETUP & MAIN LOOP: Runs on Core 1 (Networking)            ---
// -----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000); 
  
  initVariables();
  SPI.begin(TFT_CLK, -1, TFT_MOSI, TFT_CS);
  initDisplay();

  Serial.println("[WIFI] Connecting to WiFi...");
  WiFi.begin(ssid, password);
  char bootMsg[64];
  sprintf(bootMsg, "Connecting to %s...", ssid);
  
  while (!isReady) {
    showBoot(spinnerAngle, bootMsg);
    spinnerAngle += 16;
    if (spinnerAngle >= 360) spinnerAngle = 0;
    delay(25);

    if (WiFi.status() == WL_CONNECTED) {
      isReady = true;
      setWifiStatus(true);
      display.fillScreen(ST77XX_BLACK);
      Serial.print("[WIFI] Connected! IP: ");
      Serial.println(WiFi.localIP());
    }
  }

  pinMode(buzzerPin, OUTPUT);
  digitalWrite(buzzerPin, LOW);

  syncNTP();

  showBoot(spinnerAngle, "Fetching Alarms...");
  fetchAlarms();
  display.fillScreen(ST77XX_BLACK); 

  ws.begin(host, port, path);
  ws.onEvent(webSocketEvent);
  ws.setReconnectInterval(5000);

  // LAUNCH UI TASK ON CORE 0
  xTaskCreatePinnedToCore(
    uiTaskCode, /* Task function. */
    "UI Task",  /* name of task. */
    10000,      /* Stack size of task */
    NULL,       /* parameter of the task */
    1,          /* priority of the task */
    NULL,       /* Task handle to keep track of created task */
    0);         /* pin task to core 0 */
}

void loop() {
  ws.loop();

  setWifiStatus(WiFi.status() == WL_CONNECTED);

  unsigned long now = millis();

  if (now - lastHeartbeat > 10000) {
    ws.sendTXT("{\"type\":\"heartbeat\"}");
    lastHeartbeat = now;
  }

  if (now - lastAlarmCheck > 20000) {
    checkAlarms();
    lastAlarmCheck = now;
  }

  if (buzzerActive && (now - buzzerStart > ALARM_RING_DURATION)) {
    Serial.println("[BUZZER] Auto-stopping after duration limit.");
    buzzerOff();
  }
}