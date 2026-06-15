// ui.cpp — LVGL widgets. Three tabs (Usage / Limits / Info), colour-coded bars
// that mirror claude_usage.py's green/amber/red thresholds, and touch controls.
#include <lvgl.h>
#include <math.h>
#include <time.h>

#include "ui.h"
#include "config.h"
#include "display.h"

// One usage row: title + bar + "42%  2h5m" info label.
typedef struct {
  lv_obj_t *info;
  lv_obj_t *bar;
} meter_t;

static meter_t m_five, m_seven, m_opus, m_sonnet, m_extra;
static lv_obj_t *lbl_header;     // "Ada / Acme Inc"
static lv_obj_t *lbl_limits;     // rate-limit dump
static lv_obj_t *lbl_user, *lbl_org, *lbl_updated, *lbl_next, *lbl_net,
    *lbl_state;
static lv_obj_t *slider_bri;
static volatile bool s_refreshRequested = false;

// ---- helpers ----------------------------------------------------------------
static lv_color_t barColor(float p) {
  if (p < PCT_AMBER) return lv_color_hex(0x35C759);  // green
  if (p < PCT_RED) return lv_color_hex(0xFFCC00);    // amber
  return lv_color_hex(0xFF3B30);                     // red
}

static void fmtReset(long s, char *out, size_t n) {
  if (s < 0) {
    snprintf(out, n, "--");
  } else if (s == 0) {
    snprintf(out, n, "now");
  } else {
    long h = s / 3600, m = (s % 3600) / 60;
    if (h > 0)
      snprintf(out, n, "%ldh %ldm", h, m);
    else
      snprintf(out, n, "%ldm", m);
  }
}

static meter_t makeMeter(lv_obj_t *parent, const char *name) {
  meter_t mt;
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_size(c, lv_pct(100), 56);
  lv_obj_set_style_pad_all(c, 6, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(c);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, lv_color_black(), 0);
  lv_label_set_text(title, name);

  mt.info = lv_label_create(c);
  lv_obj_align(mt.info, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_set_style_text_font(mt.info, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(mt.info, lv_color_black(), 0);
  lv_label_set_text(mt.info, "--");

  mt.bar = lv_bar_create(c);
  lv_obj_set_size(mt.bar, lv_pct(100), 14);
  lv_obj_align(mt.bar, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_bar_set_range(mt.bar, 0, 100);
  lv_bar_set_value(mt.bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(mt.bar, lv_color_hex(0x333333), LV_PART_MAIN);
  return mt;
}

static void setMeter(meter_t &mt, bool present, float util, long resets) {
  if (!present) {
    lv_label_set_text(mt.info, "n/a");
    lv_bar_set_value(mt.bar, 0, LV_ANIM_OFF);
    return;
  }
  int v = (int)lroundf(util);
  lv_bar_set_value(mt.bar, v, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(mt.bar, barColor(util), LV_PART_INDICATOR);
  char rs[24];
  fmtReset(resets, rs, sizeof(rs));
  lv_label_set_text_fmt(mt.info, "%d%%   %s", v, rs);
}

// ---- event callbacks --------------------------------------------------------
static void refresh_evt(lv_event_t *e) {
  (void)e;
  s_refreshRequested = true;
  uiSetState("Refreshing...");
}

static void bri_evt(lv_event_t *e) {
  lv_obj_t *s = lv_event_get_target(e);
  displaySetBrightness((uint8_t)lv_slider_get_value(s));
}

// Persist only when the drag ends, so we don't hammer NVS on every tick.
static void bri_save_evt(lv_event_t *e) {
  (void)e;
  displaySaveBrightness();
}

bool uiTakeRefreshRequest() {
  bool r = s_refreshRequested;
  s_refreshRequested = false;
  return r;
}

// ---- tab builders -----------------------------------------------------------
static void buildUsageTab(lv_obj_t *tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tab, 2, 0);

  lbl_header = lv_label_create(tab);
  lv_obj_set_style_text_font(lbl_header, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_header, lv_color_black(), 0);
  lv_label_set_text(lbl_header, "Claude.ai usage");

  m_five = makeMeter(tab, "5-hour");
  m_seven = makeMeter(tab, "7-day");
  m_opus = makeMeter(tab, "Opus 7d");
  m_sonnet = makeMeter(tab, "Sonnet 7d");
  m_extra = makeMeter(tab, "Extra $");
}

static void buildLimitsTab(lv_obj_t *tab) {
  lbl_limits = lv_label_create(tab);
  lv_label_set_long_mode(lbl_limits, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lbl_limits, lv_pct(100));
  lv_obj_set_style_text_font(lbl_limits, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_limits, lv_color_black(), 0);
  lv_label_set_text(lbl_limits, "Rate limits load with usage.");
}

static lv_obj_t *infoRow(lv_obj_t *tab, const char *prefix) {
  lv_obj_t *l = lv_label_create(tab);
  lv_obj_set_width(l, lv_pct(100));
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(l, lv_color_black(), 0);
  lv_label_set_text(l, prefix);
  return l;
}

static void buildInfoTab(lv_obj_t *tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tab, 4, 0);

  lbl_user = infoRow(tab, "User: --");
  lbl_org = infoRow(tab, "Org: --");
  lbl_net = infoRow(tab, "WiFi: --");
  lbl_updated = infoRow(tab, "Updated: never");
  lbl_next = infoRow(tab, "Next: --");
  lbl_state = infoRow(tab, "");
  lv_obj_set_style_text_color(lbl_state, lv_color_hex(0xC75B00), 0);

  lv_obj_t *btn = lv_btn_create(tab);
  lv_obj_add_event_cb(btn, refresh_evt, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *bl = lv_label_create(btn);
  lv_label_set_text(bl, LV_SYMBOL_REFRESH " Refresh now");
  lv_obj_center(bl);

  lv_obj_t *brow = infoRow(tab, LV_SYMBOL_SETTINGS " Brightness");
  (void)brow;
  slider_bri = lv_slider_create(tab);
  lv_obj_set_width(slider_bri, lv_pct(100));
  lv_slider_set_range(slider_bri, 8, 255);
  lv_slider_set_value(slider_bri, displayGetBrightness(), LV_ANIM_OFF);
  lv_obj_add_event_cb(slider_bri, bri_evt, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(slider_bri, bri_save_evt, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(slider_bri, bri_save_evt, LV_EVENT_PRESS_LOST, nullptr);

  lv_obj_t *fw = lv_label_create(tab);
  lv_obj_set_style_text_color(fw, lv_color_hex(0x666666), 0);
  lv_obj_set_style_text_font(fw, &lv_font_montserrat_12, 0);
  lv_label_set_text(fw, FW_VERSION);
}

// ---- public -----------------------------------------------------------------
void uiInit() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

  lv_obj_t *tv = lv_tabview_create(scr, LV_DIR_TOP, 34);
  lv_obj_set_style_bg_color(tv, lv_color_white(), 0);

  buildUsageTab(lv_tabview_add_tab(tv, LV_SYMBOL_BARS " Usage"));
  buildLimitsTab(lv_tabview_add_tab(tv, LV_SYMBOL_LIST " Limits"));
  buildInfoTab(lv_tabview_add_tab(tv, LV_SYMBOL_SETTINGS " Info"));
}

void uiUpdate(const UsageData &d) {
  if (d.org_name[0] || d.name[0])
    lv_label_set_text_fmt(lbl_header, "%s  -  %s",
                          d.name[0] ? d.name : "?",
                          d.org_name[0] ? d.org_name : "?");

  setMeter(m_five, d.five_hour.present, d.five_hour.util, d.five_hour.resets_in);
  setMeter(m_seven, d.seven_day.present, d.seven_day.util, d.seven_day.resets_in);
  setMeter(m_opus, d.opus.present, d.opus.util, d.opus.resets_in);
  setMeter(m_sonnet, d.sonnet.present, d.sonnet.util, d.sonnet.resets_in);

  if (d.extra.enabled) {
    setMeter(m_extra, true, d.extra.util < 0 ? 0 : d.extra.util, -1);
    if (d.extra.used >= 0)
      lv_label_set_text_fmt(m_extra.info, "%.2f/%.2f %s", d.extra.used,
                            d.extra.total, d.extra.currency);
  } else {
    setMeter(m_extra, false, 0, -1);
    lv_label_set_text(m_extra.info, "off");
  }

  // Limits tab text.
  if (d.n_limits > 0) {
    String t;
    char last[40] = "";
    for (int i = 0; i < d.n_limits; i++) {
      const RateLimit &rl = d.limits[i];
      if (strcmp(rl.model_group, last) != 0) {
        t += String("\n") + rl.model_group + "\n";
        strncpy(last, rl.model_group, sizeof(last) - 1);
      }
      t += String("   ") + rl.limiter + " = " + rl.value + "\n";
    }
    lv_label_set_text(lbl_limits, t.c_str());
  }

  lv_label_set_text_fmt(lbl_user, "User: %s", d.name[0] ? d.name : "--");
  lv_label_set_text_fmt(lbl_org, "Org: %s", d.org_name[0] ? d.org_name : "--");

  if (d.valid) {
    struct tm tmv;
    time_t t = d.fetched_at;
    localtime_r(&t, &tmv);
    lv_label_set_text_fmt(lbl_updated, "Updated: %02d:%02d:%02d UTC", tmv.tm_hour,
                          tmv.tm_min, tmv.tm_sec);
    uiSetState("");
  } else {
    lv_label_set_text_fmt(lbl_state, LV_SYMBOL_WARNING " %s",
                          d.error[0] ? d.error : "fetch failed");
  }
}

void uiSetNet(bool connected, const String &ip) {
  if (!lbl_net) return;
  if (connected)
    lv_label_set_text_fmt(lbl_net, "WiFi: connected (%s)", ip.c_str());
  else
    lv_label_set_text(lbl_net, "WiFi: connecting...");
}

void uiTick(long secs) {
  if (!lbl_next) return;
  if (secs < 0)
    lv_label_set_text(lbl_next, "Next: now");
  else
    lv_label_set_text_fmt(lbl_next, "Next: %lds", secs);
}

void uiSetState(const char *msg) {
  if (lbl_state) lv_label_set_text(lbl_state, msg ? msg : "");
}
