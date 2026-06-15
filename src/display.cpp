// display.cpp — wires TFT_eSPI, the XPT2046 touch panel and LVGL together.
//
// On the CYD the display sits on the HSPI pins (configured via build flags in
// platformio.ini) and the touch controller on a *separate* SPI bus. We give the
// touch its own SPIClass(VSPI) remapped to the touch GPIOs so the two never
// fight over the bus.
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>
#include <Preferences.h>

#include "config.h"
#include "display.h"

#define NVS_KEY_BRIGHTNESS "bri"

static TFT_eSPI tft;
static SPIClass touchSPI(VSPI);
static XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

static uint8_t s_brightness = BL_DEFAULT;

// LVGL draw buffer: 40 lines tall. 320*40*2 = ~25 KB of internal RAM.
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[SCREEN_W * 40];
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

// ---- LVGL display flush -----------------------------------------------------
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&px->full, w * h, true);  // true = swap bytes
  tft.endWrite();
  lv_disp_flush_ready(drv);
}

// ---- LVGL touch read --------------------------------------------------------
static void touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  if (ts.tirqTouched() && ts.touched()) {
    TS_Point p = ts.getPoint();
    int x = map(p.x, TOUCH_RAW_MIN_X, TOUCH_RAW_MAX_X, 0, SCREEN_W - 1);
    int y = map(p.y, TOUCH_RAW_MIN_Y, TOUCH_RAW_MAX_Y, 0, SCREEN_H - 1);
    data->point.x = constrain(x, 0, SCREEN_W - 1);
    data->point.y = constrain(y, 0, SCREEN_H - 1);
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

void displaySetBrightness(uint8_t level) {
  s_brightness = level;
  ledcWrite(BL_PWM_CHANNEL, level);
}

uint8_t displayGetBrightness() { return s_brightness; }

void displayLoadBrightness() {
  Preferences p;
  p.begin(NVS_NAMESPACE, true);  // read-only
  uint8_t v = p.getUChar(NVS_KEY_BRIGHTNESS, BL_DEFAULT);
  p.end();
  s_brightness = (v < 8) ? 8 : v;  // never restore a fully-black screen
}

void displaySaveBrightness() {
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  p.putUChar(NVS_KEY_BRIGHTNESS, s_brightness);
  p.end();
}

int displayReadLDR() { return analogRead(LDR_PIN); }

void displayInit() {
  // Status LED (active LOW) — start off.
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, HIGH);

  tft.begin();
  tft.setRotation(TFT_ROTATION);

  // Backlight PWM — set up AFTER tft.begin(), which drives TFT_BL HIGH as a
  // plain GPIO and would otherwise detach our LEDC channel from the pin
  // (that's why the brightness slider had no effect).
  displayLoadBrightness();  // restore the saved level before applying it
  ledcSetup(BL_PWM_CHANNEL, BL_PWM_FREQ, BL_PWM_RES);
  ledcAttachPin(TFT_BL, BL_PWM_CHANNEL);
  displaySetBrightness(s_brightness);

  // Touch on its own bus: SCLK, MISO, MOSI, CS.
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(TFT_ROTATION);

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf, nullptr, SCREEN_W * 40);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_W;
  disp_drv.ver_res = SCREEN_H;
  disp_drv.flush_cb = flush_cb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_cb;
  lv_indev_drv_register(&indev_drv);
}
