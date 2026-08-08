/*
 *  BTC Monitor CYD — config.h
 *  Edit this file with your settings before compiling.
 *
 *  Timezone, orientation, currency and watch address can also be changed
 *  via the Wi-Fi captive portal — hold the screen for 3 s at power-on or
 *  hold the BOOT button for 2 s at any time.
 */

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  TIMEZONE  — default used on first boot only.
//  After portal configuration the value stored in flash takes priority.
//
//  UTC-3  Brasilia / Sao Paulo:  (-3L * 3600L)  <- default
//  UTC-4  Campo Grande / Manaus: (-4L * 3600L)
//  UTC-5  Acre / New York:       (-5L * 3600L)
//  UTC+0  Lisbon / London:       (  0L * 3600L)
//  UTC+1  Paris / Berlin:        (  1L * 3600L)
//  UTC+8  Beijing / Hong Kong:   (  8L * 3600L)
// ─────────────────────────────────────────────────────────────────────────────
#define TIMEZONE_OFFSET_SEC  (-3L * 3600L)   // UTC-3 Brasilia / São Paulo
#define DST_OFFSET_SEC       0

// ─────────────────────────────────────────────────────────────────────────────
//  WI-FI MANAGER
// ─────────────────────────────────────────────────────────────────────────────
#define WIFI_AP_NAME  "BTC-Monitor"
// CHANGE THIS. The setup/settings portal is an open Wi-Fi AP while it's
// running (broadcasts your Wi-Fi password in cleartext during setup, and
// lets anyone in range repoint the mempool backend or read your watch
// address). Must be empty ("" = open AP, not recommended) or >= 8 chars
// for WPA2.
#define WIFI_AP_PASS  "btcmonitor"
#define WIFI_PORTAL_TIMEOUT_SEC  300

// ─────────────────────────────────────────────────────────────────────────────
//  REFRESH INTERVALS
// ─────────────────────────────────────────────────────────────────────────────
#define PRICE_REFRESH_MS    60000UL    // price (BTC/USD + local)
#define FEES_REFRESH_MS    120000UL    // mempool fee tiers
#define NETWORK_REFRESH_MS 300000UL    // block height, difficulty, hashrate
#define FNG_REFRESH_MS     600000UL    // Fear & Greed index
#define ADDR_REFRESH_MS    300000UL    // watch-address balance
#define LN_REFRESH_MS      1800000UL   // Lightning Network stats (mempool.space updates this ~daily)

// ─────────────────────────────────────────────────────────────────────────────
//  HISTORY / SPARKLINE
//  30 points × 60 s = 30 min of history
// ─────────────────────────────────────────────────────────────────────────────
#define SPARKLINE_POINTS  30

// ─────────────────────────────────────────────────────────────────────────────
//  COMPARISON CURRENCY  (initial default; changeable via portal)
//  Options: BRL EUR GBP JPY ARS CLP MXN CAD AUD
// ─────────────────────────────────────────────────────────────────────────────
#define DEFAULT_CURRENCY  "BRL"

// ─────────────────────────────────────────────────────────────────────────────
//  MEMPOOL BACKEND  (initial default; changeable via portal)
//  Source for block height, hashrate, difficulty, fees and address lookups.
//  Default is the public mempool.space instance (HTTPS). Point this at your
//  own node's mempool web UI instead — e.g. "192.168.1.50:4081" — to use
//  its data. A bare host:port defaults to HTTPS *unless* it's a private/LAN
//  address (10.x, 192.168.x, 172.16-31.x, 127.x, or *.local), which default
//  to plain HTTP since a home node rarely has a valid TLS cert. Include an
//  explicit http:// or https:// prefix to override the guess either way.
//
//  Privacy note: whichever backend you use, watch-address lookups tell that
//  server "this IP is interested in this Bitcoin address" on every refresh.
//  Point this at your own node if that's a concern — see README.
// ─────────────────────────────────────────────────────────────────────────────
#define DEFAULT_MEMPOOL_HOST  "mempool.space"

// ─────────────────────────────────────────────────────────────────────────────
//  HTTP USER-AGENT  (initial default; changeable via portal)
//  Sent on every request to every API this sketch calls.
// ─────────────────────────────────────────────────────────────────────────────
#define DEFAULT_USER_AGENT  "btc-price-monitor-cyd/2.0"

// ─────────────────────────────────────────────────────────────────────────────
//  AUTO-CYCLE  (initial default; changeable via portal)
//  Seconds between automatic screen changes. 0 = disabled (manual tap only).
// ─────────────────────────────────────────────────────────────────────────────
#define DEFAULT_AUTOCYCLE_SEC  0

// ─────────────────────────────────────────────────────────────────────────────
//  HTTP SAFETY
//  Hard cap on any single HTTP response body this sketch will read, in
//  bytes, regardless of what Content-Length claims (which is untrusted and
//  optional). Protects heap from a malicious or misbehaving server on any
//  configured backend. Every real response used here is a few KB at most.
// ─────────────────────────────────────────────────────────────────────────────
#define HTTP_MAX_BODY_BYTES  32768UL

// ─────────────────────────────────────────────────────────────────────────────
//  RGB LED
// ─────────────────────────────────────────────────────────────────────────────
#define LED_BRIGHTNESS  255
#define FEE_SPIKE_DELTA   5   // sat/vB jump in fastestFee between polls that blinks the LED

// ─────────────────────────────────────────────────────────────────────────────
//  BACKLIGHT / LDR
// ─────────────────────────────────────────────────────────────────────────────
#define BL_MIN         15
#define BL_MAX         255
#define LDR_RAW_DARK   3800
#define LDR_RAW_LIGHT   300
