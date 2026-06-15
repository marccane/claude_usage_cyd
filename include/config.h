// config.h — board pins, timings and constants for the Claude/CYD monitor.
#pragma once

// ---------------------------------------------------------------------------
// Display orientation. 1 = landscape (USB on the left), 3 = landscape flipped
// 180° (USB on the right). Both the panel and touch follow this.
// ---------------------------------------------------------------------------
#define TFT_ROTATION 3
#define SCREEN_W 320
#define SCREEN_H 240

// ---------------------------------------------------------------------------
// XPT2046 resistive touch — on its OWN SPI bus (VSPI), separate from the TFT.
// These are the standard CYD touch pins.
// ---------------------------------------------------------------------------
#define XPT2046_CLK 25
#define XPT2046_MISO 39
#define XPT2046_MOSI 32
#define XPT2046_CS 33
#define XPT2046_IRQ 36

// Raw touch range -> screen mapping. Tweak these if taps land off-target.
// (Defaults are typical for the CYD XPT2046 in landscape.)
#define TOUCH_RAW_MIN_X 200
#define TOUCH_RAW_MAX_X 3900
#define TOUCH_RAW_MIN_Y 240
#define TOUCH_RAW_MAX_Y 3800

// ---------------------------------------------------------------------------
// Backlight PWM (LEDC). GPIO21 on the CYD.
// ---------------------------------------------------------------------------
#define BL_PWM_CHANNEL 0
#define BL_PWM_FREQ 5000
#define BL_PWM_RES 8           // 8-bit -> 0..255
#define BL_DEFAULT 200

// On-board RGB LED (active LOW) — used as a connection/health indicator.
#define LED_R 4
#define LED_G 16
#define LED_B 17

// Light-dependent resistor (analog) — available for auto-brightness ideas.
#define LDR_PIN 34

// ---------------------------------------------------------------------------
// Behaviour
// ---------------------------------------------------------------------------
#define REFRESH_MS (180UL * 1000UL)   // matches `watch -n 180` in claudeUsageWatch
#define WIFI_TIMEOUT_MS 20000
#define NTP_SERVER "pool.ntp.org"
#define FW_VERSION "claude-cyd 1.0"

// NVS namespace/keys for the pushed auth blob.
#define NVS_NAMESPACE "claude"
#define NVS_KEY_BLOB "blob"          // compact JSON from claude_token_export.py

// Colour thresholds mirror claude_usage.py's bar(): <60 green, <85 amber, else red.
#define PCT_AMBER 60.0f
#define PCT_RED 85.0f
