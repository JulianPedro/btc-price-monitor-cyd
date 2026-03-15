// User_Setup.h — TFT_eSPI configuration for ESP32-2432S028 (CYD)
//
// This file is copied by the CI workflow into the TFT_eSPI library directory
// before compiling, so the library is correctly configured for the CYD board.
// It can also be copied manually if you prefer to manage it yourself.

#define USER_SETUP_ID 206

// ─── Display driver ──────────────────────────────────────────────────────────
#define ILI9341_DRIVER

// ─── SPI pins (HSPI bus — display) ───────────────────────────────────────────
#define TFT_MISO  12
#define TFT_MOSI  13
#define TFT_SCLK  14
#define TFT_CS    15
#define TFT_DC     2
#define TFT_RST   -1   // tied to EN/RESET pin; -1 = not controlled by library
#define TFT_BL    21   // backlight PWM — controlled by the sketch, not the library

#define TFT_BACKLIGHT_ON HIGH

// ─── Fonts ───────────────────────────────────────────────────────────────────
#define LOAD_GLCD    // built-in 8x8 pixel font
#define LOAD_FONT2   // small 16 px
#define LOAD_FONT4   // medium 26 px
#define LOAD_FONT6   // large 48 px (digits only)
#define LOAD_FONT7   // 7-segment 48 px (digits only)
#define LOAD_FONT8   // large 75 px (digits only)
#define LOAD_GFXFF   // Adafruit GFX-compatible free fonts
#define SMOOTH_FONT

// ─── SPI clock frequencies ───────────────────────────────────────────────────
#define SPI_FREQUENCY        55000000
#define SPI_READ_FREQUENCY   20000000
#define SPI_TOUCH_FREQUENCY   2500000
