// display.h — TFT_eSPI + XPT2046 touch + LVGL bring-up.
#pragma once
#include <stdint.h>

// Initialise TFT, backlight PWM, touch (on its own SPI bus) and LVGL, then
// register the display + input drivers. Call once from setup().
void displayInit();

// Set backlight 0..255 (LEDC PWM on TFT_BL).
void displaySetBrightness(uint8_t level);
uint8_t displayGetBrightness();

// Brightness persistence (NVS). Load restores the saved level into the active
// brightness; save persists the current level. Called at boot / on slider release.
void displayLoadBrightness();
void displaySaveBrightness();

// Toggle the screen 180° (swaps rotation 1<->3), redraw, and persist to NVS.
void displayFlip();

// Raw LDR reading (0..4095) for auto-brightness experiments.
int displayReadLDR();
