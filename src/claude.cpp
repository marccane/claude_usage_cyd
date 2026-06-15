// claude.cpp — stores the pushed auth blob in NVS and fetches usage/limits.
//
// The blob is the compact JSON produced by claude_token_export.py on the PC:
//   {base_url, user_agent, cookie, org_id, org_name, name, exported_at}
// We keep the raw JSON in NVS and re-parse it on boot.
#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

#include "claude.h"
#include "config.h"

// ---- in-memory config (parsed from the blob) -------------------------------
static bool s_loaded = false;
static String s_baseUrl;
static String s_userAgent;
static String s_cookie;
static String s_orgId;
static String s_orgName;
static String s_userName;

// ---------------------------------------------------------------------------
static bool parseBlobInto(const String &json) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.printf("blob parse error: %s\n", err.c_str());
    return false;
  }
  const char *cookie = doc["cookie"] | "";
  const char *ua = doc["user_agent"] | "";
  if (strlen(cookie) == 0 || ua == nullptr || strlen(ua) == 0) {
    Serial.println("blob missing cookie/user_agent");
    return false;
  }
  s_cookie = cookie;
  s_userAgent = ua;
  s_baseUrl = doc["base_url"] | "https://claude.ai/api";
  s_orgId = doc["org_id"] | "";
  s_orgName = doc["org_name"] | "";
  s_userName = doc["name"] | "";
  s_loaded = true;
  return true;
}

bool claudeLoadToken() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);  // read-only
  String json = prefs.getString(NVS_KEY_BLOB, "");
  prefs.end();
  if (json.isEmpty()) return false;
  return parseBlobInto(json);
}

bool claudeSaveBlob(const String &json) {
  if (!parseBlobInto(json)) return false;  // validate before persisting
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  size_t n = prefs.putString(NVS_KEY_BLOB, json);
  prefs.end();
  Serial.printf("stored blob: %u bytes\n", (unsigned)n);
  return n > 0;
}

bool claudeHasToken() { return s_loaded; }
const char *claudeOrgName() { return s_orgName.c_str(); }
const char *claudeUserName() { return s_userName.c_str(); }

// ---- Set-Cookie refresh (best effort, in-memory only) ----------------------
// Cloudflare rotates __cf_bm (and sometimes cf_clearance) on responses. Keep the
// in-memory cookie fresh within a session so we don't 403 after ~30 min. The
// persisted blob is untouched; a reboot reloads whatever the PC last pushed.
static void mergeCookie(const String &setCookie) {
  int eq = setCookie.indexOf('=');
  if (eq <= 0) return;
  String name = setCookie.substring(0, eq);
  name.trim();
  int semi = setCookie.indexOf(';', eq);
  String pair = (semi < 0) ? setCookie.substring(0)
                           : setCookie.substring(0, semi);
  pair.trim();
  if (name != "__cf_bm" && name != "cf_clearance") return;  // only the CF ones

  int at = s_cookie.indexOf(name + "=");
  if (at < 0) {
    s_cookie += "; " + pair;
    return;
  }
  int end = s_cookie.indexOf("; ", at);
  if (end < 0) end = s_cookie.length();
  s_cookie = s_cookie.substring(0, at) + pair + s_cookie.substring(end);
}

// ---- HTTP -------------------------------------------------------------------
static int httpGetJson(const String &url, String &body) {
  WiFiClientSecure client;
  client.setInsecure();  // skip cert validation (no CA bundle on device)

  HTTPClient http;
  if (!http.begin(client, url)) return -1;
  http.setUserAgent(s_userAgent);
  http.addHeader("Accept", "application/json");
  http.addHeader("Referer", "https://claude.ai/");
  http.addHeader("Cookie", s_cookie);
  const char *collect[] = {"Set-Cookie"};
  http.collectHeaders(collect, 1);
  http.setTimeout(15000);

  int code = http.GET();
  if (code > 0) {
    body = http.getString();
    for (int i = 0; i < http.headers(); i++) {
      if (http.headerName(i).equalsIgnoreCase("Set-Cookie")) {
        mergeCookie(http.header(i));
      }
    }
  }
  http.end();
  return code;
}

// Parse "2026-06-15T18:30:00..." (UTC) -> seconds from now. -1 if unknown.
// Requires the system TZ to be UTC (configTime(0,0,...) in setup()).
static long resetInSeconds(const char *iso) {
  if (!iso || strlen(iso) < 19) return -1;
  struct tm tm = {};
  int y, mo, d, h, mi, s;
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return -1;
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = s;
  time_t when = mktime(&tm);
  time_t now = time(nullptr);
  if (now < 1700000000) return -1;  // clock not synced yet
  long delta = (long)(when - now);
  return delta < 0 ? 0 : delta;
}

static void fillWindow(JsonObjectConst obj, UsageWindow &w) {
  if (obj.isNull()) return;
  w.present = true;
  w.util = obj["utilization"] | 0.0f;
  w.resets_in = resetInSeconds(obj["resets_at"] | "");
}

bool claudeFetch(UsageData &out) {
  out = UsageData();  // reset
  if (!s_loaded) {
    strncpy(out.error, "no token (push from PC)", sizeof(out.error) - 1);
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    strncpy(out.error, "WiFi not connected", sizeof(out.error) - 1);
    return false;
  }

  strncpy(out.org_name, s_orgName.c_str(), sizeof(out.org_name) - 1);
  strncpy(out.name, s_userName.c_str(), sizeof(out.name) - 1);

  // Resolve org id if the PC didn't include one.
  if (s_orgId.isEmpty()) {
    String body;
    int code = httpGetJson(s_baseUrl + "/account", body);
    out.http_status = code;
    if (code != 200) {
      snprintf(out.error, sizeof(out.error), "account HTTP %d", code);
      return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      strncpy(out.error, "account parse failed", sizeof(out.error) - 1);
      return false;
    }
    for (JsonObjectConst m : doc["memberships"].as<JsonArrayConst>()) {
      JsonObjectConst org = m["organization"].as<JsonObjectConst>();
      for (JsonVariantConst cap : org["capabilities"].as<JsonArrayConst>()) {
        if (strcmp(cap | "", "raven") == 0) {
          s_orgId = org["uuid"] | "";
          s_orgName = org["name"] | "";
          strncpy(out.org_name, s_orgName.c_str(), sizeof(out.org_name) - 1);
          break;
        }
      }
    }
    if (s_orgId.isEmpty()) {
      strncpy(out.error, "no team org found", sizeof(out.error) - 1);
      return false;
    }
  }

  // ---- usage ----
  {
    String body;
    int code = httpGetJson(s_baseUrl + "/organizations/" + s_orgId + "/usage", body);
    out.http_status = code;
    if (code != 200) {
      snprintf(out.error, sizeof(out.error),
               code == 403 ? "usage HTTP 403 (Cloudflare? refresh token)" : "usage HTTP %d",
               code);
      return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      strncpy(out.error, "usage parse failed", sizeof(out.error) - 1);
      return false;
    }
    fillWindow(doc["five_hour"].as<JsonObjectConst>(), out.five_hour);
    fillWindow(doc["seven_day"].as<JsonObjectConst>(), out.seven_day);
    fillWindow(doc["seven_day_opus"].as<JsonObjectConst>(), out.opus);
    fillWindow(doc["seven_day_sonnet"].as<JsonObjectConst>(), out.sonnet);

    JsonObjectConst ex = doc["extra_usage"].as<JsonObjectConst>();
    if (!ex.isNull() && (ex["is_enabled"] | false)) {
      out.extra.enabled = true;
      out.extra.util = ex["utilization"] | -1.0f;
      if (!ex["used_credits"].isNull())
        out.extra.used = (float)(ex["used_credits"] | 0) / 100.0f;
      if (!ex["monthly_limit"].isNull())
        out.extra.total = (float)(ex["monthly_limit"] | 0) / 100.0f;
      strncpy(out.extra.currency, ex["currency"] | "", sizeof(out.extra.currency) - 1);
      if (out.extra.util < 0 && out.extra.used >= 0 && out.extra.total > 0)
        out.extra.util = out.extra.used / out.extra.total * 100.0f;
    }
  }

  // ---- rate limits (non-fatal) ----
  {
    String body;
    int code = httpGetJson(s_baseUrl + "/organizations/" + s_orgId + "/rate_limits", body);
    if (code == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, body)) {
        for (JsonObjectConst l : doc["tier_model_rate_limiters"].as<JsonArrayConst>()) {
          if (out.n_limits >= MAX_LIMITS) break;
          RateLimit &rl = out.limits[out.n_limits++];
          strncpy(rl.model_group, l["model_group"] | "", sizeof(rl.model_group) - 1);
          strncpy(rl.limiter, l["limiter"] | "", sizeof(rl.limiter) - 1);
          rl.value = l["value"] | 0;
        }
      }
    }
  }

  out.valid = true;
  out.fetched_at = time(nullptr);
  out.error[0] = '\0';
  return true;
}
