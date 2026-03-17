/*
 *  BTC Price Monitor — ESP32 CYD (ESP32-2432S028)
 *  https://github.com/JulianPedro/btc-price-monitor-cyd
 *
 *  Required libraries (Arduino Library Manager):
 *    - TFT_eSPI            (Bodmer)
 *    - XPT2046_Touchscreen (Paul Stoffregen)
 *    - ArduinoJson         >= 7.x  (Benoit Blanchon)
 *    - WiFiManager         (tzapu)
 *
 *  Configuration: edit config.h before compiling.
 *
 *  Gestures:
 *    Single tap        → next screen  (Dashboard / Details / Clock)
 *    Double tap        → toggle display on / off
 *    Hold 3 s at boot  → open settings portal (Wi-Fi / timezone / orientation / currency)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <FS.h>
using fs::FS;              // ESP32 core 3.x: WebServer.h uses FS without namespace
#include <WiFiManager.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>

#include "config.h"

// ═════════════════════════════════════════════════════════════════════════════
//  HARDWARE — CYD (ESP32-2432S028R) pins
// ═════════════════════════════════════════════════════════════════════════════
#define PIN_BL        21
#define PIN_LDR       34
#define PIN_LED_R      4
#define PIN_LED_G     16
#define PIN_LED_B     17
#define PIN_BOOT       0   // BOOT button — active LOW, internal pull-up
#define TOUCH_CS      33
#define TOUCH_IRQ     36
#define TOUCH_SCK     25
#define TOUCH_MISO    39
#define TOUCH_MOSI    32

// ═════════════════════════════════════════════════════════════════════════════
//  PORTRAIT layout — Y positions (display 240 × 320, 3 px border)
// ═════════════════════════════════════════════════════════════════════════════
#define L_BORDER       3
#define L_CLOCK_Y      6
#define L_DATE_Y      33
#define L_SEP1_Y      44
#define L_LBL_BTC_Y   48
#define L_PRICE_Y     58
#define L_CHANGE_Y    86
#define L_SEP2_Y     107
#define L_LBL_LOC_Y  111
#define L_LOCAL_Y    121
#define L_SPARK_Y    142
#define L_SPARK_H    118   // sparkline ends at y = 260
#define L_SEP3_Y     263
#define L_STATUS_Y   267
#define L_DOTS_Y     300

// ═════════════════════════════════════════════════════════════════════════════
//  LANDSCAPE layout — positions (display 320 × 240, 3 px border)
//  2-column: prices on the left | sparkline on the right
// ═════════════════════════════════════════════════════════════════════════════
#define LL_BORDER      3
#define LL_SEP_H       44
#define LL_DIV_X      169
#define LL_LCOL_X       3
#define LL_LCOL_W     164
#define LL_LCOL_CX     85
#define LL_RCOL_X     172
#define LL_RCOL_W     145
#define LL_LBL_BTC_Y   48
#define LL_PRICE_Y     58
#define LL_CHANGE_Y    77
#define LL_SEP2_Y      97
#define LL_LBL_LOC_Y  101
#define LL_LOCAL_Y    111
#define LL_FOOT_SEP_Y 200
#define LL_STATUS_Y   205
#define LL_DOTS_Y     228

// ═════════════════════════════════════════════════════════════════════════════
//  APIs
// ═════════════════════════════════════════════════════════════════════════════
static const char* BTC_PRIMARY  = "https://economia.awesomeapi.com.br/json/last/BTC-USD";
static const char* BTC_FALLBACK = "https://api.coinbase.com/v2/prices/BTC-USD/spot";
// Local currency URLs are built dynamically from g_currency

// ═════════════════════════════════════════════════════════════════════════════
//  GLOBALS
// ═════════════════════════════════════════════════════════════════════════════
TFT_eSPI            tft;
SPIClass            touchSPI(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
Preferences         prefs;

uint16_t C_ORANGE, C_GOLDEN, C_LABEL, C_SPARK_LINE, C_SPARK_FILL, C_DIM;

// ═════════════════════════════════════════════════════════════════════════════
//  PERSISTENT SETTINGS  (Preferences, namespace "btcmon")
// ═════════════════════════════════════════════════════════════════════════════
int8_t  g_tz_hours = -4;
uint8_t g_orient   = 0;        // 0 = portrait  1 = landscape
String  g_currency = "USD";    // 3-letter ISO code

void loadPrefs() {
  prefs.begin("btcmon", true);
  g_tz_hours = (int8_t) prefs.getInt("tz",     (int)(TIMEZONE_OFFSET_SEC / 3600L));
  g_orient   = (uint8_t)prefs.getUInt("orient", 0);
  g_currency  = prefs.getString("cur", DEFAULT_CURRENCY);
  prefs.end();
  g_currency.toUpperCase();
  if (g_currency.length() != 3) g_currency = DEFAULT_CURRENCY;
}

void savePrefs(int8_t tz, uint8_t orient, const String& cur) {
  prefs.begin("btcmon", false);
  prefs.putInt("tz",     (int)tz);
  prefs.putUInt("orient",(unsigned int)orient);
  prefs.putString("cur", cur);
  prefs.end();
}

// ═════════════════════════════════════════════════════════════════════════════
//  TOUCH
// ═════════════════════════════════════════════════════════════════════════════
static const int     TS_MINX  = 250,  TS_MAXX = 3850;
static const int     TS_MINY  = 250,  TS_MAXY = 3850;
static const uint32_t DTAP_MS = 350;
static const int     TAP_SLOP = 30;

enum  TapState { TAP_IDLE, TAP_PENDING };
TapState tapState     = TAP_IDLE;
uint32_t pendingTapAt = 0;
int16_t  pendingTapX  = -1, pendingTapY = -1;

// ═════════════════════════════════════════════════════════════════════════════
//  STATE
// ═════════════════════════════════════════════════════════════════════════════
bool    screenOn  = true;
uint8_t curScreen = 0;

float   btcUsd   = 0, btcUsdPrev   = 0;
float   btcLocal = 0, btcLocalPrev = 0;

unsigned long lastPriceUpdate      = 0;
unsigned long lastSuccessfulUpdate = 0;
time_t        lastUpdateTime       = 0;   // wall-clock time of last successful fetch

float   priceHist[SPARKLINE_POINTS];
uint8_t histHead  = 0;
uint8_t histCount = 0;

float    ldrEma      = -1.0f;
uint32_t lastLdrRead = 0;
uint32_t lastClockDraw = 0;

// ─── LED ────────────────────────────────────────────────────────────────────
// LED is OFF by default. Effects are triggered only on price change:
//   BREATH_UP   — slow green breathing for ~10 s  (price went up)
//   BREATH_DOWN — slow red breathing for ~10 s    (price went down)
// Each breath cycle lasts 3 s (fade in + fade out via sine wave).
enum LedMode { LED_OFF, LED_BREATH_UP, LED_BREATH_DOWN };
LedMode  ledMode        = LED_OFF;
uint32_t ledEffectStart = 0;

// ═════════════════════════════════════════════════════════════════════════════
//  PROTOTYPES
// ═════════════════════════════════════════════════════════════════════════════
void setupWifi();
void setupTime();
void backlightSet(uint8_t v);
void backlightOff();
void backlightOn();
void toggleScreen();
void touchInit();
void touchUpdate();
bool touchGetXY(int16_t &x, int16_t &y);
void buttonInit();
void buttonUpdate();
void openPortal();
void ldrInit();
void ldrUpdate();
void ledInit();
void ledWriteRGB(uint8_t r, uint8_t g, uint8_t b);
void ledOff();
void ledUpdate();
void ledPriceSignal(bool went_up);
void histPush(float v);
bool fetchBTCUSD(float &out);
bool fetchBTCLocal(float &out);
void redrawScreen();
void drawScreen0_portrait();
void drawScreen0_landscape();
void drawScreen1();
void drawScreen2();
void updateClock();
void drawBorder();
void drawPageDots();
void drawSparkline(int x, int y, int w, int h);
void drawChangeIndicator(int x, int y, int w, float cur, float prev);
void drawStatusBar(int x, int y, int w);
void drawWifiDot(int x, int y, bool ok);
String fmtUSD(float v);
String fmtLocal(float v);
String nowHHMMSS();
String nowDate();

// ═════════════════════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  loadPrefs();
  ledInit();

  tft.init();
  tft.setRotation(g_orient == 0 ? 2 : 1);

  C_ORANGE     = tft.color565(247, 147,  26);
  C_GOLDEN     = tft.color565(255, 200,  50);
  C_LABEL      = tft.color565(120, 120, 120);
  C_SPARK_LINE = tft.color565(100, 210, 255);
  C_SPARK_FILL = tft.color565( 10,  40,  60);
  C_DIM        = tft.color565( 60,  60,  60);

  pinMode(PIN_BL, OUTPUT);
  backlightOn();
  tft.fillScreen(TFT_BLACK);
  drawBorder();

  touchInit();
  buttonInit();
  ldrInit();

  setupWifi();

  if (WiFi.status() == WL_CONNECTED)
    setupTime();

  if (fetchBTCUSD(btcUsd))  histPush(btcUsd);
  fetchBTCLocal(btcLocal);
  if (btcUsd > 0) { lastSuccessfulUpdate = millis(); time(&lastUpdateTime); }

  lastPriceUpdate = millis();
  lastClockDraw   = millis();
  redrawScreen();
}

// ═════════════════════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════════════════════
void loop() {
  ledUpdate();
  touchUpdate();
  buttonUpdate();
  ldrUpdate();

  if (millis() - lastPriceUpdate >= PRICE_REFRESH_MS) {
    lastPriceUpdate = millis();

    if (WiFi.status() == WL_CONNECTED) {
      float nb = btcUsd, nl = btcLocal;
      bool okB = fetchBTCUSD(nb);
      bool okL = fetchBTCLocal(nl);

      if (okB) {
        bool up = nb > btcUsd + 0.01f;
        bool dn = nb < btcUsd - 0.01f;
        if (up || dn) ledPriceSignal(up);
        btcUsdPrev = btcUsd; btcUsd = nb;
        histPush(btcUsd);
        lastSuccessfulUpdate = millis(); time(&lastUpdateTime);
      }
      if (okL) { btcLocalPrev = btcLocal; btcLocal = nl; }

      if (screenOn) redrawScreen();
    }
  }

  if (millis() - lastClockDraw >= 1000) {
    lastClockDraw = millis();
    if (screenOn) updateClock();
  }

  delay(10);
}

// ═════════════════════════════════════════════════════════════════════════════
//  WI-FI  (WiFiManager with tz, orientation and currency parameters)
// ═════════════════════════════════════════════════════════════════════════════
static void onPortalStart(WiFiManager*) {
  tft.fillRect(L_BORDER+1, L_BORDER+1,
               tft.width()-2*(L_BORDER+1), 100, TFT_BLACK);
  int cx = tft.width() / 2;
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK); tft.setTextSize(1);
  tft.drawString("Settings portal open", cx, 10);
  tft.setTextColor(C_LABEL, TFT_BLACK);
  tft.drawString("Connect to AP:", cx, 22);
  tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(2);
  tft.drawString(WIFI_AP_NAME, cx, 34);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK); tft.setTextSize(1);
  tft.drawString("then open 192.168.4.1", cx, 58);
  tft.setTextDatum(TL_DATUM);
}

void setupWifi() {
  int cx = tft.width() / 2;
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK); tft.setTextSize(1);
  tft.drawString("Connecting to Wi-Fi...", cx, L_BORDER + 6);
  tft.setTextColor(C_DIM, TFT_BLACK);
  tft.drawString("Hold 3s to open settings", cx, L_BORDER + 18);
  tft.setTextDatum(TL_DATUM);

  // Detect 3-second hold → force portal
  bool     forcePortal = false;
  uint32_t holdStart   = 0;
  uint32_t checkEnd    = millis() + 4000;
  while (millis() < checkEnd && !forcePortal) {
    int16_t tx, ty;
    if (touchGetXY(tx, ty)) {
      if (holdStart == 0) holdStart = millis();
      if (millis() - holdStart >= 3000) forcePortal = true;
    } else {
      holdStart = 0;
    }
    delay(20);
  }

  // Build parameter buffers from current values
  char tz_buf[5], orient_buf[3], cur_buf[5];
  snprintf(tz_buf,     sizeof(tz_buf),     "%d", (int)g_tz_hours);
  snprintf(orient_buf, sizeof(orient_buf), "%d", (int)g_orient);
  g_currency.toCharArray(cur_buf, sizeof(cur_buf));

  WiFiManagerParameter p_tz("tz",
    "Timezone offset in hours (e.g. -3 for Brasilia, 0 for London, 8 for HKG)",
    tz_buf, 4);
  WiFiManagerParameter p_orient("orient",
    "Orientation: 0 = portrait (upright)   1 = landscape (flat)",
    orient_buf, 2);
  WiFiManagerParameter p_cur("cur",
    "Comparison currency code: BRL EUR GBP JPY ARS CLP MXN CAD AUD",
    cur_buf, 4);

  WiFiManager wm;
  wm.addParameter(&p_tz);
  wm.addParameter(&p_orient);
  wm.addParameter(&p_cur);
  wm.setConnectTimeout(20);
  wm.setAPCallback(onPortalStart);
  if (WIFI_PORTAL_TIMEOUT_SEC > 0)
    wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_SEC);

  const char* pass = (strlen(WIFI_AP_PASS) > 0) ? WIFI_AP_PASS : nullptr;

  if (forcePortal) {
    tft.fillRect(L_BORDER+1, L_BORDER+1,
                 tft.width()-2*(L_BORDER+1), 30, TFT_BLACK);
    wm.startConfigPortal(WIFI_AP_NAME, pass);
  } else {
    wm.autoConnect(WIFI_AP_NAME, pass);
  }

  // Apply and persist if anything changed
  int8_t  newTz     = (int8_t) constrain(atoi(p_tz.getValue()), -12, 14);
  uint8_t newOrient = (uint8_t)constrain(atoi(p_orient.getValue()), 0, 1);
  String  newCur    = String(p_cur.getValue());
  newCur.toUpperCase(); newCur.trim();
  if (newCur.length() != 3) newCur = g_currency;

  if (newTz != g_tz_hours || newOrient != g_orient || newCur != g_currency) {
    savePrefs(newTz, newOrient, newCur);
    ESP.restart();
  }

  tft.fillRect(L_BORDER+1, L_BORDER+1,
               tft.width()-2*(L_BORDER+1), 100, TFT_BLACK);

  if (WiFi.status() != WL_CONNECTED) {
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_RED, TFT_BLACK); tft.setTextSize(1);
    tft.drawString("Wi-Fi not connected", tft.width()/2, 10);
    tft.drawString("Offline mode", tft.width()/2, 22);
    tft.setTextDatum(TL_DATUM);
    delay(2000);
    tft.fillRect(L_BORDER+1, L_BORDER+1,
                 tft.width()-2*(L_BORDER+1), 40, TFT_BLACK);
  }
}

// ═════════════════════════════════════════════════════════════════════════════
//  TIME (SNTP)
// ═════════════════════════════════════════════════════════════════════════════
void setupTime() {
  configTime((long)g_tz_hours * 3600L, DST_OFFSET_SEC,
             "pool.ntp.org", "time.nist.gov");
  struct tm t;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&t, 500)) break;
    delay(50);
  }
}

String nowHHMMSS() {
  struct tm t;
  if (!getLocalTime(&t)) return "--:--:--";
  char buf[9]; strftime(buf, sizeof(buf), "%H:%M:%S", &t);
  return String(buf);
}

String nowDate() {
  struct tm t;
  if (!getLocalTime(&t)) return "";
  char buf[32]; strftime(buf, sizeof(buf), "%a, %d %b %Y", &t);
  return String(buf);
}

// ═════════════════════════════════════════════════════════════════════════════
//  BACKLIGHT
// ═════════════════════════════════════════════════════════════════════════════
void backlightSet(uint8_t v) { analogWrite(PIN_BL, v); }
void backlightOff()          { backlightSet(0); screenOn = false; }
void backlightOn()           { screenOn = true; backlightSet(180); }

void toggleScreen() {
  if (screenOn) {
    backlightOff();
    tft.fillScreen(TFT_BLACK);
  } else {
    backlightOn();
    tft.fillScreen(TFT_BLACK);
    redrawScreen();
  }
}

// ═════════════════════════════════════════════════════════════════════════════
//  TOUCH
// ═════════════════════════════════════════════════════════════════════════════
void touchInit() {
  touchSPI.begin(TOUCH_SCK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);
}

bool touchGetXY(int16_t &x, int16_t &y) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  if (p.z < 200) return false;
  x = constrain(map(p.x, TS_MINX, TS_MAXX, 0, tft.width()-1),  0, tft.width()-1);
  y = constrain(map(p.y, TS_MINY, TS_MAXY, 0, tft.height()-1), 0, tft.height()-1);
  return true;
}

void touchUpdate() {
  if (tapState == TAP_PENDING && (millis() - pendingTapAt) > DTAP_MS) {
    tapState = TAP_IDLE;
    if (screenOn) {
      curScreen = (curScreen + 1) % 3;
      tft.fillScreen(TFT_BLACK);
      redrawScreen();
    }
    return;
  }

  int16_t x, y;
  if (!touchGetXY(x, y)) return;

  static bool wasDown = false;
  if (wasDown) return;
  wasDown = true;

  uint32_t t0 = millis();
  while (ts.touched() && (millis() - t0 < 300)) delay(5);
  wasDown = false;

  uint32_t now = millis();
  if (tapState == TAP_PENDING) {
    int dx = x - pendingTapX, dy = y - pendingTapY;
    if ((now - pendingTapAt) <= DTAP_MS &&
        abs(dx) <= TAP_SLOP && abs(dy) <= TAP_SLOP) {
      tapState = TAP_IDLE;
      toggleScreen();
      return;
    }
  }
  tapState = TAP_PENDING; pendingTapAt = now;
  pendingTapX = x; pendingTapY = y;
}

// ═════════════════════════════════════════════════════════════════════════════
//  BOOT button — short press: force refresh  |  hold 2 s: open portal
// ═════════════════════════════════════════════════════════════════════════════
#define BTN_LONG_MS  2000   // hold duration (ms) to trigger portal
#define BTN_DEBOUNCE   50   // debounce window (ms)

void buttonInit() {
  pinMode(PIN_BOOT, INPUT_PULLUP);
}

void buttonUpdate() {
  static bool     lastRaw    = HIGH;
  static bool     stableState = HIGH;
  static uint32_t lastChange = 0;
  static uint32_t pressedAt  = 0;
  static bool     longFired  = false;

  bool raw = digitalRead(PIN_BOOT);

  // Debounce
  if (raw != lastRaw) { lastChange = millis(); lastRaw = raw; return; }
  if (millis() - lastChange < BTN_DEBOUNCE) return;

  bool prev = stableState;
  stableState = raw;

  if (prev == HIGH && stableState == LOW) {
    // Falling edge — button pressed
    pressedAt = millis();
    longFired = false;
  } else if (stableState == LOW && !longFired) {
    // Held — check for long press threshold
    if (millis() - pressedAt >= BTN_LONG_MS) {
      longFired = true;
      openPortal();
    }
  } else if (prev == LOW && stableState == HIGH && !longFired) {
    // Rising edge — released before long press → short press: force refresh
    lastPriceUpdate = 0;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  openPortal — opens the WiFiManager config portal from the running app.
//  Saves changed settings and restarts if anything was modified.
// ─────────────────────────────────────────────────────────────────────────────
void openPortal() {
  int cx = tft.width() / 2;
  tft.fillScreen(TFT_BLACK);
  drawBorder();
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK); tft.setTextSize(1);
  tft.drawString("Settings portal open", cx, L_BORDER + 6);
  tft.setTextColor(C_DIM, TFT_BLACK);
  tft.drawString("Connect to BTC-Monitor Wi-Fi", cx, L_BORDER + 18);
  tft.setTextDatum(TL_DATUM);

  char tz_buf[5], orient_buf[3], cur_buf[5];
  snprintf(tz_buf,     sizeof(tz_buf),     "%d", (int)g_tz_hours);
  snprintf(orient_buf, sizeof(orient_buf), "%d", (int)g_orient);
  g_currency.toCharArray(cur_buf, sizeof(cur_buf));

  WiFiManagerParameter p_tz("tz",
    "Timezone offset in hours (e.g. -3 for Brasilia, 0 for London, 8 for HKG)",
    tz_buf, 4);
  WiFiManagerParameter p_orient("orient",
    "Orientation: 0 = portrait (upright)   1 = landscape (flat)",
    orient_buf, 2);
  WiFiManagerParameter p_cur("cur",
    "Comparison currency code: BRL EUR GBP JPY ARS CLP MXN CAD AUD",
    cur_buf, 4);

  WiFiManager wm;
  wm.addParameter(&p_tz);
  wm.addParameter(&p_orient);
  wm.addParameter(&p_cur);
  wm.setAPCallback(onPortalStart);
  if (WIFI_PORTAL_TIMEOUT_SEC > 0)
    wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_SEC);

  const char* pass = (strlen(WIFI_AP_PASS) > 0) ? WIFI_AP_PASS : nullptr;
  wm.startConfigPortal(WIFI_AP_NAME, pass);

  int8_t  newTz     = (int8_t) constrain(atoi(p_tz.getValue()), -12, 14);
  uint8_t newOrient = (uint8_t)constrain(atoi(p_orient.getValue()), 0, 1);
  String  newCur    = String(p_cur.getValue());
  newCur.toUpperCase(); newCur.trim();
  if (newCur.length() != 3) newCur = g_currency;

  if (newTz != g_tz_hours || newOrient != g_orient || newCur != g_currency) {
    savePrefs(newTz, newOrient, newCur);
    ESP.restart();
  }

  // Nothing changed — restore the screen
  tft.fillScreen(TFT_BLACK);
  drawBorder();
  redrawScreen();
}

// ═════════════════════════════════════════════════════════════════════════════
//  LDR (auto-brightness)
// ═════════════════════════════════════════════════════════════════════════════
void ldrInit() { analogReadResolution(12); pinMode(PIN_LDR, INPUT); }

void ldrUpdate() {
  if (!screenOn || millis() - lastLdrRead < 80) return;
  lastLdrRead = millis();

  int raw = analogRead(PIN_LDR);

  if (ldrEma < 0) ldrEma = raw;
  ldrEma += 0.12f * (raw - ldrEma);

  int r = constrain((int)ldrEma, LDR_RAW_LIGHT, LDR_RAW_DARK);
  float t = powf((float)(LDR_RAW_DARK - r) / (LDR_RAW_DARK - LDR_RAW_LIGHT), 1.6f);
  backlightSet((uint8_t)(BL_MIN + t * (BL_MAX - BL_MIN)));
}

// ═════════════════════════════════════════════════════════════════════════════
//  LED RGB (active LOW)
//
//  Off by default. Price-change effects:
//    Up   → BREATH_UP   : green sine-wave breathing for 10 s (~3 breaths)
//    Down → BREATH_DOWN : red   sine-wave breathing for 10 s (~3 breaths)
// ═════════════════════════════════════════════════════════════════════════════
void ledInit() {
  pinMode(PIN_LED_R, OUTPUT); analogWrite(PIN_LED_R, 255);
  pinMode(PIN_LED_G, OUTPUT); analogWrite(PIN_LED_G, 255);
  pinMode(PIN_LED_B, OUTPUT); analogWrite(PIN_LED_B, 255);
}

void ledWriteRGB(uint8_t r, uint8_t g, uint8_t b) {
  uint8_t br = LED_BRIGHTNESS;
  analogWrite(PIN_LED_R, 255 - (uint16_t)r * br / 255);
  analogWrite(PIN_LED_G, 255 - (uint16_t)g * br / 255);
  analogWrite(PIN_LED_B, 255 - (uint16_t)b * br / 255);
}

void ledOff() {
  analogWrite(PIN_LED_R, 255);
  analogWrite(PIN_LED_G, 255);
  analogWrite(PIN_LED_B, 255);
  ledMode = LED_OFF;
}

void ledPriceSignal(bool went_up) {
  ledMode        = went_up ? LED_BREATH_UP : LED_BREATH_DOWN;
  ledEffectStart = millis();
}

void ledUpdate() {
  if (ledMode == LED_OFF) return;

  uint32_t elapsed = millis() - ledEffectStart;
  if (elapsed >= 10000) { ledOff(); return; }   // total effect duration: 10 s

  // Sine-wave breathing: one full breath (0→peak→0) every 3 s
  float phase      = (float)(elapsed % 3000) / 3000.0f;   // 0..1 within current breath
  float brightness = sinf(phase * PI);                      // 0 → 1 → 0
  uint8_t val      = (uint8_t)(brightness * LED_BRIGHTNESS);

  if (ledMode == LED_BREATH_UP)   ledWriteRGB(0,   val, 0);   // green
  if (ledMode == LED_BREATH_DOWN) ledWriteRGB(val, 0,   0);   // red
}

// ═════════════════════════════════════════════════════════════════════════════
//  SPARKLINE (ring buffer)
// ═════════════════════════════════════════════════════════════════════════════
void histPush(float v) {
  priceHist[histHead] = v;
  histHead = (histHead + 1) % SPARKLINE_POINTS;
  if (histCount < SPARKLINE_POINTS) histCount++;
}

void drawSparkline(int x, int y, int w, int h) {
  tft.fillRect(x, y, w, h, TFT_BLACK);

  if (histCount < 2) {
    tft.setTextColor(C_LABEL, TFT_BLACK); tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("collecting history...", x + w/2, y + h/2);
    tft.setTextDatum(TL_DATUM);
    return;
  }

  float vmin = priceHist[(histHead - histCount + SPARKLINE_POINTS) % SPARKLINE_POINTS];
  float vmax = vmin;
  for (int i = 1; i < histCount; i++) {
    float v = priceHist[(histHead - histCount + i + SPARKLINE_POINTS) % SPARKLINE_POINTS];
    if (v < vmin) vmin = v; if (v > vmax) vmax = v;
  }

  float range = (vmax - vmin < 1.0f) ? 1.0f : (vmax - vmin);
  float pad   = range * 0.12f;
  vmin -= pad; vmax += pad; range = vmax - vmin;

  int px[SPARKLINE_POINTS], py[SPARKLINE_POINTS];
  for (int i = 0; i < histCount; i++) {
    float v = priceHist[(histHead - histCount + i + SPARKLINE_POINTS) % SPARKLINE_POINTS];
    px[i] = x + (i * (w - 1)) / (histCount - 1);
    py[i] = constrain(y + h - 1 - (int)((v - vmin) / range * (h - 2)), y, y+h-1);
  }

  for (int i = 0; i < histCount; i++)
    tft.drawLine(px[i], py[i], px[i], y + h - 1, C_SPARK_FILL);

  for (int i = 1; i < histCount; i++) {
    tft.drawLine(px[i-1], py[i-1],   px[i], py[i],   C_SPARK_LINE);
    tft.drawLine(px[i-1], py[i-1]+1, px[i], py[i]+1, C_SPARK_LINE);
  }
  tft.fillCircle(px[histCount-1], py[histCount-1], 2, TFT_WHITE);

  char buf[20];
  tft.setTextSize(1); tft.setTextColor(C_LABEL, TFT_BLACK);
  snprintf(buf, sizeof(buf), "$%.0f", vmax - pad);
  tft.setTextDatum(TL_DATUM); tft.drawString(buf, x + 2, y + 1);
  snprintf(buf, sizeof(buf), "$%.0f", vmin + pad);
  tft.drawString(buf, x + 2, y + h - 9);
  snprintf(buf, sizeof(buf), "%dm", (int)(histCount * PRICE_REFRESH_MS / 60000));
  tft.setTextDatum(TR_DATUM); tft.drawString(buf, x + w - 2, y + 1);
  tft.setTextDatum(TL_DATUM);
}

// ═════════════════════════════════════════════════════════════════════════════
//  COMMON UI
// ═════════════════════════════════════════════════════════════════════════════
void drawBorder() {
  int W = tft.width(), H = tft.height();
  for (int i = 0; i < L_BORDER; i++)
    tft.drawRect(i, i, W - 2*i, H - 2*i, C_ORANGE);
}

void drawPageDots() {
  int W  = tft.width();
  int y  = (g_orient == 0) ? L_DOTS_Y : LL_DOTS_Y;
  int cx = W / 2;
  for (int i = 0; i < 3; i++) {
    int dx = cx + (i - 1) * 14;
    if (i == curScreen) tft.fillCircle(dx, y, 3, C_ORANGE);
    else                tft.drawCircle(dx, y, 3, C_LABEL);
  }
}

void drawWifiDot(int x, int y, bool ok) {
  uint16_t c = ok ? TFT_GREEN : TFT_RED;
  tft.fillCircle(x, y, 3, ok ? c : TFT_BLACK);
  tft.drawCircle(x, y, 3, c);
}

// Change indicator ▲/▼ + percentage, centered within [x, x+w)
void drawChangeIndicator(int x, int y, int w, float cur, float prev) {
  tft.fillRect(x, y, w, 20, TFT_BLACK);
  if (prev <= 0 || cur <= 0) {                                                                                                                  
    tft.setTextColor(C_DIM, TFT_BLACK); tft.setTextSize(1);                                                                                     
    tft.setTextDatum(MC_DATUM);                                                                                                                 
    tft.drawString("--", x + w/2, y + 10);                                                                                                      
    tft.setTextDatum(TC_DATUM);                                                                                                                 
    return;                                                                                                                                     
  }

  float    pct   = (cur - prev) / prev * 100.0f;
  bool     up    = (pct >= 0);
  uint16_t color = up ? TFT_GREEN : TFT_RED;

  char buf[16];
  snprintf(buf, sizeof(buf), "%+.2f%%", pct);
  tft.setTextSize(2);
  int totalW = 14 + tft.textWidth(buf);
  int sx     = x + (w - totalW) / 2;
  int ty     = y + 2;

  if (up) tft.fillTriangle(sx+6, ty,    sx,    ty+10, sx+12, ty+10, color);
  else    tft.fillTriangle(sx,   ty,    sx+12, ty,    sx+6,  ty+10, color);

  tft.setTextColor(color, TFT_BLACK);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(buf, sx + 14, y + 9);
  // Restore caller's datum — this function is always called within a TC_DATUM context
  tft.setTextDatum(TC_DATUM);
}

// Status bar within [x, x+w): wifi dot | "Xm ago" | HH:MM
void drawStatusBar(int x, int y, int w) {
  tft.fillRect(x, y, w, 14, TFT_BLACK);
  bool connected = (WiFi.status() == WL_CONNECTED);
  drawWifiDot(x + 6, y + 7, connected);

  tft.setTextColor(C_DIM, TFT_BLACK); tft.setTextSize(1);
  char buf[24] = "no data";
  if (lastSuccessfulUpdate > 0) {
    unsigned long ago = (millis() - lastSuccessfulUpdate) / 1000;
    if      (ago < 60)   snprintf(buf, sizeof(buf), "now");
    else if (ago < 3600) snprintf(buf, sizeof(buf), "%lum ago", ago / 60);
    else                 snprintf(buf, sizeof(buf), ">1h ago");
  }
  tft.setTextDatum(ML_DATUM);
  tft.drawString(buf, x + 14, y + 7);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(nowHHMMSS().substring(0, 5), x + w - 3, y + 7);
  tft.setTextDatum(TL_DATUM);
}

// ═════════════════════════════════════════════════════════════════════════════
//  FORMATTING
//  Cents omitted — BTC prices are large enough that sub-dollar precision
//  adds no value and wastes display width.
// ═════════════════════════════════════════════════════════════════════════════
String fmtUSD(float v) {
  if (v <= 0) return "--";
  long w = lroundf(v);
  char buf[24];
  if      (w >= 1000000) snprintf(buf, sizeof(buf), "$%ld,%03ld,%03ld", w/1000000, (w/1000)%1000, w%1000);
  else if (w >= 1000)    snprintf(buf, sizeof(buf), "$%ld,%03ld",        w/1000,    w%1000);
  else                   snprintf(buf, sizeof(buf), "$%ld",               w);
  return String(buf);
}

String fmtLocal(float v) {
  if (v <= 0) return "--";
  long w = lroundf(v);
  char num[24];
  if      (w >= 1000000) snprintf(num, sizeof(num), "%ld,%03ld,%03ld", w/1000000, (w/1000)%1000, w%1000);
  else if (w >= 1000)    snprintf(num, sizeof(num), "%ld,%03ld",        w/1000,    w%1000);
  else                   snprintf(num, sizeof(num), "%ld",               w);
  char buf[32];
  if      (g_currency == "BRL") snprintf(buf, sizeof(buf), "R$%s",          num);
  else if (g_currency == "USD") snprintf(buf, sizeof(buf), "$%s",           num);
  else                          snprintf(buf, sizeof(buf), "%s %s", g_currency.c_str(), num);
  return String(buf);
}

// ═════════════════════════════════════════════════════════════════════════════
//  HTTP / APIs
// ═════════════════════════════════════════════════════════════════════════════
bool fetchBTCUSD(float &out) {
  WiFiClientSecure cl; cl.setInsecure();
  HTTPClient http; http.setTimeout(8000);

  if (http.begin(cl, BTC_PRIMARY)) {
    if (http.GET() == HTTP_CODE_OK) {
      JsonDocument doc;
      String body = http.getString(); http.end();
      if (!deserializeJson(doc, body)) {
        JsonVariant bid = doc["BTCUSD"]["bid"];
        if (bid.is<const char*>()) { out = atof(bid.as<const char*>()); return true; }
        if (bid.is<float>())       { out = bid.as<float>();              return true; }
      }
    } else { http.end(); }
  }

  if (http.begin(cl, BTC_FALLBACK)) {
    if (http.GET() == HTTP_CODE_OK) {
      JsonDocument doc;
      String body = http.getString(); http.end();
      if (!deserializeJson(doc, body)) {
        JsonVariant amt = doc["data"]["amount"];
        if (amt.is<const char*>()) { out = atof(amt.as<const char*>()); return true; }
        if (amt.is<float>())       { out = amt.as<float>();              return true; }
      }
    } else { http.end(); }
  }
  return false;
}

bool fetchBTCLocal(float &out) {
  if (g_currency == "USD") { out = btcUsd; return (btcUsd > 0); }

  WiFiClientSecure cl; cl.setInsecure();
  HTTPClient http; http.setTimeout(8000);

  // Primary: AwesomeAPI BTC-{CURRENCY}
  String url1 = "https://economia.awesomeapi.com.br/json/last/BTC-" + g_currency;
  if (http.begin(cl, url1)) {
    if (http.GET() == HTTP_CODE_OK) {
      JsonDocument doc;
      String body = http.getString(); http.end();
      if (!deserializeJson(doc, body)) {
        String key = "BTC" + g_currency;
        JsonVariant bid = doc[key.c_str()]["bid"];
        if (bid.is<const char*>()) { out = atof(bid.as<const char*>()); return true; }
        if (bid.is<float>())       { out = bid.as<float>();              return true; }
      }
    } else { http.end(); }
  }

  // Fallback: Coinbase BTC-{CURRENCY}
  String url2 = "https://api.coinbase.com/v2/prices/BTC-" + g_currency + "/spot";
  if (http.begin(cl, url2)) {
    if (http.GET() == HTTP_CODE_OK) {
      JsonDocument doc;
      String body = http.getString(); http.end();
      if (!deserializeJson(doc, body)) {
        JsonVariant amt = doc["data"]["amount"];
        if (amt.is<const char*>()) { out = atof(amt.as<const char*>()); return true; }
        if (amt.is<float>())       { out = amt.as<float>();              return true; }
      }
    } else { http.end(); }
  }
  return false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  SCREENS
// ═════════════════════════════════════════════════════════════════════════════
void redrawScreen() {
  drawBorder();
  switch (curScreen) {
    case 0: (g_orient == 0) ? drawScreen0_portrait() : drawScreen0_landscape(); break;
    case 1: drawScreen1();   break;
    case 2: drawScreen2();   break;
  }
  drawPageDots();  // always drawn last so screen content never overwrites the dots
}

// ─────────────────────────────────────────────────────────────────────────────
//  Screen 0 — Portrait  (240 × 320)
// ─────────────────────────────────────────────────────────────────────────────
void drawScreen0_portrait() {
  int W = tft.width(), cx = W / 2;
  tft.setTextDatum(TC_DATUM);

  // Clock + date
  tft.fillRect(L_BORDER+1, L_CLOCK_Y, W-2*(L_BORDER+1), L_SEP1_Y - L_CLOCK_Y, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(3);
  tft.drawString(nowHHMMSS(), cx, L_CLOCK_Y);
  tft.setTextColor(C_LABEL, TFT_BLACK); tft.setTextSize(1);
  tft.drawString(nowDate(), cx, L_DATE_Y);
  tft.drawLine(L_BORDER+4, L_SEP1_Y, W-L_BORDER-5, L_SEP1_Y, C_DIM);

  // BTC / USD
  tft.fillRect(L_BORDER+1, L_LBL_BTC_Y, W-2*(L_BORDER+1), L_SEP2_Y - L_LBL_BTC_Y, TFT_BLACK);
  tft.setTextColor(C_LABEL, TFT_BLACK); tft.setTextSize(1);
  tft.drawString("BTC / USD", cx, L_LBL_BTC_Y);
  tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(3);
  tft.drawString(fmtUSD(btcUsd), cx, L_PRICE_Y);
  drawChangeIndicator(L_BORDER+1, L_CHANGE_Y, W-2*(L_BORDER+1), btcUsd, btcUsdPrev);
  tft.drawLine(L_BORDER+4, L_SEP2_Y, W-L_BORDER-5, L_SEP2_Y, C_DIM);

  // BTC / local currency
  tft.fillRect(L_BORDER+1, L_LBL_LOC_Y, W-2*(L_BORDER+1), L_SPARK_Y - L_LBL_LOC_Y, TFT_BLACK);
  tft.setTextColor(C_LABEL, TFT_BLACK); tft.setTextSize(1);
  tft.drawString("BTC / " + g_currency, cx, L_LBL_LOC_Y);
  tft.setTextColor(C_GOLDEN, TFT_BLACK); tft.setTextSize(2);
  tft.drawString(fmtLocal(btcLocal), cx, L_LOCAL_Y);

  // Sparkline
  drawSparkline(L_BORDER+2, L_SPARK_Y, W-2*(L_BORDER+2), L_SPARK_H);
  tft.drawLine(L_BORDER+4, L_SEP3_Y, W-L_BORDER-5, L_SEP3_Y, C_DIM);

  // Status bar
  drawStatusBar(L_BORDER+1, L_STATUS_Y, W-2*(L_BORDER+1));
  tft.setTextDatum(TL_DATUM);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Screen 0 — Landscape  (320 × 240) — 2-column layout
// ─────────────────────────────────────────────────────────────────────────────
void drawScreen0_landscape() {
  int W = tft.width(), H = tft.height(), cx = W / 2;
  tft.setTextDatum(TC_DATUM);

  // Header (full width)
  tft.fillRect(L_BORDER+1, L_CLOCK_Y, W-2*(L_BORDER+1), LL_SEP_H - L_CLOCK_Y, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(3);
  tft.drawString(nowHHMMSS(), cx, L_CLOCK_Y);
  tft.setTextColor(C_LABEL, TFT_BLACK); tft.setTextSize(1);
  tft.drawString(nowDate(), cx, L_DATE_Y);
  tft.drawLine(LL_BORDER+4, LL_SEP_H, W-LL_BORDER-5, LL_SEP_H, C_DIM);

  // Vertical divider
  tft.drawLine(LL_DIV_X, LL_SEP_H+1, LL_DIV_X, H-LL_BORDER-1, C_DIM);

  // Left column — prices
  tft.fillRect(LL_LCOL_X, LL_LBL_BTC_Y, LL_LCOL_W, LL_FOOT_SEP_Y - LL_LBL_BTC_Y, TFT_BLACK);
  int lx = LL_LCOL_CX;
  tft.setTextColor(C_LABEL, TFT_BLACK); tft.setTextSize(1);
  tft.drawString("BTC / USD", lx, LL_LBL_BTC_Y);
  tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(2);
  tft.drawString(fmtUSD(btcUsd), lx, LL_PRICE_Y);
  drawChangeIndicator(LL_LCOL_X, LL_CHANGE_Y, LL_LCOL_W, btcUsd, btcUsdPrev);
  tft.drawLine(LL_LCOL_X+4, LL_SEP2_Y, LL_DIV_X-4, LL_SEP2_Y, C_DIM);

  tft.setTextColor(C_LABEL, TFT_BLACK); tft.setTextSize(1);
  tft.drawString("BTC / " + g_currency, lx, LL_LBL_LOC_Y);
  tft.setTextColor(C_GOLDEN, TFT_BLACK); tft.setTextSize(2);
  tft.drawString(fmtLocal(btcLocal), lx, LL_LOCAL_Y);

  // Right column — sparkline
  drawSparkline(LL_RCOL_X, LL_LBL_BTC_Y, LL_RCOL_W, LL_FOOT_SEP_Y - LL_LBL_BTC_Y);

  // Footer (full width)
  tft.drawLine(LL_BORDER+4, LL_FOOT_SEP_Y, W-LL_BORDER-5, LL_FOOT_SEP_Y, C_DIM);
  drawStatusBar(LL_BORDER+1, LL_STATUS_Y, W-2*(LL_BORDER+1));
  tft.setTextDatum(TL_DATUM);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Screen 1 — Details  (portrait and landscape)
// ─────────────────────────────────────────────────────────────────────────────
void drawScreen1() {
  int W = tft.width(), H = tft.height(), cx = W/2;
  int y = L_BORDER + 10;
  tft.setTextDatum(TC_DATUM);

  tft.setTextColor(C_LABEL, TFT_BLACK); tft.setTextSize(1);
  tft.drawString("BTC / USD", cx, y); y += 12;
  tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(3);
  tft.drawString(fmtUSD(btcUsd), cx, y); y += 28;

  drawChangeIndicator(L_BORDER+1, y, W-2*(L_BORDER+1), btcUsd, btcUsdPrev); y += 22;

  tft.setTextColor(C_LABEL, TFT_BLACK); tft.setTextSize(1);
  tft.drawString("prev: " + fmtUSD(btcUsdPrev), cx, y); y += 14;
  tft.drawLine(L_BORDER+4, y, W-L_BORDER-5, y, C_DIM); y += 10;

  tft.drawString("BTC / " + g_currency, cx, y); y += 12;
  tft.setTextColor(C_GOLDEN, TFT_BLACK); tft.setTextSize(2);
  tft.drawString(fmtLocal(btcLocal), cx, y); y += 22;
  tft.drawLine(L_BORDER+4, y, W-L_BORDER-5, y, C_DIM); y += 10;

  {
    char buf[32];
    if (lastUpdateTime > 0) {
      struct tm t;
      localtime_r(&lastUpdateTime, &t);
      strftime(buf, sizeof(buf), "updated: %H:%M", &t);
    } else {
      strcpy(buf, "updated: --:--");
    }
    tft.setTextColor(C_DIM, TFT_BLACK); tft.setTextSize(1);
    tft.drawString(buf, cx, y); y += 14;
  }

  // Session high / low / range from ring buffer
  // Use the same statusY constants as screen 0 so the status bar never overlaps the dots
  int statusY = (g_orient == 0) ? L_STATUS_Y : LL_STATUS_Y;
  tft.fillRect(L_BORDER+1, y, W-2*(L_BORDER+1), statusY - y, TFT_BLACK);
  if (histCount >= 2) {
    float sHigh = priceHist[(histHead - histCount + SPARKLINE_POINTS) % SPARKLINE_POINTS];
    float sLow  = sHigh;
    for (int i = 1; i < histCount; i++) {
      float v = priceHist[(histHead - histCount + i + SPARKLINE_POINTS) % SPARKLINE_POINTS];
      if (v > sHigh) sHigh = v;
      if (v < sLow)  sLow  = v;
    }
    char buf[32];
    if (g_orient == 0) {
      // Portrait — plenty of space, use size 2
      tft.drawLine(L_BORDER+4, y, W-L_BORDER-5, y, C_DIM); y += 8;
      tft.setTextColor(C_LABEL, TFT_BLACK); tft.setTextSize(1);
      tft.drawString("session", cx, y); y += 14;
      snprintf(buf, sizeof(buf), "High   %s", fmtUSD(sHigh).c_str());
      tft.setTextColor(TFT_GREEN, TFT_BLACK); tft.setTextSize(2);
      tft.drawString(buf, cx, y); y += 20;
      snprintf(buf, sizeof(buf), "Low    %s", fmtUSD(sLow).c_str());
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString(buf, cx, y); y += 20;
      snprintf(buf, sizeof(buf), "Range  %s", fmtUSD(sHigh - sLow).c_str());
      tft.setTextColor(C_DIM, TFT_BLACK);
      tft.drawString(buf, cx, y);
    } else {
      // Landscape — compact, use size 1
      tft.drawLine(L_BORDER+4, y, W-L_BORDER-5, y, C_DIM); y += 6;
      tft.setTextColor(C_LABEL, TFT_BLACK); tft.setTextSize(1);
      tft.drawString("session", cx, y); y += 10;
      snprintf(buf, sizeof(buf), "High   %s", fmtUSD(sHigh).c_str());
      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.drawString(buf, cx, y); y += 11;
      snprintf(buf, sizeof(buf), "Low    %s", fmtUSD(sLow).c_str());
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString(buf, cx, y); y += 11;
      snprintf(buf, sizeof(buf), "Range  %s", fmtUSD(sHigh - sLow).c_str());
      tft.setTextColor(C_DIM, TFT_BLACK);
      tft.drawString(buf, cx, y);
    }
  }

  drawStatusBar(L_BORDER+1, statusY, W-2*(L_BORDER+1));
  tft.setTextDatum(TL_DATUM);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Screen 2 — Clock  (portrait and landscape)
//  Uses TC_DATUM + computed clockY so updateClock() clears the exact same area
// ─────────────────────────────────────────────────────────────────────────────
void drawScreen2() {
  int W = tft.width(), H = tft.height(), cx = W/2;
  int clockY = H/2 - 36;   // top of clock text (TC_DATUM → top-center)
  int dateY  = clockY + 36;
  int sepY   = dateY + 12;

  tft.setTextDatum(TC_DATUM);

  // Clear clock area explicitly before drawing
  tft.fillRect(L_BORDER+1, clockY, W-2*(L_BORDER+1), 34, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(4);
  tft.drawString(nowHHMMSS(), cx, clockY);

  tft.setTextColor(C_LABEL, TFT_BLACK); tft.setTextSize(1);
  tft.drawString(nowDate(), cx, dateY);

  if (btcUsd > 0) {
    tft.drawLine(L_BORDER+4, sepY, W-L_BORDER-5, sepY, C_DIM);
    tft.setTextColor(C_ORANGE, TFT_BLACK); tft.setTextSize(2);
    tft.drawString(fmtUSD(btcUsd), cx, sepY + 4);
  }
  tft.setTextDatum(TL_DATUM);
}

// ─────────────────────────────────────────────────────────────────────────────
//  updateClock — refreshes only the clock area (called every second)
// ─────────────────────────────────────────────────────────────────────────────
void updateClock() {
  int W  = tft.width(), H = tft.height(), cx = W/2;
  int statusY = (g_orient == 0) ? L_STATUS_Y : LL_STATUS_Y;
  tft.setTextDatum(TC_DATUM);

  switch (curScreen) {
    case 0:
      tft.fillRect(L_BORDER+1, L_CLOCK_Y, W-2*(L_BORDER+1), 26, TFT_BLACK);
      tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(3);
      tft.drawString(nowHHMMSS(), cx, L_CLOCK_Y);
      drawStatusBar(L_BORDER+1, statusY, W-2*(L_BORDER+1));
      break;

    case 1:
      drawStatusBar(L_BORDER+1, statusY, W-2*(L_BORDER+1));
      break;

    case 2: {
      int clockY = H/2 - 36;
      tft.fillRect(L_BORDER+1, clockY, W-2*(L_BORDER+1), 34, TFT_BLACK);
      tft.setTextColor(TFT_WHITE, TFT_BLACK); tft.setTextSize(4);
      tft.drawString(nowHHMMSS(), cx, clockY);
      break;
    }
  }
  tft.setTextDatum(TL_DATUM);
}
