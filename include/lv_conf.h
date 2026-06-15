// lv_conf.h — partial LVGL 8.x config. Anything omitted falls back to the
// defaults in lv_conf_internal.h. Enabled via -D LV_CONF_INCLUDE_SIMPLE.
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// 16-bit colour. We byte-swap in the flush callback (tft.pushColors(..., true)),
// so leave LV_COLOR_16_SWAP at 0 here.
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

// LVGL's own heap (objects, styles). 32K fits this tabview UI with margin and
// keeps static DRAM within the ESP32-WROOM budget (no PSRAM on the CYD).
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (32U * 1024U)

// Drive LVGL's tick from Arduino millis() — no separate timer ISR needed.
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_DPI_DEF 130

// Fonts used by the UI.
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// Keep it lean / quiet on an MCU.
#define LV_USE_LOG 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MALLOC 0

#endif // LV_CONF_H
