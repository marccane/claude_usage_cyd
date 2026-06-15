// ui.cpp — LVGL widgets. Three tabs (Usage / Limits / Info), colour-coded bars
// that mirror claude_usage.py's green/amber/red thresholds, touch controls, and
// a light/dark theme toggle (persisted in NVS).
//
// Theming: text + bar-track colours come from two shared lv_style_t objects, so
// switching themes is just "update the style + report change". Backgrounds are
// set directly on the few container objects.
#include <lvgl.h>
#include <math.h>
#include <time.h>
#include <Preferences.h>

#include "ui.h"
#include "config.h"
#include "display.h"

typedef struct {
  lv_obj_t *cont;
  lv_obj_t *info;
  lv_obj_t *bar;
} meter_t;

static meter_t m_five, m_seven, m_opus, m_sonnet, m_extra;
static lv_obj_t *lbl_header, *lbl_limits;
static lv_obj_t *lbl_user, *lbl_org, *lbl_updated, *lbl_next, *lbl_net, *lbl_state,
    *lbl_fw;
static lv_obj_t *slider_bri, *sw_dark, *sw_bars;
static lv_obj_t *s_tv, *s_page_usage, *s_page_limits, *s_page_info;
static lv_style_t s_style_text;   // primary text colour (themed, inherited by labels)
static lv_style_t s_style_track;  // bar track colour (themed)
static bool s_dark = false;
static volatile bool s_refreshRequested = false;

// ---- theme persistence ------------------------------------------------------
static bool loadDarkPref() {
  Preferences p;
  p.begin(NVS_NAMESPACE, true);
  bool d = p.getBool("dark", false);
  p.end();
  return d;
}
static void saveDarkPref(bool d) {
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  p.putBool("dark", d);
  p.end();
}

static bool loadBarsPref() {
  Preferences p;
  p.begin(NVS_NAMESPACE, true);
  bool b = p.getBool("bars", true);  // detail bars shown by default
  p.end();
  return b;
}
static void saveBarsPref(bool b) {
  Preferences p;
  p.begin(NVS_NAMESPACE, false);
  p.putBool("bars", b);
  p.end();
}

// Show/hide the Opus, Sonnet and Extra-$ rows (hidden rows drop out of the
// flex layout, so the remaining bars reflow up).
static void setDetailBarsVisible(bool v) {
  lv_obj_t *conts[3] = {m_opus.cont, m_sonnet.cont, m_extra.cont};
  for (int i = 0; i < 3; i++) {
    if (!conts[i]) continue;
    if (v)
      lv_obj_clear_flag(conts[i], LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(conts[i], LV_OBJ_FLAG_HIDDEN);
  }
}

// ---- helpers ----------------------------------------------------------------
static void styleText(lv_obj_t *l) { lv_obj_add_style(l, &s_style_text, 0); }

static void themeBg(lv_obj_t *o, lv_color_t bg) {
  lv_obj_set_style_bg_color(o, bg, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

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

// ---- theming ----------------------------------------------------------------
static void applyTheme(bool dark) {
  lv_color_t bg = dark ? lv_color_hex(0x15151A) : lv_color_white();
  lv_color_t text = dark ? lv_color_hex(0xF0F0F0) : lv_color_black();
  lv_color_t tabbar = dark ? lv_color_hex(0x26262E) : lv_color_hex(0xE6E6E6);
  lv_color_t border = dark ? lv_color_hex(0x3C3C46) : lv_color_hex(0xBFBFBF);
  lv_color_t track = dark ? lv_color_hex(0x3A3A44) : lv_color_hex(0xD2D2D2);
  lv_color_t accent = dark ? lv_color_hex(0xFFB000) : lv_color_hex(0xC75B00);
  lv_color_t dim = lv_color_hex(0x8A8A8A);

  // Shared styles -> refresh every object that uses them.
  lv_style_set_text_color(&s_style_text, text);
  lv_style_set_bg_color(&s_style_track, track);
  lv_obj_report_style_change(&s_style_text);
  lv_obj_report_style_change(&s_style_track);

  // Backgrounds.
  themeBg(lv_scr_act(), bg);
  themeBg(s_tv, bg);
  themeBg(lv_tabview_get_content(s_tv), bg);
  themeBg(s_page_usage, bg);
  themeBg(s_page_limits, bg);
  themeBg(s_page_info, bg);

  // Tab bar: distinct fill + a divider line under it, so the selection zone is
  // clearly separated from the content.
  lv_obj_t *bar = lv_tabview_get_tab_btns(s_tv);
  themeBg(bar, tabbar);
  lv_obj_set_style_border_color(bar, border, 0);
  lv_obj_set_style_border_width(bar, 2, 0);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_text_color(bar, text, LV_PART_ITEMS);
  lv_obj_set_style_text_color(bar, accent, LV_PART_ITEMS | LV_STATE_CHECKED);

  // Accent / dim labels (local styles override the shared text style).
  if (lbl_state) lv_obj_set_style_text_color(lbl_state, accent, 0);
  if (lbl_fw) lv_obj_set_style_text_color(lbl_fw, dim, 0);
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

static void dark_evt(lv_event_t *e) {
  s_dark = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  applyTheme(s_dark);
  saveDarkPref(s_dark);
}

static void bars_evt(lv_event_t *e) {
  bool v = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  setDetailBarsVisible(v);
  saveBarsPref(v);
}

static void flip_evt(lv_event_t *e) {
  (void)e;
  displayFlip();
}

bool uiTakeRefreshRequest() {
  bool r = s_refreshRequested;
  s_refreshRequested = false;
  return r;
}

// ---- meter rows -------------------------------------------------------------
static meter_t makeMeter(lv_obj_t *parent, const char *name) {
  meter_t mt;
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_size(c, lv_pct(100), 56);
  lv_obj_set_style_pad_all(c, 6, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  mt.cont = c;

  lv_obj_t *title = lv_label_create(c);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  styleText(title);
  lv_label_set_text(title, name);

  mt.info = lv_label_create(c);
  lv_obj_align(mt.info, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_set_style_text_font(mt.info, &lv_font_montserrat_20, 0);
  styleText(mt.info);
  lv_label_set_text(mt.info, "--");

  mt.bar = lv_bar_create(c);
  lv_obj_set_size(mt.bar, lv_pct(100), 14);
  lv_obj_align(mt.bar, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_bar_set_range(mt.bar, 0, 100);
  lv_bar_set_value(mt.bar, 0, LV_ANIM_OFF);
  lv_obj_add_style(mt.bar, &s_style_track, LV_PART_MAIN);
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

// ---- tab builders -----------------------------------------------------------
static void buildUsageTab(lv_obj_t *tab) {
  lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tab, 2, 0);

  lbl_header = lv_label_create(tab);
  lv_obj_set_style_text_font(lbl_header, &lv_font_montserrat_14, 0);
  styleText(lbl_header);
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
  styleText(lbl_limits);
  lv_label_set_text(lbl_limits, "Rate limits load with usage.");
}

static lv_obj_t *infoRow(lv_obj_t *tab, const char *prefix) {
  lv_obj_t *l = lv_label_create(tab);
  lv_obj_set_width(l, lv_pct(100));
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  styleText(l);
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

  lv_obj_t *btn = lv_btn_create(tab);
  lv_obj_add_event_cb(btn, refresh_evt, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *bl = lv_label_create(btn);
  lv_label_set_text(bl, LV_SYMBOL_REFRESH " Refresh now");
  lv_obj_center(bl);

  infoRow(tab, LV_SYMBOL_SETTINGS " Brightness");
  slider_bri = lv_slider_create(tab);
  lv_obj_set_width(slider_bri, lv_pct(100));
  lv_slider_set_range(slider_bri, 8, 255);
  lv_slider_set_value(slider_bri, displayGetBrightness(), LV_ANIM_OFF);
  lv_obj_add_event_cb(slider_bri, bri_evt, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(slider_bri, bri_save_evt, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(slider_bri, bri_save_evt, LV_EVENT_PRESS_LOST, nullptr);

  infoRow(tab, "Dark theme");
  sw_dark = lv_switch_create(tab);
  lv_obj_add_event_cb(sw_dark, dark_evt, LV_EVENT_VALUE_CHANGED, nullptr);

  infoRow(tab, "Detail bars (Opus / Sonnet / $)");
  sw_bars = lv_switch_create(tab);
  lv_obj_add_event_cb(sw_bars, bars_evt, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *flipbtn = lv_btn_create(tab);
  lv_obj_add_event_cb(flipbtn, flip_evt, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *fl = lv_label_create(flipbtn);
  lv_label_set_text(fl, LV_SYMBOL_LOOP " Flip screen 180");
  lv_obj_center(fl);

  lbl_fw = lv_label_create(tab);
  lv_obj_set_style_text_font(lbl_fw, &lv_font_montserrat_12, 0);
  lv_label_set_text(lbl_fw, FW_VERSION);
}

// ---- public -----------------------------------------------------------------
void uiInit() {
  lv_style_init(&s_style_text);
  lv_style_init(&s_style_track);
  lv_style_set_bg_opa(&s_style_track, LV_OPA_COVER);

  s_tv = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 36);

  s_page_usage = lv_tabview_add_tab(s_tv, LV_SYMBOL_BARS " Usage");
  buildUsageTab(s_page_usage);
  s_page_limits = lv_tabview_add_tab(s_tv, LV_SYMBOL_LIST " Limits");
  buildLimitsTab(s_page_limits);
  s_page_info = lv_tabview_add_tab(s_tv, LV_SYMBOL_SETTINGS " Info");
  buildInfoTab(s_page_info);

  s_dark = loadDarkPref();
  if (s_dark) lv_obj_add_state(sw_dark, LV_STATE_CHECKED);
  applyTheme(s_dark);

  bool barsOn = loadBarsPref();
  if (barsOn) lv_obj_add_state(sw_bars, LV_STATE_CHECKED);
  setDetailBarsVisible(barsOn);
}

void uiUpdate(const UsageData &d) {
  if (d.org_name[0] || d.name[0])
    lv_label_set_text_fmt(lbl_header, "%s  -  %s", d.name[0] ? d.name : "?",
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
