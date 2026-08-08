/*
 *  BTC Price Monitor — ESP32 CYD (ESP32-2432S028)
 *  https://github.com/JulianPedro/btc-price-monitor-cyd
 *
 *  Required libraries (Arduino Library Manager):
 *    TFT_eSPI · XPT2046_Touchscreen · ArduinoJson >= 7 · WiFiManager
 *
 *  Screens (single tap cycles forward, double tap toggles display):
 *    0 — Price       BTC/USD · local currency · sparkline
 *    1 — Network     block height · hashrate · difficulty retarget
 *    2 — Halving     countdown · epoch progress · 21M supply bar
 *    3 — Fees        mempool fee tiers (color bar chart)
 *    4 — Address     watch-only balance in BTC + sats
 *    5 — Clock       large time · Fear & Greed · days since genesis
 *    6 — Lightning   network capacity · channels · nodes
 *
 *  Gestures:
 *    Single tap          → next screen
 *    Double tap          → toggle display on/off
 *    BOOT short press    → force refresh all data
 *    BOOT hold 2 s       → open settings portal
 *    Touch hold 3 s boot → open settings portal
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <FS.h>
using fs::FS;
#include <WiFiManager.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>

#include "config.h"

// ═════════════════════════════════════════════════════════════════════════════
//  HARDWARE — CYD (ESP32-2432S028R)
// ═════════════════════════════════════════════════════════════════════════════
#define PIN_BL        21
#define PIN_LDR       34
#define PIN_LED_R      4
#define PIN_LED_G     16
#define PIN_LED_B     17
#define PIN_BOOT       0
#define TOUCH_CS      33
#define TOUCH_IRQ     36
#define TOUCH_SCK     25
#define TOUCH_MISO    39
#define TOUCH_MOSI    32

// ═════════════════════════════════════════════════════════════════════════════
//  PORTRAIT layout (240 × 320)
// ═════════════════════════════════════════════════════════════════════════════
#define L_BORDER      3
#define L_CLOCK_Y     6
#define L_DATE_Y      33
#define L_SEP1_Y      44
#define L_LBL_BTC_Y   48
#define L_PRICE_Y     58
#define L_CHANGE_Y    86
#define L_SEP2_Y     107
#define L_LBL_LOC_Y  111
#define L_LOCAL_Y    121
#define L_SPARK_Y    142
#define L_SPARK_H    118
#define L_SEP3_Y     263
#define L_STATUS_Y   267
#define L_DOTS_Y     310

// ═════════════════════════════════════════════════════════════════════════════
//  LANDSCAPE layout (320 × 240)
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
#define LL_DOTS_Y     230

#define NUM_SCREENS 7

// ═════════════════════════════════════════════════════════════════════════════
//  APIs
// ═════════════════════════════════════════════════════════════════════════════
static const char* BTC_PRIMARY   = "https://economia.awesomeapi.com.br/json/last/BTC-USD";
static const char* BTC_FALLBACK  = "https://api.coinbase.com/v2/prices/BTC-USD/spot";
static const char* AWESOME_BASE  = "https://economia.awesomeapi.com.br/json/last/BTC-";
static const char* COINBASE_BASE = "https://api.coinbase.com/v2/prices/BTC-";
static const char* API_FNG       = "https://api.alternative.me/fng/";

// mempool.space-compatible endpoints — host is configurable (see
// g_mempoolHost / mempoolBase()) so these are paths only, no scheme/host.
static const char* API_FEES_PATH   = "/api/v1/fees/recommended";
static const char* API_DIFF_PATH   = "/api/v1/difficulty-adjustment";
static const char* API_BLOCKS_PATH = "/api/v1/blocks";
static const char* API_ADDR_PATH   = "/api/address/";
static const char* API_LN_PATH     = "/api/v1/lightning/statistics/latest";

// ═════════════════════════════════════════════════════════════════════════════
//  OBJECTS
// ═════════════════════════════════════════════════════════════════════════════
TFT_eSPI            tft;
SPIClass            touchSPI(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
Preferences         prefs;

// ─── Color palette — minimal dark dashboard, single orange accent ─────────
uint16_t C_ORANGE, C_GOLDEN, C_LABEL, C_DIM, C_CARD, C_BG;
uint16_t C_GREEN, C_RED, C_YELLOW, C_BLUE, C_BLUE_FILL;

// ═════════════════════════════════════════════════════════════════════════════
//  PERSISTENT SETTINGS
// ═════════════════════════════════════════════════════════════════════════════
int8_t   g_tz_hours = -4;
uint8_t  g_orient   = 0;
String   g_currency = "USD";
String   g_watchAddr = "";
String   g_mempoolHost = DEFAULT_MEMPOOL_HOST;
String   g_userAgent   = DEFAULT_USER_AGENT;
uint16_t g_autoCycleSec = DEFAULT_AUTOCYCLE_SEC;   // 0 = auto-cycle disabled

void loadPrefs() {
  prefs.begin("btcmon", true);
  g_tz_hours     = (int8_t) prefs.getInt   ("tz",     (int)(TIMEZONE_OFFSET_SEC / 3600L));
  g_orient       = (uint8_t)prefs.getUInt  ("orient", 0);
  g_currency     =           prefs.getString("cur",    DEFAULT_CURRENCY);
  g_watchAddr    =           prefs.getString("addr",   "");
  g_mempoolHost  =           prefs.getString("mphost", DEFAULT_MEMPOOL_HOST);
  g_userAgent    =           prefs.getString("ua",     DEFAULT_USER_AGENT);
  g_autoCycleSec = (uint16_t)prefs.getUInt  ("acycle", DEFAULT_AUTOCYCLE_SEC);
  prefs.end();
  g_currency.toUpperCase();
  if (g_currency.length() != 3) g_currency = DEFAULT_CURRENCY;
  g_watchAddr.trim();
  g_mempoolHost.trim();
  if (g_mempoolHost.length() == 0) g_mempoolHost = DEFAULT_MEMPOOL_HOST;
  g_userAgent.trim();
  if (g_userAgent.length() == 0) g_userAgent = DEFAULT_USER_AGENT;
}

void savePrefs(int8_t tz, uint8_t orient, const String& cur, const String& addr,
               const String& mpHost, const String& ua, uint16_t autoCycleSec) {
  prefs.begin("btcmon", false);
  prefs.putInt   ("tz",     (int)tz);
  prefs.putUInt  ("orient", (unsigned int)orient);
  prefs.putString("cur",    cur);
  prefs.putString("addr",   addr);
  prefs.putString("mphost", mpHost);
  prefs.putString("ua",     ua);
  prefs.putUInt  ("acycle", (unsigned int)autoCycleSec);
  prefs.end();
}

// True for RFC1918 private ranges, loopback, and mDNS .local names — the
// only cases where defaulting to plain HTTP (no cert to validate) is a
// reasonable guess instead of a downgrade.
bool isPrivateHost(const String& h) {
  if (h.endsWith(".local"))    return true;
  if (h.startsWith("127."))    return true;
  if (h.startsWith("10."))     return true;
  if (h.startsWith("192.168.")) return true;
  if (h.startsWith("172.")) {
    int d1 = h.indexOf('.');
    int d2 = h.indexOf('.', d1 + 1);
    if (d2 > d1) {
      int second = h.substring(d1 + 1, d2).toInt();
      if (second >= 16 && second <= 31) return true;
    }
  }
  return false;
}

// Builds the scheme+host prefix for all mempool.space-compatible calls.
// Defaults to HTTPS for anything that isn't a recognized private/LAN
// address, so a public hostname the user types never silently downgrades
// to cleartext. A private address (typical home node) defaults to HTTP,
// since it rarely has a valid TLS cert. An explicit http:// or https://
// prefix always wins either way.
String mempoolBase() {
  String h = g_mempoolHost;
  h.trim();
  if (h.length() == 0) h = DEFAULT_MEMPOOL_HOST;
  while (h.endsWith("/")) h.remove(h.length() - 1);
  if (h.startsWith("http://") || h.startsWith("https://")) return h;
  return (isPrivateHost(h) ? "http://" : "https://") + h;
}

// Picks a plain or TLS client for a mempool.space-compatible URL and
// starts the HTTPClient request on it.
bool httpBeginMempool(HTTPClient& http, WiFiClientSecure& secureCl, WiFiClient& plainCl, const String& url) {
  if (url.startsWith("https://")) {
    secureCl.setInsecure();
    return http.begin(secureCl, url);
  }
  return http.begin(plainCl, url);
}

// Reads an HTTP response body into `out`, refusing anything whose declared
// Content-Length exceeds HTTP_MAX_BODY_BYTES, and refusing to keep an
// oversized body even when no length was declared up front — a malicious
// or misbehaving server (on any configured backend, not just
// mempool.space) can't use this to exhaust heap. Delegates the actual
// transfer to HTTPClient::getString(), which already honors
// http.setTimeout() — an earlier version of this function used its own
// polling loop with a much looser 15 s no-data stall guard, which made
// slow/degraded connections take far longer to fail than before this
// check existed. Call http.end() either way after this returns.
bool httpReadBody(HTTPClient& http, String& out) {
  int declaredLen = http.getSize();
  if (declaredLen > (int)HTTP_MAX_BODY_BYTES) return false;
  out = http.getString();
  if (out.length() > HTTP_MAX_BODY_BYTES) { out = ""; return false; }
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  TOUCH
// ═════════════════════════════════════════════════════════════════════════════
static const int      TS_MINX  = 250,  TS_MAXX = 3850;
static const int      TS_MINY  = 250,  TS_MAXY = 3850;
static const uint32_t DTAP_MS  = 350;
static const int      TAP_SLOP = 30;

enum  TapState { TAP_IDLE, TAP_PENDING };
TapState tapState     = TAP_IDLE;
uint32_t pendingTapAt = 0;
int16_t  pendingTapX  = -1, pendingTapY = -1;

// ═════════════════════════════════════════════════════════════════════════════
//  STATE
// ═════════════════════════════════════════════════════════════════════════════
bool    screenOn  = true;
uint8_t curScreen = 0;

// Price
float btcUsd = 0, btcUsdPrev = 0;
float btcLocal = 0, btcLocalPrev = 0;
unsigned long lastPriceUpdate      = 0;
unsigned long lastSuccessfulUpdate = 0;
time_t        lastUpdateTime       = 0;

// Sparkline
float   priceHist[SPARKLINE_POINTS];
uint8_t histHead  = 0;
uint8_t histCount = 0;

// Block / network
uint32_t g_blockHeight    = 0;
uint32_t g_lastBlockTime  = 0;
float    g_diffChange     = 0.0f;
float    g_epochProgress  = 0.0f;
uint32_t g_retargetBlocks = 0;
float    g_hashrate       = 0.0f;

// Mempool fees  (sat/vB)
uint8_t  g_feesFast  = 0;
uint8_t  g_feesHalf  = 0;
uint8_t  g_feesHour  = 0;
uint8_t  g_feesEcon  = 0;

// Fear & Greed
uint8_t g_fng = 0;
char    g_fngLabel[22] = "";
int8_t  g_prevFngExtreme = 0;   // 0=normal 1=extreme fear 2=extreme greed

// Watch address
int64_t g_addrBalance    = 0;
int64_t g_addrUnconfirmed = 0;
bool    g_addrFetched    = false;

// Lightning Network
double   g_lnCapacitySats  = 0;   // total public network capacity, in sats
uint32_t g_lnChannels      = 0;
uint32_t g_lnNodes         = 0;
double   g_lnAvgCapacity   = 0;   // average channel size, in sats
uint32_t g_lnTorNodes      = 0;
uint32_t g_lnClearnetNodes = 0;

// LDR
float    ldrEma      = -1.0f;
uint32_t lastLdrRead = 0;

// Clock redraw
uint32_t lastClockDraw = 0;

// Auto-cycle screens
unsigned long lastAutoCycle = 0;

// Fetch timers
unsigned long lastFeesUpdate    = 0;
unsigned long lastNetworkUpdate = 0;
unsigned long lastFngUpdate     = 0;
unsigned long lastAddrUpdate    = 0;
unsigned long lastLnUpdate      = 0;

// ─── LED ────────────────────────────────────────────────────────────────────
enum LedMode {
  LED_OFF,
  LED_BREATH_UP,    // price up   — green breathing, 10 s
  LED_BREATH_DOWN,  // price down — red breathing, 10 s
  LED_BLINK_BLOCK,  // new block mined — white double-blink
  LED_BLINK_FEE,    // mempool fee spike — amber single blink
  LED_PULSE_FEAR,   // F&G entered extreme fear  — blue pulse
  LED_PULSE_GREED,  // F&G entered extreme greed — purple pulse
};
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
void ledSelfTest();
void ledWriteRGB(uint8_t r, uint8_t g, uint8_t b);
void ledOff();
void ledUpdate();
void ledPriceSignal(bool went_up);
void ledBlockSignal();
void ledFeeSpikeSignal();
void ledFngSignal(bool extremeGreed);
void histPush(float v);
bool fetchBTCUSD(float &out);
bool fetchBTCLocal(float &out);
bool fetchFees();
bool fetchNetworkData();
bool fetchFearGreed();
bool fetchAddress();
bool fetchLightning();
void redrawScreen();
void drawBackground();
void drawScreen0_portrait();
void drawScreen0_landscape();
void drawScreen1_network();
void drawScreen2_halving();
void drawScreen3_fees();
void drawScreen4_address();
void drawScreen5_clock();
void drawScreen6_lightning();
void updateClock();
void drawBorder();
void drawPageDots();
void drawSparkline(int x, int y, int w, int h);
void drawChangeIndicator(int x, int y, int w, float cur, float prev);
void drawStatusBar(int x, int y, int w);
void drawWifiSignal(int x, int y, bool ok);
void sectionHeader(int x, int y, const char* lbl, uint16_t col = 0);
void hRule(int x, int y, int w);
void progressBar(int x, int y, int w, int h, float pct, uint16_t col);
uint16_t feeColor(uint8_t sat_vb);
uint16_t fngColor(uint8_t val);
String fmtUSD(float v);
String fmtLocal(float v);
String fmtBlockNum(uint32_t h);
String fmtHashrate(float eh);
String fmtSats(int64_t sats);
String lastBlockAge();
float  calcCirculating(uint32_t height);
uint32_t calcNextHalving(uint32_t height);
uint32_t daysSinceGenesis();
String nowHHMMSS();
String nowDate();

// ═════════════════════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  loadPrefs();
  ledInit();
  ledSelfTest();

  tft.init();
  tft.setRotation(g_orient == 0 ? 2 : 1);

  C_ORANGE    = tft.color565(247, 147,  26);   // BTC brand — primary accent
  C_GOLDEN    = tft.color565(255, 191,  92);   // warm secondary accent
  C_LABEL     = tft.color565(148, 158, 173);   // cool slate-gray, labels
  C_DIM       = tft.color565( 68,  74,  86);   // rules / dim text / dots
  C_CARD      = tft.color565( 34,  38,  46);   // card surface — subtle lift off bg
  C_BG        = tft.color565( 22,  25,  31);   // flat graphite background
  C_GREEN     = tft.color565( 61, 209, 130);   // flat green — price up / low fees
  C_RED       = tft.color565(235,  87,  87);   // flat red — price down / high fees
  C_YELLOW    = tft.color565(240, 180,  60);   // amber — mid fee tier
  C_BLUE      = tft.color565( 90, 169, 230);   // steel blue — hashrate / sparkline accent
  C_BLUE_FILL = tft.color565( 24,  40,  54);   // sparkline fill, muted blue-gray

  pinMode(PIN_BL, OUTPUT);
  backlightOn();
  drawBackground();
  drawBorder();

  touchInit();
  buttonInit();
  ldrInit();

  setupWifi();
  if (WiFi.status() == WL_CONNECTED) setupTime();

  if (fetchBTCUSD(btcUsd)) histPush(btcUsd);
  fetchBTCLocal(btcLocal);
  if (btcUsd > 0) { lastSuccessfulUpdate = millis(); time(&lastUpdateTime); }

  fetchFees();
  fetchNetworkData();
  fetchFearGreed();
  if (g_watchAddr.length() > 0) fetchAddress();
  fetchLightning();

  lastPriceUpdate   = millis();
  lastFeesUpdate    = millis();
  lastNetworkUpdate = millis();
  lastFngUpdate     = millis();
  lastAddrUpdate    = millis();
  lastLnUpdate      = millis();
  lastClockDraw     = millis();
  lastAutoCycle     = millis();

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

  bool changed = false;

  if (WiFi.status() == WL_CONNECTED) {
    // Price
    if (millis() - lastPriceUpdate >= PRICE_REFRESH_MS) {
      lastPriceUpdate = millis();
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
        changed = true;
      }
      if (okL) { btcLocalPrev = btcLocal; btcLocal = nl; changed = true; }
    }

    // Fees
    if (millis() - lastFeesUpdate >= FEES_REFRESH_MS) {
      lastFeesUpdate = millis();
      uint8_t prevFast = g_feesFast;
      if (fetchFees()) {
        if (prevFast > 0 && g_feesFast >= prevFast + FEE_SPIKE_DELTA) ledFeeSpikeSignal();
        if (curScreen == 3) changed = true;
      }
    }

    // Network / block / difficulty
    if (millis() - lastNetworkUpdate >= NETWORK_REFRESH_MS) {
      lastNetworkUpdate = millis();
      uint32_t prevHeight = g_blockHeight;
      if (fetchNetworkData()) {
        if (prevHeight > 0 && g_blockHeight > prevHeight) ledBlockSignal();
        if (curScreen == 1 || curScreen == 2) changed = true;
      }
    }

    // Fear & Greed
    if (millis() - lastFngUpdate >= FNG_REFRESH_MS) {
      lastFngUpdate = millis();
      if (fetchFearGreed()) {
        bool extremeFear  = (g_fng <= 24);
        bool extremeGreed = (g_fng >= 75);
        if (extremeFear  && g_prevFngExtreme != 1) ledFngSignal(false);
        if (extremeGreed && g_prevFngExtreme != 2) ledFngSignal(true);
        g_prevFngExtreme = extremeFear ? 1 : (extremeGreed ? 2 : 0);
        if (curScreen == 5) changed = true;
      }
    }

    // Address
    if (g_watchAddr.length() > 0 && millis() - lastAddrUpdate >= ADDR_REFRESH_MS) {
      lastAddrUpdate = millis();
      if (fetchAddress() && curScreen == 4) changed = true;
    }

    // Lightning Network
    if (millis() - lastLnUpdate >= LN_REFRESH_MS) {
      lastLnUpdate = millis();
      if (fetchLightning() && curScreen == 6) changed = true;
    }
  }

  // Auto-cycle screens (0 = disabled). A manual tap (touchUpdate) or
  // turning the screen back on also resets this timer, so the interval
  // always restarts from the moment the user last actually looked.
  if (g_autoCycleSec > 0 && screenOn &&
      millis() - lastAutoCycle >= (unsigned long)g_autoCycleSec * 1000UL) {
    lastAutoCycle = millis();
    curScreen = (curScreen + 1) % NUM_SCREENS;
    drawBackground();
    redrawScreen();
    changed = false;
  }

  if (changed && screenOn) redrawScreen();

  if (millis() - lastClockDraw >= 1000) {
    lastClockDraw = millis();
    if (screenOn) updateClock();
  }

  delay(10);
}

// ═════════════════════════════════════════════════════════════════════════════
//  WI-FI
// ═════════════════════════════════════════════════════════════════════════════
static void onPortalStart(WiFiManager*) {
  tft.fillRect(L_BORDER+1, L_BORDER+1,
               tft.width()-2*(L_BORDER+1), 100, C_BG);
  int cx = tft.width() / 2;
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_ORANGE, C_BG); tft.setTextSize(1);
  tft.drawString("Settings portal open", cx, 10);
  tft.setTextColor(C_LABEL, C_BG);
  tft.drawString("Connect to AP:", cx, 22);
  tft.setTextColor(TFT_WHITE, C_BG); tft.setTextSize(2);
  tft.drawString(WIFI_AP_NAME, cx, 34);
  tft.setTextColor(C_YELLOW, C_BG); tft.setTextSize(1);
  tft.drawString("then open 192.168.4.1", cx, 58);
  tft.setTextDatum(TL_DATUM);
}

void setupWifi() {
  int cx = tft.width() / 2;
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_YELLOW, C_BG); tft.setTextSize(1);
  tft.drawString("Connecting to Wi-Fi...", cx, L_BORDER + 6);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("Hold 3s to open settings", cx, L_BORDER + 18);
  tft.setTextDatum(TL_DATUM);

  bool     forcePortal = false;
  uint32_t holdStart   = 0;
  uint32_t checkEnd    = millis() + 4000;
  while (millis() < checkEnd && !forcePortal) {
    int16_t tx, ty;
    if (touchGetXY(tx, ty)) {
      if (holdStart == 0) holdStart = millis();
      if (millis() - holdStart >= 3000) forcePortal = true;
    } else { holdStart = 0; }
    delay(20);
  }

  char tz_buf[5], orient_buf[3], cur_buf[5], addr_buf[65], mphost_buf[65];
  char ua_buf[41], acycle_buf[6];
  snprintf(tz_buf,     sizeof(tz_buf),     "%d",  (int)g_tz_hours);
  snprintf(orient_buf, sizeof(orient_buf), "%d",  (int)g_orient);
  g_currency.toCharArray(cur_buf, sizeof(cur_buf));
  g_watchAddr.toCharArray(addr_buf, sizeof(addr_buf));
  g_mempoolHost.toCharArray(mphost_buf, sizeof(mphost_buf));
  g_userAgent.toCharArray(ua_buf, sizeof(ua_buf));
  snprintf(acycle_buf, sizeof(acycle_buf), "%u", g_autoCycleSec);

  WiFiManagerParameter p_tz("tz",
    "Timezone offset in hours (e.g. -3 for Brasilia)", tz_buf, 4);
  WiFiManagerParameter p_orient("orient",
    "Orientation: 0=portrait  1=landscape", orient_buf, 2);
  WiFiManagerParameter p_cur("cur",
    "Comparison currency: BRL EUR GBP JPY ARS CLP MXN CAD AUD", cur_buf, 4);
  WiFiManagerParameter p_addr("addr",
    "Watch address (BTC, leave blank to disable)", addr_buf, 64);
  WiFiManagerParameter p_mphost("mphost",
    "Mempool host (mempool.space, or e.g. 192.168.1.50:4081 for your own node)", mphost_buf, 64);
  WiFiManagerParameter p_ua("ua",
    "HTTP User-Agent sent to APIs", ua_buf, 40);
  WiFiManagerParameter p_acycle("acycle",
    "Auto-cycle screens every N seconds (0 = off, manual tap only)", acycle_buf, 5);

  WiFiManager wm;
  wm.addParameter(&p_tz);
  wm.addParameter(&p_orient);
  wm.addParameter(&p_cur);
  wm.addParameter(&p_addr);
  wm.addParameter(&p_mphost);
  wm.addParameter(&p_ua);
  wm.addParameter(&p_acycle);
  wm.setConnectTimeout(20);
  wm.setAPCallback(onPortalStart);
  // Saving via the params-only "Setup" page (as opposed to reconfiguring
  // WiFi itself) doesn't close the portal on its own — closing it here
  // means the device just gets on with restarting instead of sitting on
  // the "Saved" page until someone manually goes back and taps Exit.
  wm.setSaveParamsCallback([&wm]() { wm.stopConfigPortal(); });
  if (WIFI_PORTAL_TIMEOUT_SEC > 0) wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_SEC);

  const char* pass = (strlen(WIFI_AP_PASS) > 0) ? WIFI_AP_PASS : nullptr;
  if (forcePortal) {
    tft.fillRect(L_BORDER+1, L_BORDER+1, tft.width()-2*(L_BORDER+1), 30, C_BG);
    wm.startConfigPortal(WIFI_AP_NAME, pass);
  } else {
    // Don't let autoConnect() fall back to opening the portal on its own
    // if the saved Wi-Fi is unreachable (router reboot, outage, etc.) —
    // that would broadcast the setup AP unattended every time. Instead
    // just go offline; the portal only opens on the explicit gestures
    // (touch-hold above, or BOOT-hold via openPortal()).
    wm.setEnableConfigPortal(false);
    wm.autoConnect(WIFI_AP_NAME, pass);
  }

  int8_t   newTz     = (int8_t) constrain(atoi(p_tz.getValue()), -12, 14);
  uint8_t  newOrient = (uint8_t)constrain(atoi(p_orient.getValue()), 0, 1);
  String   newCur    = String(p_cur.getValue());
  newCur.toUpperCase(); newCur.trim();
  if (newCur.length() != 3) newCur = g_currency;
  String newAddr = String(p_addr.getValue()); newAddr.trim();
  String newMpHost = String(p_mphost.getValue()); newMpHost.trim();
  if (newMpHost.length() == 0) newMpHost = DEFAULT_MEMPOOL_HOST;
  String newUa = String(p_ua.getValue()); newUa.trim();
  if (newUa.length() == 0) newUa = DEFAULT_USER_AGENT;
  uint16_t newAutoCycle = (uint16_t)constrain(atoi(p_acycle.getValue()), 0, 3600);

  if (newTz != g_tz_hours || newOrient != g_orient || newCur != g_currency ||
      newAddr != g_watchAddr || newMpHost != g_mempoolHost ||
      newUa != g_userAgent || newAutoCycle != g_autoCycleSec) {
    savePrefs(newTz, newOrient, newCur, newAddr, newMpHost, newUa, newAutoCycle);
    ESP.restart();
  }

  tft.fillRect(L_BORDER+1, L_BORDER+1, tft.width()-2*(L_BORDER+1), 100, C_BG);

  if (WiFi.status() != WL_CONNECTED) {
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_RED, C_BG); tft.setTextSize(1);
    tft.drawString("Wi-Fi not connected", tft.width()/2, 10);
    tft.drawString("Offline mode", tft.width()/2, 22);
    tft.setTextDatum(TL_DATUM);
    delay(2000);
    tft.fillRect(L_BORDER+1, L_BORDER+1, tft.width()-2*(L_BORDER+1), 40, C_BG);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  openPortal — launched from the BOOT button during normal operation
// ─────────────────────────────────────────────────────────────────────────────
void openPortal() {
  drawBackground(); drawBorder();
  int cx = tft.width() / 2;
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_YELLOW, C_BG); tft.setTextSize(1);
  tft.drawString("Settings portal open", cx, L_BORDER + 6);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("Connect to BTC-Monitor Wi-Fi", cx, L_BORDER + 18);
  tft.setTextDatum(TL_DATUM);

  char tz_buf[5], orient_buf[3], cur_buf[5], addr_buf[65], mphost_buf[65];
  char ua_buf[41], acycle_buf[6];
  snprintf(tz_buf,     sizeof(tz_buf),     "%d",  (int)g_tz_hours);
  snprintf(orient_buf, sizeof(orient_buf), "%d",  (int)g_orient);
  g_currency.toCharArray(cur_buf, sizeof(cur_buf));
  g_watchAddr.toCharArray(addr_buf, sizeof(addr_buf));
  g_mempoolHost.toCharArray(mphost_buf, sizeof(mphost_buf));
  g_userAgent.toCharArray(ua_buf, sizeof(ua_buf));
  snprintf(acycle_buf, sizeof(acycle_buf), "%u", g_autoCycleSec);

  WiFiManagerParameter p_tz("tz",     "Timezone offset in hours", tz_buf, 4);
  WiFiManagerParameter p_orient("orient", "Orientation: 0=portrait  1=landscape", orient_buf, 2);
  WiFiManagerParameter p_cur("cur",   "Comparison currency code", cur_buf, 4);
  WiFiManagerParameter p_addr("addr", "Watch address (BTC)", addr_buf, 64);
  WiFiManagerParameter p_mphost("mphost",
    "Mempool host (mempool.space, or e.g. 192.168.1.50:4081 for your own node)", mphost_buf, 64);
  WiFiManagerParameter p_ua("ua",
    "HTTP User-Agent sent to APIs", ua_buf, 40);
  WiFiManagerParameter p_acycle("acycle",
    "Auto-cycle screens every N seconds (0 = off, manual tap only)", acycle_buf, 5);

  WiFiManager wm;
  wm.addParameter(&p_tz);
  wm.addParameter(&p_orient);
  wm.addParameter(&p_cur);
  wm.addParameter(&p_addr);
  wm.addParameter(&p_mphost);
  wm.addParameter(&p_ua);
  wm.addParameter(&p_acycle);
  wm.setAPCallback(onPortalStart);
  // See setupWifi() — closes the portal automatically once params are
  // saved, instead of leaving it open until someone taps Exit by hand.
  wm.setSaveParamsCallback([&wm]() { wm.stopConfigPortal(); });
  if (WIFI_PORTAL_TIMEOUT_SEC > 0) wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_SEC);

  const char* pass = (strlen(WIFI_AP_PASS) > 0) ? WIFI_AP_PASS : nullptr;
  wm.startConfigPortal(WIFI_AP_NAME, pass);

  int8_t   newTz     = (int8_t) constrain(atoi(p_tz.getValue()), -12, 14);
  uint8_t  newOrient = (uint8_t)constrain(atoi(p_orient.getValue()), 0, 1);
  String   newCur    = String(p_cur.getValue());
  newCur.toUpperCase(); newCur.trim();
  if (newCur.length() != 3) newCur = g_currency;
  String newAddr = String(p_addr.getValue()); newAddr.trim();
  String newMpHost = String(p_mphost.getValue()); newMpHost.trim();
  if (newMpHost.length() == 0) newMpHost = DEFAULT_MEMPOOL_HOST;
  String newUa = String(p_ua.getValue()); newUa.trim();
  if (newUa.length() == 0) newUa = DEFAULT_USER_AGENT;
  uint16_t newAutoCycle = (uint16_t)constrain(atoi(p_acycle.getValue()), 0, 3600);

  if (newTz != g_tz_hours || newOrient != g_orient || newCur != g_currency ||
      newAddr != g_watchAddr || newMpHost != g_mempoolHost ||
      newUa != g_userAgent || newAutoCycle != g_autoCycleSec) {
    savePrefs(newTz, newOrient, newCur, newAddr, newMpHost, newUa, newAutoCycle);
    ESP.restart();
  }

  drawBackground(); drawBorder(); redrawScreen();
}

// ═════════════════════════════════════════════════════════════════════════════
//  TIME (SNTP)
// ═════════════════════════════════════════════════════════════════════════════
void setupTime() {
  configTime((long)g_tz_hours * 3600L, DST_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
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
    backlightOff(); tft.fillScreen(TFT_BLACK);
  } else {
    backlightOn(); drawBackground(); redrawScreen();
    lastAutoCycle = millis();
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
      curScreen = (curScreen + 1) % NUM_SCREENS;
      drawBackground();
      redrawScreen();
      lastAutoCycle = millis();
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
  tapState = TAP_PENDING;
  pendingTapAt = now;
  pendingTapX = x; pendingTapY = y;
}

// ═════════════════════════════════════════════════════════════════════════════
//  BOOT BUTTON
// ═════════════════════════════════════════════════════════════════════════════
#define BTN_LONG_MS  2000
#define BTN_DEBOUNCE   50

void buttonInit() { pinMode(PIN_BOOT, INPUT_PULLUP); }

void buttonUpdate() {
  static bool     lastRaw    = HIGH;
  static bool     stableState = HIGH;
  static uint32_t lastChange = 0;
  static uint32_t pressedAt  = 0;
  static bool     longFired  = false;

  bool raw = digitalRead(PIN_BOOT);
  if (raw != lastRaw) { lastChange = millis(); lastRaw = raw; return; }
  if (millis() - lastChange < BTN_DEBOUNCE) return;

  bool prev = stableState; stableState = raw;

  if (prev == HIGH && stableState == LOW) {
    pressedAt = millis(); longFired = false;
  } else if (stableState == LOW && !longFired) {
    if (millis() - pressedAt >= BTN_LONG_MS) { longFired = true; openPortal(); }
  } else if (prev == LOW && stableState == HIGH && !longFired) {
    // Short press → force refresh
    lastPriceUpdate   = 0;
    lastFeesUpdate    = 0;
    lastNetworkUpdate = 0;
    lastFngUpdate     = 0;
    lastAddrUpdate    = 0;
  }
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
// ═════════════════════════════════════════════════════════════════════════════
void ledInit() {
  pinMode(PIN_LED_R, OUTPUT); analogWrite(PIN_LED_R, 255);
  pinMode(PIN_LED_G, OUTPUT); analogWrite(PIN_LED_G, 255);
  pinMode(PIN_LED_B, OUTPUT); analogWrite(PIN_LED_B, 255);
}

// Unmissable R -> G -> B flash at every boot, independent of WiFi/price/
// block timing. If this isn't visible on the physical LED, the problem is
// in the pins/wiring, not in any of the event-triggered signals below —
// GPIO16/17 are shared with onboard PSRAM on some CYD board revisions and
// may not drive the LED as expected.
void ledSelfTest() {
  ledWriteRGB(255, 0, 0); delay(200);
  ledWriteRGB(0, 255, 0); delay(200);
  ledWriteRGB(0, 0, 255); delay(200);
  ledOff();
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
  ledMode = went_up ? LED_BREATH_UP : LED_BREATH_DOWN;
  ledEffectStart = millis();
}

// New block mined — short, sharp double-blink so it reads as an "event"
// rather than the slow breathing used for price moves.
void ledBlockSignal() {
  ledMode = LED_BLINK_BLOCK;
  ledEffectStart = millis();
}

// Mempool fastest-fee jumped by FEE_SPIKE_DELTA sat/vB or more since the
// last poll — single amber blink.
void ledFeeSpikeSignal() {
  ledMode = LED_BLINK_FEE;
  ledEffectStart = millis();
}

// Fear & Greed index just crossed into an extreme zone.
void ledFngSignal(bool extremeGreed) {
  ledMode = extremeGreed ? LED_PULSE_GREED : LED_PULSE_FEAR;
  ledEffectStart = millis();
}

void ledUpdate() {
  if (ledMode == LED_OFF) return;
  uint32_t elapsed = millis() - ledEffectStart;

  switch (ledMode) {
    case LED_BREATH_UP:
    case LED_BREATH_DOWN: {
      if (elapsed >= 10000) { ledOff(); return; }
      float phase      = (float)(elapsed % 3000) / 3000.0f;
      float brightness = sinf(phase * PI);
      uint8_t val       = (uint8_t)(brightness * LED_BRIGHTNESS);
      if (ledMode == LED_BREATH_UP) ledWriteRGB(0,   val, 0);
      else                          ledWriteRGB(val, 0,   0);
      break;
    }
    case LED_BLINK_BLOCK: {
      if (elapsed >= 800) { ledOff(); return; }
      bool on = (elapsed < 150) || (elapsed >= 300 && elapsed < 450);
      ledWriteRGB(on ? 255 : 0, on ? 255 : 0, on ? 255 : 0);
      break;
    }
    case LED_BLINK_FEE: {
      if (elapsed >= 500) { ledOff(); return; }
      bool on = (elapsed < 300);
      ledWriteRGB(on ? 255 : 0, on ? 160 : 0, 0);
      break;
    }
    case LED_PULSE_FEAR:
    case LED_PULSE_GREED: {
      if (elapsed >= 2000) { ledOff(); return; }
      float phase      = (float)elapsed / 2000.0f;
      float brightness = sinf(phase * PI);
      uint8_t val       = (uint8_t)(brightness * LED_BRIGHTNESS);
      if (ledMode == LED_PULSE_FEAR) ledWriteRGB(0,   (uint8_t)(val * 0.3f), val);
      else                           ledWriteRGB((uint8_t)(val * 0.65f), 0, val);
      break;
    }
    default: break;
  }
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
  tft.fillRoundRect(x, y, w, h, 4, C_CARD);

  if (histCount < 2) {
    tft.setTextColor(C_LABEL, C_CARD); tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("collecting data...", x + w/2, y + h/2);
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

  // Points anchor to fixed slots along the full SPARKLINE_POINTS timeline
  // (not histCount), so the chart fills in left-to-right as data arrives
  // instead of re-stretching — and visually shifting — every existing
  // point on each new sample.
  int baseline = y + h - 4;
  int px[SPARKLINE_POINTS], py[SPARKLINE_POINTS];
  for (int i = 0; i < histCount; i++) {
    float v = priceHist[(histHead - histCount + i + SPARKLINE_POINTS) % SPARKLINE_POINTS];
    px[i] = x + 2 + (i * (w - 5)) / (SPARKLINE_POINTS - 1);
    py[i] = constrain(baseline - (int)((v - vmin) / range * (h - 8)), y + 2, baseline);
  }

  // Solid fill under the curve — one quad (two triangles) per segment,
  // instead of a per-point vertical "comb" that left visible gaps.
  for (int i = 1; i < histCount; i++) {
    tft.fillTriangle(px[i-1], py[i-1], px[i], py[i], px[i-1], baseline, C_BLUE_FILL);
    tft.fillTriangle(px[i],   py[i],   px[i], baseline, px[i-1], baseline, C_BLUE_FILL);
  }

  for (int i = 1; i < histCount; i++) {
    tft.drawLine(px[i-1], py[i-1],   px[i], py[i],   C_BLUE);
    tft.drawLine(px[i-1], py[i-1]+1, px[i], py[i]+1, C_BLUE);
  }
  tft.fillCircle(px[histCount-1], py[histCount-1], 3, TFT_WHITE);
  tft.fillCircle(px[histCount-1], py[histCount-1], 2, C_BLUE);

  tft.setTextSize(1);
  char buf[20];
  snprintf(buf, sizeof(buf), "$%.0f", vmax - pad);
  tft.setTextColor(C_LABEL, C_CARD);
  tft.setTextDatum(TL_DATUM); tft.drawString(buf, x + 4, y + 3);
  snprintf(buf, sizeof(buf), "$%.0f", vmin + pad);
  tft.drawString(buf, x + 4, y + h - 11);
  snprintf(buf, sizeof(buf), "%dm", (int)(histCount * PRICE_REFRESH_MS / 60000));
  tft.setTextDatum(TR_DATUM); tft.drawString(buf, x + w - 4, y + 3);

  tft.setTextDatum(TL_DATUM);
}

// ═════════════════════════════════════════════════════════════════════════════
//  DRAWING HELPERS
// ═════════════════════════════════════════════════════════════════════════════
// Flat graphite page background. Drawn once per screen switch; all
// per-region redraws below erase to the same C_BG, so no seams.
void drawBackground() {
  tft.fillScreen(C_BG);
}

// Single thin orange accent frame.
void drawBorder() {
  tft.drawRect(0, 0, tft.width(), tft.height(), C_ORANGE);
}

void drawPageDots() {
  int W  = tft.width();
  int y  = (g_orient == 0) ? L_DOTS_Y : LL_DOTS_Y;
  int cx = W / 2;
  for (int i = 0; i < NUM_SCREENS; i++) {
    int dx = cx - (NUM_SCREENS - 1) * 6 + i * 12;
    if (i == curScreen) tft.fillCircle(dx, y, 3, C_ORANGE);
    else                tft.drawCircle(dx, y, 3, C_DIM);
  }
}

// 4-bar signal strength indicator. x = left edge, y = baseline (bars grow
// upward from here). Disconnected draws a plain red dot instead of bars.
void drawWifiSignal(int x, int y, bool ok) {
  if (!ok) {
    tft.fillCircle(x + 3, y - 3, 3, C_RED);
    tft.drawCircle(x + 3, y - 3, 3, C_RED);
    return;
  }
  int32_t rssi = WiFi.RSSI();
  int bars = (rssi >= -55) ? 4 : (rssi >= -65) ? 3 : (rssi >= -75) ? 2 : 1;
  static const uint8_t H[4] = { 3, 5, 7, 9 };
  const int bw = 2, gap = 1;
  for (int i = 0; i < 4; i++) {
    int bx = x + i * (bw + gap);
    tft.fillRect(bx, y - H[i], bw, H[i], (i < bars) ? C_GREEN : C_DIM);
  }
}

// Left accent-bar + uppercase label
// NOTE: every screen-draw function sets TC_DATUM once at the top and expects
// it to still be active for the "centered value" draws that follow a
// section header — so this restores TC_DATUM, not TL_DATUM, when it's done.
void sectionHeader(int x, int y, const char* lbl, uint16_t col) {
  if (col == 0) col = C_ORANGE;
  tft.fillRect(x, y, 3, 9, col);
  tft.setTextColor(C_LABEL, C_BG);
  tft.setTextSize(1);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(lbl, x + 6, y + 4);
  tft.setTextDatum(TC_DATUM);
}

void hRule(int x, int y, int w) {
  tft.drawFastHLine(x, y, w, C_DIM);
}

void progressBar(int x, int y, int w, int h, float pct, uint16_t col) {
  tft.drawRoundRect(x, y, w, h, 2, C_DIM);
  int fill = constrain((int)(pct / 100.0f * (w - 2)), 0, w - 2);
  if (fill > 0) tft.fillRoundRect(x + 1, y + 1, fill, h - 2, 1, col);
}

void drawChangeIndicator(int x, int y, int w, float cur, float prev) {
  tft.fillRect(x, y, w, 20, C_BG);
  if (prev <= 0 || cur <= 0) {
    tft.setTextColor(C_DIM, C_BG); tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("--", x + w/2, y + 10);
    tft.setTextDatum(TC_DATUM);
    return;
  }
  float    pct   = (cur - prev) / prev * 100.0f;
  bool     up    = (pct >= 0);
  uint16_t color = up ? C_GREEN : C_RED;
  char buf[16];
  snprintf(buf, sizeof(buf), "%+.2f%%", pct);
  tft.setTextSize(2);
  int totalW = 14 + tft.textWidth(buf);
  int sx = x + (w - totalW) / 2;
  int ty = y + 2;
  if (up) tft.fillTriangle(sx+6, ty,    sx,    ty+10, sx+12, ty+10, color);
  else    tft.fillTriangle(sx,   ty,    sx+12, ty,    sx+6,  ty+10, color);
  tft.setTextColor(color, C_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(buf, sx + 14, y + 9);
  tft.setTextDatum(TC_DATUM);
}

void drawStatusBar(int x, int y, int w) {
  tft.fillRect(x, y, w, 14, C_BG);
  bool connected = (WiFi.status() == WL_CONNECTED);
  drawWifiSignal(x + 2, y + 11, connected);
  tft.setTextColor(C_DIM, C_BG); tft.setTextSize(1);
  char buf[24] = "no data";
  if (lastSuccessfulUpdate > 0) {
    unsigned long ago = (millis() - lastSuccessfulUpdate) / 1000;
    if      (ago < 60)   snprintf(buf, sizeof(buf), "live");
    else if (ago < 3600) snprintf(buf, sizeof(buf), "%lum ago", ago / 60);
    else                 snprintf(buf, sizeof(buf), ">1h ago");
  }
  tft.setTextDatum(ML_DATUM);
  tft.drawString(buf, x + 14, y + 7);
  int leftEdge = x + 14 + tft.textWidth(buf);

  String clockStr = nowHHMMSS().substring(0, 5);
  int rightEdge = x + w - 3 - tft.textWidth(clockStr);

  // Only flagged when NOT the default public backend, so the common case
  // stays uncluttered — this is purely "am I actually hitting my node?".
  // Shows the configured host itself (scheme stripped for brevity),
  // truncated to whatever room is free between the two labels either side.
  if (!g_mempoolHost.equalsIgnoreCase(DEFAULT_MEMPOOL_HOST)) {
    String host = g_mempoolHost;
    if      (host.startsWith("https://")) host.remove(0, 8);
    else if (host.startsWith("http://"))  host.remove(0, 7);

    int gapW = rightEdge - leftEdge - 12;
    if (gapW > 12) {
      tft.setTextColor(C_BLUE, C_BG);
      tft.setTextDatum(MC_DATUM);
      while (host.length() > 1 && tft.textWidth(host) > gapW) host.remove(host.length() - 1);
      tft.drawString(host, (leftEdge + rightEdge) / 2, y + 7);
    }
  }

  tft.setTextColor(C_DIM, C_BG);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(clockStr, x + w - 3, y + 7);
  tft.setTextDatum(TL_DATUM);
}

// ═════════════════════════════════════════════════════════════════════════════
//  FORMATTING UTILITIES
// ═════════════════════════════════════════════════════════════════════════════
String fmtUSD(float v) {
  if (v <= 0) return "--";
  long w = lroundf(v);
  char buf[24];
  if      (w >= 1000000) snprintf(buf, sizeof(buf), "$%ld,%03ld,%03ld", w/1000000, (w/1000)%1000, w%1000);
  else if (w >= 1000)    snprintf(buf, sizeof(buf), "$%ld,%03ld",       w/1000,    w%1000);
  else                   snprintf(buf, sizeof(buf), "$%ld",              w);
  return String(buf);
}

String fmtLocal(float v) {
  if (v <= 0) return "--";
  long w = lroundf(v);
  char num[24];
  if      (w >= 1000000) snprintf(num, sizeof(num), "%ld,%03ld,%03ld", w/1000000, (w/1000)%1000, w%1000);
  else if (w >= 1000)    snprintf(num, sizeof(num), "%ld,%03ld",       w/1000,    w%1000);
  else                   snprintf(num, sizeof(num), "%ld",              w);
  char buf[32];
  if      (g_currency == "BRL") snprintf(buf, sizeof(buf), "R$%s",          num);
  else if (g_currency == "USD") snprintf(buf, sizeof(buf), "$%s",           num);
  else                          snprintf(buf, sizeof(buf), "%s %s", g_currency.c_str(), num);
  return String(buf);
}

String fmtBlockNum(uint32_t h) {
  if (h == 0) return "--";
  char buf[16];
  if      (h >= 1000000) snprintf(buf, sizeof(buf), "%lu,%03lu,%03lu", h/1000000UL, (h/1000UL)%1000UL, h%1000UL);
  else if (h >= 1000)    snprintf(buf, sizeof(buf), "%lu,%03lu",       h/1000UL,    h%1000UL);
  else                   snprintf(buf, sizeof(buf), "%lu",              (unsigned long)h);
  return String(buf);
}

String fmtHashrate(float eh) {
  if (eh <= 0) return "--";
  char buf[20];
  if (eh >= 1000) snprintf(buf, sizeof(buf), "%.1f ZH/s", eh / 1000.0f);
  else            snprintf(buf, sizeof(buf), "%.0f EH/s",  eh);
  return String(buf);
}

String fmtSats(int64_t sats) {
  if (sats == 0) return "0 sats";
  bool neg = (sats < 0);
  uint64_t v = neg ? (uint64_t)(-sats) : (uint64_t)sats;
  char num[32];
  if (v >= 1000000000000ULL)
    snprintf(num, sizeof(num), "%llu,%03llu,%03llu,%03llu,%03llu",
      v/1000000000000ULL, (v/1000000000ULL)%1000ULL,
      (v/1000000ULL)%1000ULL, (v/1000ULL)%1000ULL, v%1000ULL);
  else if (v >= 1000000000ULL)
    snprintf(num, sizeof(num), "%llu,%03llu,%03llu,%03llu",
      v/1000000000ULL, (v/1000000ULL)%1000ULL,
      (v/1000ULL)%1000ULL, v%1000ULL);
  else if (v >= 1000000ULL)
    snprintf(num, sizeof(num), "%llu,%03llu,%03llu",
      v/1000000ULL, (v/1000ULL)%1000ULL, v%1000ULL);
  else if (v >= 1000ULL)
    snprintf(num, sizeof(num), "%llu,%03llu", v/1000ULL, v%1000ULL);
  else
    snprintf(num, sizeof(num), "%llu", v);
  char buf[48];
  snprintf(buf, sizeof(buf), "%s%s sats", neg ? "-" : "", num);
  return String(buf);
}

String lastBlockAge() {
  if (g_lastBlockTime == 0) return "--";
  time_t now; time(&now);
  long ago = (long)(now - (time_t)g_lastBlockTime);
  if (ago < 0) ago = 0;
  if (ago < 60)  { char b[20]; snprintf(b, sizeof(b), "%ld sec ago", ago);      return String(b); }
  long mins = ago / 60;
  if (mins < 60) { char b[20]; snprintf(b, sizeof(b), "%ld min ago", mins);     return String(b); }
  char b[20]; snprintf(b, sizeof(b), "%ld h ago", mins / 60); return String(b);
}

float calcCirculating(uint32_t height) {
  float supply = 0;
  float reward = 50.0f;
  uint32_t rem = height;
  while (rem > 0 && reward >= 1e-8f) {
    uint32_t ep = (rem > 210000u) ? 210000u : rem;
    supply += (float)ep * reward;
    rem -= ep;
    reward *= 0.5f;
  }
  return supply;
}

uint32_t calcNextHalving(uint32_t height) {
  uint32_t next = ((height / 210000u) + 1u) * 210000u;
  return (next > height) ? (next - height) : 0u;
}

uint32_t daysSinceGenesis() {
  time_t now; time(&now);
  if (now < 1231006505L) return 0;
  return (uint32_t)((now - 1231006505L) / 86400L);
}

uint16_t feeColor(uint8_t s) {
  if (s >= 50) return C_RED;
  if (s >= 20) return C_ORANGE;
  if (s >= 8)  return C_YELLOW;
  return C_GREEN;
}

uint16_t fngColor(uint8_t v) {
  if (v <= 24) return C_RED;
  if (v <= 44) return C_ORANGE;
  if (v <= 55) return C_YELLOW;
  if (v <= 74) return tft.color565(100, 200, 50);
  return C_GREEN;
}

// ═════════════════════════════════════════════════════════════════════════════
//  HTTP / APIs
// ═════════════════════════════════════════════════════════════════════════════
bool fetchBTCUSD(float &out) {
  WiFiClientSecure cl; cl.setInsecure();
  HTTPClient http; http.setTimeout(8000);
  http.setUserAgent(g_userAgent);

  if (http.begin(cl, BTC_PRIMARY) && http.GET() == HTTP_CODE_OK) {
    JsonDocument doc;
    String body;
    bool bodyOk = httpReadBody(http, body);
    http.end();
    if (bodyOk && !deserializeJson(doc, body)) {
      JsonVariant bid = doc["BTCUSD"]["bid"];
      if (bid.is<const char*>()) { out = atof(bid.as<const char*>()); return true; }
      if (bid.is<float>())       { out = bid.as<float>();              return true; }
    }
  } else { http.end(); }

  if (http.begin(cl, BTC_FALLBACK) && http.GET() == HTTP_CODE_OK) {
    JsonDocument doc;
    String body;
    bool bodyOk = httpReadBody(http, body);
    http.end();
    if (bodyOk && !deserializeJson(doc, body)) {
      JsonVariant amt = doc["data"]["amount"];
      if (amt.is<const char*>()) { out = atof(amt.as<const char*>()); return true; }
      if (amt.is<float>())       { out = amt.as<float>();              return true; }
    }
  } else { http.end(); }
  return false;
}

bool fetchBTCLocal(float &out) {
  if (g_currency == "USD") { out = btcUsd; return (btcUsd > 0); }
  WiFiClientSecure cl; cl.setInsecure();
  HTTPClient http; http.setTimeout(8000);
  http.setUserAgent(g_userAgent);

  String url1 = String(AWESOME_BASE) + g_currency;
  if (http.begin(cl, url1) && http.GET() == HTTP_CODE_OK) {
    JsonDocument doc;
    String body;
    bool bodyOk = httpReadBody(http, body);
    http.end();
    if (bodyOk && !deserializeJson(doc, body)) {
      String key = "BTC" + g_currency;
      JsonVariant bid = doc[key.c_str()]["bid"];
      if (bid.is<const char*>()) { out = atof(bid.as<const char*>()); return true; }
      if (bid.is<float>())       { out = bid.as<float>();              return true; }
    }
  } else { http.end(); }

  String url2 = String(COINBASE_BASE) + g_currency + "/spot";
  if (http.begin(cl, url2) && http.GET() == HTTP_CODE_OK) {
    JsonDocument doc;
    String body;
    bool bodyOk = httpReadBody(http, body);
    http.end();
    if (bodyOk && !deserializeJson(doc, body)) {
      JsonVariant amt = doc["data"]["amount"];
      if (amt.is<const char*>()) { out = atof(amt.as<const char*>()); return true; }
      if (amt.is<float>())       { out = amt.as<float>();              return true; }
    }
  } else { http.end(); }
  return false;
}

bool fetchFees() {
  WiFiClientSecure secureCl; WiFiClient plainCl;
  HTTPClient http; http.setTimeout(8000);
  http.setUserAgent(g_userAgent);
  String url = mempoolBase() + API_FEES_PATH;
  if (!httpBeginMempool(http, secureCl, plainCl, url)) return false;
  if (http.GET() != HTTP_CODE_OK) { http.end(); return false; }
  JsonDocument doc;
  String body;
  if (!httpReadBody(http, body)) { http.end(); return false; }
  http.end();
  if (deserializeJson(doc, body)) return false;
  g_feesFast = doc["fastestFee"].as<uint8_t>();
  g_feesHalf = doc["halfHourFee"].as<uint8_t>();
  g_feesHour = doc["hourFee"].as<uint8_t>();
  g_feesEcon = doc["economyFee"].as<uint8_t>();
  return true;
}

bool fetchNetworkData() {
  bool gotDiff = false, gotBlock = false;

  // Difficulty-adjustment endpoint — retarget progress only, no block/hashrate data.
  {
    WiFiClientSecure secureCl; WiFiClient plainCl;
    HTTPClient http; http.setTimeout(10000);
    http.setUserAgent(g_userAgent);
    String url = mempoolBase() + API_DIFF_PATH;
    if (httpBeginMempool(http, secureCl, plainCl, url) && http.GET() == HTTP_CODE_OK) {
      JsonDocument doc;
      String body;
      bool bodyOk = httpReadBody(http, body);
      http.end();
      if (bodyOk && !deserializeJson(doc, body)) {
        g_diffChange     = doc["difficultyChange"].as<float>();
        g_epochProgress  = doc["progressPercent"].as<float>();
        g_retargetBlocks = doc["remainingBlocks"].as<uint32_t>();
        gotDiff = true;
      }
    } else { http.end(); }
  }

  // Latest-blocks endpoint — [0] is the tip: gives height, time, and
  // difficulty (used to derive hashrate), none of which are in API_DIFF_PATH.
  {
    WiFiClientSecure secureCl; WiFiClient plainCl;
    HTTPClient http; http.setTimeout(10000);
    http.setUserAgent(g_userAgent);
    String url = mempoolBase() + API_BLOCKS_PATH;
    if (httpBeginMempool(http, secureCl, plainCl, url) && http.GET() == HTTP_CODE_OK) {
      JsonDocument doc;
      String body;
      bool bodyOk = httpReadBody(http, body);
      http.end();
      if (bodyOk && !deserializeJson(doc, body) && doc.is<JsonArray>() && doc.size() > 0) {
        g_blockHeight   = doc[0]["height"].as<uint32_t>();
        g_lastBlockTime = doc[0]["timestamp"].as<uint32_t>();
        double diff = doc[0]["difficulty"].as<double>();
        if (diff > 0) g_hashrate = (float)(diff * 4294967296.0 / 600.0 / 1e18);
        gotBlock = true;
      }
    } else { http.end(); }
  }

  return gotDiff || gotBlock;
}

bool fetchFearGreed() {
  WiFiClientSecure cl; cl.setInsecure();
  HTTPClient http; http.setTimeout(8000);
  http.setUserAgent(g_userAgent);
  if (!http.begin(cl, API_FNG)) return false;
  if (http.GET() != HTTP_CODE_OK) { http.end(); return false; }
  JsonDocument doc;
  String body;
  if (!httpReadBody(http, body)) { http.end(); return false; }
  http.end();
  if (deserializeJson(doc, body)) return false;
  g_fng = doc["data"][0]["value"].as<uint8_t>();
  const char* lbl = doc["data"][0]["value_classification"].as<const char*>();
  if (lbl) strncpy(g_fngLabel, lbl, sizeof(g_fngLabel) - 1);
  return true;
}

bool fetchAddress() {
  if (g_watchAddr.length() < 10) return false;
  WiFiClientSecure secureCl; WiFiClient plainCl;
  HTTPClient http; http.setTimeout(10000);
  http.setUserAgent(g_userAgent);
  String url = mempoolBase() + API_ADDR_PATH + g_watchAddr;
  if (!httpBeginMempool(http, secureCl, plainCl, url)) return false;
  if (http.GET() != HTTP_CODE_OK) { http.end(); return false; }
  JsonDocument doc;
  String body;
  if (!httpReadBody(http, body)) { http.end(); return false; }
  http.end();
  if (deserializeJson(doc, body)) return false;
  int64_t funded   = doc["chain_stats"]["funded_txo_sum"].as<int64_t>();
  int64_t spent    = doc["chain_stats"]["spent_txo_sum"].as<int64_t>();
  int64_t mFunded  = doc["mempool_stats"]["funded_txo_sum"].as<int64_t>();
  int64_t mSpent   = doc["mempool_stats"]["spent_txo_sum"].as<int64_t>();
  g_addrBalance    = funded - spent;
  g_addrUnconfirmed = mFunded - mSpent;
  g_addrFetched    = true;
  return true;
}

bool fetchLightning() {
  WiFiClientSecure secureCl; WiFiClient plainCl;
  HTTPClient http; http.setTimeout(10000);
  http.setUserAgent(g_userAgent);
  String url = mempoolBase() + API_LN_PATH;
  if (!httpBeginMempool(http, secureCl, plainCl, url)) return false;
  if (http.GET() != HTTP_CODE_OK) { http.end(); return false; }
  JsonDocument doc;
  String body;
  if (!httpReadBody(http, body)) { http.end(); return false; }
  http.end();
  if (deserializeJson(doc, body)) return false;
  JsonVariant latest = doc["latest"];
  g_lnCapacitySats  = latest["total_capacity"].as<double>();
  g_lnChannels      = latest["channel_count"].as<uint32_t>();
  g_lnNodes         = latest["node_count"].as<uint32_t>();
  g_lnAvgCapacity   = latest["avg_capacity"].as<double>();
  g_lnTorNodes      = latest["tor_nodes"].as<uint32_t>();
  g_lnClearnetNodes = latest["clearnet_nodes"].as<uint32_t>();
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  SCREEN DISPATCH
// ═════════════════════════════════════════════════════════════════════════════
void redrawScreen() {
  drawBorder();
  switch (curScreen) {
    case 0: (g_orient == 0) ? drawScreen0_portrait() : drawScreen0_landscape(); break;
    case 1: drawScreen1_network();  break;
    case 2: drawScreen2_halving();  break;
    case 3: drawScreen3_fees();     break;
    case 4: drawScreen4_address();  break;
    case 5: drawScreen5_clock();    break;
    case 6: drawScreen6_lightning(); break;
  }
  drawPageDots();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Screen 0 — Price · Portrait (240 × 320)
// ─────────────────────────────────────────────────────────────────────────────
void drawScreen0_portrait() {
  int W = tft.width(), cx = W / 2;
  tft.setTextDatum(TC_DATUM);

  // Clock + date
  tft.fillRect(L_BORDER+1, L_CLOCK_Y, W-2*(L_BORDER+1), L_SEP1_Y - L_CLOCK_Y, C_BG);
  tft.setTextColor(TFT_WHITE, C_BG); tft.setTextSize(3);
  tft.drawString(nowHHMMSS(), cx, L_CLOCK_Y);
  tft.setTextColor(C_LABEL, C_BG); tft.setTextSize(1);
  tft.drawString(nowDate(), cx, L_DATE_Y);
  hRule(L_BORDER+4, L_SEP1_Y, W-2*(L_BORDER+4));

  // BTC / USD
  tft.fillRect(L_BORDER+1, L_LBL_BTC_Y, W-2*(L_BORDER+1), L_SEP2_Y - L_LBL_BTC_Y, C_BG);
  sectionHeader(L_BORDER+4, L_LBL_BTC_Y, "BTC / USD");
  tft.setTextColor(TFT_WHITE, C_BG); tft.setTextSize(3);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(fmtUSD(btcUsd), cx, L_PRICE_Y);
  drawChangeIndicator(L_BORDER+1, L_CHANGE_Y, W-2*(L_BORDER+1), btcUsd, btcUsdPrev);
  hRule(L_BORDER+4, L_SEP2_Y, W-2*(L_BORDER+4));

  // BTC / local
  tft.fillRect(L_BORDER+1, L_LBL_LOC_Y, W-2*(L_BORDER+1), L_SPARK_Y - L_LBL_LOC_Y, C_BG);
  sectionHeader(L_BORDER+4, L_LBL_LOC_Y, ("BTC / " + g_currency).c_str(), C_GOLDEN);
  tft.setTextColor(C_GOLDEN, C_BG); tft.setTextSize(2);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(fmtLocal(btcLocal), cx, L_LOCAL_Y);

  // Sparkline in card
  drawSparkline(L_BORDER+2, L_SPARK_Y, W-2*(L_BORDER+2), L_SPARK_H);
  hRule(L_BORDER+4, L_SEP3_Y, W-2*(L_BORDER+4));
  drawStatusBar(L_BORDER+1, L_STATUS_Y, W-2*(L_BORDER+1));
  tft.setTextDatum(TL_DATUM);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Screen 0 — Price · Landscape (320 × 240)
// ─────────────────────────────────────────────────────────────────────────────
void drawScreen0_landscape() {
  int W = tft.width(), H = tft.height(), cx = W / 2;
  tft.setTextDatum(TC_DATUM);

  // Header full width
  tft.fillRect(L_BORDER+1, L_CLOCK_Y, W-2*(L_BORDER+1), LL_SEP_H - L_CLOCK_Y, C_BG);
  tft.setTextColor(TFT_WHITE, C_BG); tft.setTextSize(3);
  tft.drawString(nowHHMMSS(), cx, L_CLOCK_Y);
  tft.setTextColor(C_LABEL, C_BG); tft.setTextSize(1);
  tft.drawString(nowDate(), cx, L_DATE_Y);
  hRule(LL_BORDER+4, LL_SEP_H, W-2*(LL_BORDER+4));

  // Vertical divider — stops at the footer separator, so it doesn't run
  // down through the status bar (it used to reach H-LL_BORDER-1, passing
  // right behind the backend-host text drawn there).
  tft.drawLine(LL_DIV_X, LL_SEP_H+1, LL_DIV_X, LL_FOOT_SEP_Y - 1, C_DIM);

  // Left column — prices
  tft.fillRect(LL_LCOL_X, LL_LBL_BTC_Y, LL_LCOL_W, LL_FOOT_SEP_Y - LL_LBL_BTC_Y, C_BG);
  sectionHeader(LL_LCOL_X+2, LL_LBL_BTC_Y, "BTC / USD");
  tft.setTextColor(TFT_WHITE, C_BG); tft.setTextSize(2);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(fmtUSD(btcUsd), LL_LCOL_CX, LL_PRICE_Y);
  drawChangeIndicator(LL_LCOL_X, LL_CHANGE_Y, LL_LCOL_W, btcUsd, btcUsdPrev);
  hRule(LL_LCOL_X+4, LL_SEP2_Y, LL_DIV_X - LL_LCOL_X - 8);
  sectionHeader(LL_LCOL_X+2, LL_LBL_LOC_Y, ("BTC / " + g_currency).c_str(), C_GOLDEN);
  tft.setTextColor(C_GOLDEN, C_BG); tft.setTextSize(2);
  tft.drawString(fmtLocal(btcLocal), LL_LCOL_CX, LL_LOCAL_Y);

  // Right column — sparkline
  drawSparkline(LL_RCOL_X, LL_LBL_BTC_Y, LL_RCOL_W, LL_FOOT_SEP_Y - LL_LBL_BTC_Y);

  // Footer
  hRule(LL_BORDER+4, LL_FOOT_SEP_Y, W-2*(LL_BORDER+4));
  drawStatusBar(LL_BORDER+1, LL_STATUS_Y, W-2*(LL_BORDER+1));
  tft.setTextDatum(TL_DATUM);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Screen 1 — Network
// ─────────────────────────────────────────────────────────────────────────────
void drawScreen1_network() {
  int W = tft.width(), cx = W / 2;
  int y = L_BORDER + 6;
  tft.setTextDatum(TC_DATUM);

  sectionHeader(L_BORDER+4, y, "BLOCK HEIGHT"); y += 12;
  if (g_blockHeight > 0) {
    tft.fillRect(L_BORDER+2, y-2, W-2*(L_BORDER+2), 28, C_BG);
    tft.setTextColor(C_ORANGE, C_BG); tft.setTextSize(3);
    tft.drawString(fmtBlockNum(g_blockHeight), cx, y);
  } else {
    tft.setTextColor(C_DIM, C_BG); tft.setTextSize(2);
    tft.drawString("--", cx, y);
  }
  y += 28;

  hRule(L_BORDER+4, y, W-2*(L_BORDER+4)); y += 6;

  sectionHeader(L_BORDER+4, y, "HASHRATE", C_BLUE); y += 12;
  tft.setTextColor(C_BLUE, C_BG); tft.setTextSize(2);
  tft.drawString(fmtHashrate(g_hashrate), cx, y); y += 20;

  hRule(L_BORDER+4, y, W-2*(L_BORDER+4)); y += 6;

  sectionHeader(L_BORDER+4, y, "DIFFICULTY RETARGET"); y += 12;
  {
    char buf[48];
    snprintf(buf, sizeof(buf), "%lu blocks to retarget", (unsigned long)g_retargetBlocks);
    tft.setTextColor(TFT_WHITE, C_BG); tft.setTextSize(1);
    tft.drawString(buf, cx, y); y += 12;
    progressBar(L_BORDER+6, y, W-2*(L_BORDER+6), 10, g_epochProgress, C_ORANGE); y += 14;
    uint16_t dc = (g_diffChange >= 0) ? C_GREEN : C_RED;
    snprintf(buf, sizeof(buf), "exp. %+.1f%%   %.0f%% epoch", g_diffChange, g_epochProgress);
    tft.setTextColor(dc, C_BG); tft.setTextSize(1);
    tft.drawString(buf, cx, y); y += 12;
  }

  hRule(L_BORDER+4, y, W-2*(L_BORDER+4)); y += 6;

  sectionHeader(L_BORDER+4, y, "LAST BLOCK"); y += 12;
  tft.setTextColor(TFT_WHITE, C_BG); tft.setTextSize(2);
  tft.drawString(lastBlockAge(), cx, y); y += 20;

  // Session H/L from sparkline — folded directly under Last Block with no
  // header/rule of its own, to keep this screen's total height well clear
  // of the status bar.
  if (histCount >= 2) {
    float hi = priceHist[(histHead - histCount + SPARKLINE_POINTS) % SPARKLINE_POINTS];
    float lo = hi;
    for (int i = 1; i < histCount; i++) {
      float v = priceHist[(histHead - histCount + i + SPARKLINE_POINTS) % SPARKLINE_POINTS];
      if (v > hi) hi = v; if (v < lo) lo = v;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "H %s", fmtUSD(hi).c_str());
    tft.setTextColor(C_GREEN, C_BG); tft.setTextSize(1);
    tft.drawString(buf, cx - 40, y);
    snprintf(buf, sizeof(buf), "L %s", fmtUSD(lo).c_str());
    tft.setTextColor(C_RED, C_BG);
    tft.drawString(buf, cx + 40, y);
  }

  int statusY = (g_orient == 0) ? L_STATUS_Y : LL_STATUS_Y;
  hRule(L_BORDER+4, statusY - 4, W-2*(L_BORDER+4));
  drawStatusBar(L_BORDER+1, statusY, W-2*(L_BORDER+1));
  tft.setTextDatum(TL_DATUM);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Screen 2 — Halving + 21M supply
// ─────────────────────────────────────────────────────────────────────────────
void drawScreen2_halving() {
  int W = tft.width(), cx = W / 2;
  int y = L_BORDER + 6;
  tft.setTextDatum(TC_DATUM);

  sectionHeader(L_BORDER+4, y, "HALVING COUNTDOWN"); y += 13;

  uint32_t halvBlocks = (g_blockHeight > 0) ? calcNextHalving(g_blockHeight) : 0;
  if (halvBlocks > 0) {
    tft.fillRect(L_BORDER+2, y-2, W-2*(L_BORDER+2), 32, C_BG);
    tft.setTextColor(C_ORANGE, C_BG); tft.setTextSize(3);
    tft.drawString(fmtBlockNum(halvBlocks), cx, y); y += 28;
    // Epoch progress
    uint32_t epochStart = (g_blockHeight / 210000u) * 210000u;
    float ep = g_blockHeight > 0 ? ((float)(g_blockHeight - epochStart) / 210000.0f * 100.0f) : 0;
    progressBar(L_BORDER+6, y, W-2*(L_BORDER+6), 12, ep, C_ORANGE); y += 16;
    char buf[40];
    snprintf(buf, sizeof(buf), "%.1f%% of epoch mined", ep);
    tft.setTextColor(C_LABEL, C_BG); tft.setTextSize(1);
    tft.drawString(buf, cx, y); y += 14;
    // Estimate: ~10 min per block
    uint32_t days = halvBlocks / 144;
    snprintf(buf, sizeof(buf), "~%lu days remaining", (unsigned long)days);
    tft.setTextColor(C_DIM, C_BG); tft.setTextSize(1);
    tft.drawString(buf, cx, y); y += 12;
  } else {
    tft.setTextColor(C_DIM, C_BG); tft.setTextSize(1);
    tft.drawString("awaiting block data...", cx, y); y += 20;
  }

  hRule(L_BORDER+4, y, W-2*(L_BORDER+4)); y += 8;

  sectionHeader(L_BORDER+4, y, "21 MILLION", C_GOLDEN); y += 13;
  float circ = (g_blockHeight > 0) ? calcCirculating(g_blockHeight) : 0;
  float pct21 = circ / 21000000.0f * 100.0f;
  {
    char buf[40];
    snprintf(buf, sizeof(buf), "%.4fM / 21M BTC", circ / 1e6f);
    tft.setTextColor(C_GOLDEN, C_BG); tft.setTextSize(1);
    tft.drawString(buf, cx, y); y += 12;
    progressBar(L_BORDER+6, y, W-2*(L_BORDER+6), 12, pct21, C_GOLDEN); y += 16;
    snprintf(buf, sizeof(buf), "%.4f%% mined  %.4fM remaining", pct21, (21.0f - circ / 1e6f));
    tft.setTextColor(C_DIM, C_BG); tft.setTextSize(1);
    tft.drawString(buf, cx, y); y += 14;
  }

  hRule(L_BORDER+4, y, W-2*(L_BORDER+4)); y += 8;

  sectionHeader(L_BORDER+4, y, "CURRENT EPOCH"); y += 12;
  {
    uint32_t ep = g_blockHeight / 210000u;
    float reward = 50.0f;
    for (uint32_t i = 0; i < ep; i++) reward *= 0.5f;
    char buf[40];
    snprintf(buf, sizeof(buf), "%.4f BTC / block", reward);
    tft.setTextColor(TFT_WHITE, C_BG); tft.setTextSize(1);
    tft.drawString(buf, cx, y); y += 12;
    snprintf(buf, sizeof(buf), "epoch #%lu - next halving %s",
             (unsigned long)ep + 1, fmtBlockNum((ep + 1) * 210000u).c_str());
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString(buf, cx, y);
  }

  int statusY = (g_orient == 0) ? L_STATUS_Y : LL_STATUS_Y;
  hRule(L_BORDER+4, statusY - 4, W-2*(L_BORDER+4));
  drawStatusBar(L_BORDER+1, statusY, W-2*(L_BORDER+1));
  tft.setTextDatum(TL_DATUM);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Screen 3 — Mempool fees
// ─────────────────────────────────────────────────────────────────────────────
void drawScreen3_fees() {
  int W = tft.width(), cx = W / 2;
  int y = L_BORDER + 6;
  tft.setTextDatum(TC_DATUM);

  sectionHeader(L_BORDER+4, y, "MEMPOOL FEES  (sat/vB)"); y += 13;

  // Draw 4 fee bars
  struct { const char* lbl; uint8_t val; } tiers[] = {
    { "NEXT BLOCK", g_feesFast },
    { "30 MINUTES", g_feesHalf },
    { "1 HOUR",     g_feesHour },
    { "ECONOMY",    g_feesEcon },
  };
  uint8_t maxFee = max(max(g_feesFast, g_feesHalf), max(g_feesHour, g_feesEcon));
  if (maxFee == 0) maxFee = 1;

  int barW = W - 2*(L_BORDER + 6);
  for (int i = 0; i < 4; i++) {
    uint8_t v = tiers[i].val;
    uint16_t col = feeColor(v);
    float pct = (float)v / (float)maxFee * 100.0f;

    tft.setTextColor(C_LABEL, C_BG); tft.setTextSize(1);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(tiers[i].lbl, L_BORDER+6, y + 4);

    char buf[12];
    snprintf(buf, sizeof(buf), "%d s/vB", v);
    tft.setTextColor(col, C_BG);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(buf, W - L_BORDER - 4, y + 4); y += 10;

    progressBar(L_BORDER+6, y, barW, 14, pct, col); y += 16;
    tft.setTextDatum(TC_DATUM);
  }

  hRule(L_BORDER+4, y, W-2*(L_BORDER+4)); y += 6;

  sectionHeader(L_BORDER+4, y, "BLOCK HEIGHT"); y += 12;
  tft.setTextColor(C_ORANGE, C_BG); tft.setTextSize(2);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(fmtBlockNum(g_blockHeight), cx, y); y += 20;

  hRule(L_BORDER+4, y, W-2*(L_BORDER+4)); y += 6;
  sectionHeader(L_BORDER+4, y, "SATS PER DOLLAR"); y += 12;
  if (btcUsd > 0) {
    uint32_t spd = (uint32_t)(100000000.0f / btcUsd);
    char buf[20];
    snprintf(buf, sizeof(buf), "%lu sats", (unsigned long)spd);
    tft.setTextColor(TFT_WHITE, C_BG); tft.setTextSize(2);
    tft.setTextDatum(TC_DATUM);
    tft.drawString(buf, cx, y);
  }

  int statusY = (g_orient == 0) ? L_STATUS_Y : LL_STATUS_Y;
  hRule(L_BORDER+4, statusY - 4, W-2*(L_BORDER+4));
  drawStatusBar(L_BORDER+1, statusY, W-2*(L_BORDER+1));
  tft.setTextDatum(TL_DATUM);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Screen 4 — Watch address
// ─────────────────────────────────────────────────────────────────────────────
void drawScreen4_address() {
  int W = tft.width(), cx = W / 2;
  int y = L_BORDER + 6;
  tft.setTextDatum(TC_DATUM);

  sectionHeader(L_BORDER+4, y, "ADDRESS WATCH"); y += 13;

  if (g_watchAddr.length() == 0) {
    tft.fillRoundRect(L_BORDER+4, y, W-2*(L_BORDER+4), 60, 4, C_CARD);
    tft.setTextColor(C_LABEL, C_CARD); tft.setTextSize(1);
    tft.drawString("No address configured.", cx, y + 12);
    tft.drawString("Open portal & set", cx, y + 24);
    tft.drawString("watch address.", cx, y + 36);
    int statusY = (g_orient == 0) ? L_STATUS_Y : LL_STATUS_Y;
    hRule(L_BORDER+4, statusY - 4, W-2*(L_BORDER+4));
    drawStatusBar(L_BORDER+1, statusY, W-2*(L_BORDER+1));
    tft.setTextDatum(TL_DATUM);
    return;
  }

  // Truncated address
  String addr = g_watchAddr;
  String disp = (addr.length() > 22)
    ? addr.substring(0, 10) + ".." + addr.substring(addr.length() - 10)
    : addr;
  tft.setTextColor(C_DIM, C_BG); tft.setTextSize(1);
  tft.drawString(disp, cx, y); y += 14;

  hRule(L_BORDER+4, y, W-2*(L_BORDER+4)); y += 8;

  sectionHeader(L_BORDER+4, y, "CONFIRMED BALANCE", C_GOLDEN); y += 13;
  if (g_addrFetched) {
    // BTC value
    double btcVal = (double)g_addrBalance / 1e8;
    char buf[32];
    snprintf(buf, sizeof(buf), "%.8f BTC", btcVal);
    tft.fillRect(L_BORDER+2, y-2, W-2*(L_BORDER+2), 24, C_BG);
    tft.setTextColor(C_GOLDEN, C_BG); tft.setTextSize(2);
    tft.drawString(buf, cx, y); y += 22;

    // Sats
    tft.setTextColor(TFT_WHITE, C_BG); tft.setTextSize(1);
    tft.drawString(fmtSats(g_addrBalance), cx, y); y += 14;

    // USD equivalent
    if (btcUsd > 0) {
      float usdVal = (float)g_addrBalance / 1e8f * btcUsd;
      snprintf(buf, sizeof(buf), "~%s USD", fmtUSD(usdVal).c_str());
      tft.setTextColor(C_LABEL, C_BG); tft.setTextSize(1);
      tft.drawString(buf, cx, y); y += 14;
    }

    hRule(L_BORDER+4, y, W-2*(L_BORDER+4)); y += 8;

    sectionHeader(L_BORDER+4, y, "UNCONFIRMED"); y += 12;
    uint16_t uc = (g_addrUnconfirmed > 0) ? C_GREEN : (g_addrUnconfirmed < 0 ? C_RED : C_DIM);
    tft.setTextColor(uc, C_BG); tft.setTextSize(1);
    char sign = (g_addrUnconfirmed >= 0) ? '+' : '-';
    int64_t absU = (g_addrUnconfirmed < 0) ? -g_addrUnconfirmed : g_addrUnconfirmed;
    String uStr = String(sign) + fmtSats(absU);
    tft.drawString(uStr, cx, y); y += 14;

  } else {
    tft.setTextColor(C_DIM, C_BG); tft.setTextSize(1);
    tft.drawString("fetching...", cx, y); y += 14;
  }

  int statusY = (g_orient == 0) ? L_STATUS_Y : LL_STATUS_Y;
  hRule(L_BORDER+4, statusY - 4, W-2*(L_BORDER+4));
  drawStatusBar(L_BORDER+1, statusY, W-2*(L_BORDER+1));
  tft.setTextDatum(TL_DATUM);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Screen 5 — Clock + Fear & Greed + Genesis
// ─────────────────────────────────────────────────────────────────────────────
void drawScreen5_clock() {
  int W = tft.width(), H = tft.height(), cx = W / 2;
  tft.setTextDatum(TC_DATUM);

  // Large clock (HH:MM) — offsets below are tightened from the original
  // (clockY 10, etc.) to buy back headroom above the status bar; every
  // gap still keeps its intended clearance, just trimmed to the minimum.
  int clockY = 8;
  tft.fillRect(L_BORDER+1, clockY, W-2*(L_BORDER+1), 34, C_BG);
  tft.setTextColor(TFT_WHITE, C_BG); tft.setTextSize(4);
  tft.drawString(nowHHMMSS().substring(0, 5), cx, clockY);

  // Date
  tft.setTextColor(C_LABEL, C_BG); tft.setTextSize(1);
  tft.drawString(nowDate(), cx, 46);

  // BTC price
  tft.setTextColor(C_ORANGE, C_BG); tft.setTextSize(2);
  tft.drawString(fmtUSD(btcUsd), cx, 60);

  // Sats per dollar
  if (btcUsd > 0) {
    uint32_t spd = (uint32_t)(100000000.0f / btcUsd);
    char buf[24]; snprintf(buf, sizeof(buf), "%lu sats/$", (unsigned long)spd);
    tft.setTextColor(C_DIM, C_BG); tft.setTextSize(1);
    tft.drawString(buf, cx, 80);
  }

  hRule(L_BORDER+4, 92, W-2*(L_BORDER+4));

  // Fear & Greed
  sectionHeader(L_BORDER+4, 96, "FEAR & GREED INDEX");
  if (g_fng > 0) {
    uint16_t fc = fngColor(g_fng);
    tft.setTextColor(fc, C_BG); tft.setTextSize(3);
    char buf[8]; snprintf(buf, sizeof(buf), "%d", g_fng);
    tft.drawString(buf, cx - 20, 108);
    tft.setTextSize(1);
    tft.drawString(g_fngLabel, cx + 30, 116);
    progressBar(L_BORDER+6, 134, W-2*(L_BORDER+6), 10, (float)g_fng, fc);
    // Labels for the bar
    tft.setTextColor(C_DIM, C_BG); tft.setTextSize(1);
    tft.setTextDatum(ML_DATUM); tft.drawString("Fear", L_BORDER+6, 149);
    tft.setTextDatum(MR_DATUM); tft.drawString("Greed", W-L_BORDER-6, 149);
    tft.setTextDatum(TC_DATUM);
  } else {
    tft.setTextColor(C_DIM, C_BG); tft.setTextSize(1);
    tft.drawString("--", cx, 112);
  }

  hRule(L_BORDER+4, 157, W-2*(L_BORDER+4));

  // Days since genesis
  sectionHeader(L_BORDER+4, 161, "GENESIS BLOCK");
  uint32_t days = daysSinceGenesis();
  if (days > 0) {
    char buf[40];
    uint32_t years = days / 365;
    uint32_t rem   = days % 365;
    snprintf(buf, sizeof(buf), "%lu days  (%lu yr %lu d)", (unsigned long)days, (unsigned long)years, (unsigned long)rem);
    tft.setTextColor(TFT_WHITE, C_BG); tft.setTextSize(1);
    tft.drawString(buf, cx, 173);
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString("Jan 03, 2009  block #0", cx, 185);
  }

  int statusY = (g_orient == 0) ? L_STATUS_Y : LL_STATUS_Y;
  hRule(L_BORDER+4, statusY - 4, W-2*(L_BORDER+4));
  drawStatusBar(L_BORDER+1, statusY, W-2*(L_BORDER+1));
  tft.setTextDatum(TL_DATUM);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Screen 6 — Lightning Network (mempool.space)
// ─────────────────────────────────────────────────────────────────────────────
void drawScreen6_lightning() {
  int W = tft.width(), cx = W / 2;
  int y = L_BORDER + 6;
  tft.setTextDatum(TC_DATUM);

  sectionHeader(L_BORDER+4, y, "LN CAPACITY", C_BLUE); y += 12;
  if (g_lnCapacitySats > 0) {
    tft.fillRect(L_BORDER+2, y-2, W-2*(L_BORDER+2), 30, C_BG);
    tft.setTextColor(C_BLUE, C_BG); tft.setTextSize(3);
    char buf[24];
    snprintf(buf, sizeof(buf), "%.0f BTC", g_lnCapacitySats / 1e8);
    tft.drawString(buf, cx, y);
  } else {
    tft.setTextColor(C_DIM, C_BG); tft.setTextSize(2);
    tft.drawString("--", cx, y);
  }
  y += 30;

  hRule(L_BORDER+4, y, W-2*(L_BORDER+4)); y += 8;

  sectionHeader(L_BORDER+4, y, "CHANNELS"); y += 12;
  tft.setTextColor(TFT_WHITE, C_BG); tft.setTextSize(2);
  tft.drawString(fmtBlockNum(g_lnChannels), cx, y); y += 20;

  hRule(L_BORDER+4, y, W-2*(L_BORDER+4)); y += 8;

  sectionHeader(L_BORDER+4, y, "NODES"); y += 12;
  tft.setTextColor(TFT_WHITE, C_BG); tft.setTextSize(2);
  tft.drawString(fmtBlockNum(g_lnNodes), cx, y); y += 20;

  hRule(L_BORDER+4, y, W-2*(L_BORDER+4)); y += 8;

  sectionHeader(L_BORDER+4, y, "NETWORK DETAIL"); y += 12;
  {
    char buf[40];
    snprintf(buf, sizeof(buf), "avg channel %s", fmtSats((int64_t)g_lnAvgCapacity).c_str());
    tft.setTextColor(C_GOLDEN, C_BG); tft.setTextSize(1);
    tft.drawString(buf, cx, y); y += 12;
    snprintf(buf, sizeof(buf), "%lu tor  /  %lu clearnet",
             (unsigned long)g_lnTorNodes, (unsigned long)g_lnClearnetNodes);
    tft.setTextColor(C_LABEL, C_BG);
    tft.drawString(buf, cx, y); y += 12;
  }

  int statusY = (g_orient == 0) ? L_STATUS_Y : LL_STATUS_Y;
  hRule(L_BORDER+4, statusY - 4, W-2*(L_BORDER+4));
  drawStatusBar(L_BORDER+1, statusY, W-2*(L_BORDER+1));
  tft.setTextDatum(TL_DATUM);
}

// ─────────────────────────────────────────────────────────────────────────────
//  updateClock — called every second, only refreshes time-sensitive areas
// ─────────────────────────────────────────────────────────────────────────────
void updateClock() {
  int W  = tft.width(), cx = W / 2;
  int statusY = (g_orient == 0) ? L_STATUS_Y : LL_STATUS_Y;
  tft.setTextDatum(TC_DATUM);

  switch (curScreen) {
    case 0:
      tft.fillRect(L_BORDER+1, L_CLOCK_Y, W-2*(L_BORDER+1), 26, C_BG);
      tft.setTextColor(TFT_WHITE, C_BG); tft.setTextSize(3);
      tft.drawString(nowHHMMSS(), cx, L_CLOCK_Y);
      drawStatusBar(L_BORDER+1, statusY, W-2*(L_BORDER+1));
      break;

    case 1: case 2: case 3: case 4: case 6:
      drawStatusBar(L_BORDER+1, statusY, W-2*(L_BORDER+1));
      break;

    case 5: {
      // Must match drawScreen5_clock()'s clockY(8)/height(34) exactly —
      // this used to erase down to y=50 with the clock at y=10, which
      // overwrote the date line (now at y=46) on every 1-second tick.
      tft.fillRect(L_BORDER+1, 8, W-2*(L_BORDER+1), 34, C_BG);
      tft.setTextColor(TFT_WHITE, C_BG); tft.setTextSize(4);
      tft.drawString(nowHHMMSS().substring(0, 5), cx, 8);
      drawStatusBar(L_BORDER+1, statusY, W-2*(L_BORDER+1));
      break;
    }
  }
  tft.setTextDatum(TL_DATUM);
}
