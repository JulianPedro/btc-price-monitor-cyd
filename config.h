/*
 *  BTC Monitor CYD — config.h
 *  Edit this file with your settings before compiling.
 *
 *  Timezone and screen orientation can also be changed via the Wi-Fi
 *  captive portal — hold the screen for 3 s at power-on to reopen
 *  the portal at any time without reflashing.
 */

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  TIMEZONE  — default value used on FIRST boot only.
//  After configuration via the portal, the value saved in flash takes priority.
//
//  UTC-3  Brasilia / Sao Paulo:  (-3L * 3600L)  <- default
//  UTC-4  Campo Grande / Manaus: (-4L * 3600L)
//  UTC-5  Acre / New York:       (-5L * 3600L)
//  UTC+0  Lisbon / London:       (  0L * 3600L)
//  UTC+1  Paris / Berlin:        (  1L * 3600L)
//  UTC+8  Beijing / Hong Kong:   (  8L * 3600L)
// ─────────────────────────────────────────────────────────────────────────────
#define TIMEZONE_OFFSET_SEC  (-3L * 3600L)   // initial default
#define DST_OFFSET_SEC       0

// ─────────────────────────────────────────────────────────────────────────────
//  WI-FI MANAGER
//  On first boot (or when the screen is held for 3 s), the device creates
//  an Access Point. Connect to it and open 192.168.4.1 to configure
//  Wi-Fi, timezone, screen orientation, and comparison currency.
// ─────────────────────────────────────────────────────────────────────────────
#define WIFI_AP_NAME  "BTC-Monitor"
#define WIFI_AP_PASS  ""          // empty = open AP; set a password if preferred

// Maximum time waiting for portal configuration (seconds). 0 = infinite.
#define WIFI_PORTAL_TIMEOUT_SEC  300

// ─────────────────────────────────────────────────────────────────────────────
//  PRICE UPDATES
// ─────────────────────────────────────────────────────────────────────────────
#define PRICE_REFRESH_MS  60000UL   // 60 000 ms = 60 s

// ─────────────────────────────────────────────────────────────────────────────
//  HISTORY / SPARKLINE
//  Each point = 1 PRICE_REFRESH_MS interval.
//  30 points x 60 s = 30 minutes of history.
// ─────────────────────────────────────────────────────────────────────────────
#define SPARKLINE_POINTS  30

// ─────────────────────────────────────────────────────────────────────────────
//  COMPARISON CURRENCY — initial default (can be changed via portal)
//  Examples: BRL, EUR, GBP, JPY, ARS, CLP, MXN, CAD, AUD
// ─────────────────────────────────────────────────────────────────────────────
#define DEFAULT_CURRENCY  "BRL"

// ─────────────────────────────────────────────────────────────────────────────
//  RGB LED  (brightness when flashing on price change)
//  0 = off, 255 = maximum
//  LED is off by default; flashes green (price up) or red (price down)
// ─────────────────────────────────────────────────────────────────────────────
#define LED_BRIGHTNESS  48

// ─────────────────────────────────────────────────────────────────────────────
//  BACKLIGHT / LDR  (auto-brightness)
//  Adjust RAW_DARK and RAW_LIGHT to match your sensor's response.
//  Tip: open the Serial Monitor and observe the values printed at startup.
// ─────────────────────────────────────────────────────────────────────────────
#define BL_MIN          15     // prevents screen from going completely dark
#define BL_MAX          255
#define LDR_RAW_DARK    3800   // typical ADC reading in darkness
#define LDR_RAW_LIGHT   300    // typical ADC reading under bright light
