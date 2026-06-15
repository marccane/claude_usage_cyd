// main.cpp — Claude.ai usage monitor for the CYD.
//
// Boot -> WiFi -> NTP (UTC) -> load NVS token -> background fetch every 180 s.
// LVGL runs in loop(); the (slow, blocking) HTTPS fetch runs in its own task so
// the touch UI stays responsive. The PC pushes token updates over USB serial.
#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>
#include <mbedtls/base64.h>
#include <time.h>

#include "config.h"
#include "secrets.h"
#include "display.h"
#include "claude.h"
#include "ui.h"

// ---- shared state between loop() (UI) and the fetch task -------------------
static UsageData g_result;             // last good/attempted result (owned by loop)
static volatile bool g_fetchRequested = false;
static volatile bool g_fetching = false;
static volatile bool g_resultReady = false;
static UsageData g_pending;            // written by fetch task, read once by loop
static unsigned long g_lastFetchMs = 0;
static String g_serialLine;

// ---- on-board RGB LED (active LOW) -----------------------------------------
static void setLed(bool r, bool g, bool b) {
  digitalWrite(LED_R, r ? LOW : HIGH);
  digitalWrite(LED_G, g ? LOW : HIGH);
  digitalWrite(LED_B, b ? LOW : HIGH);
}

// ---- background fetch task --------------------------------------------------
static void fetchTask(void *) {
  for (;;) {
    if (g_fetchRequested) {
      g_fetchRequested = false;
      g_fetching = true;
      setLed(false, false, true);          // blue = fetching
      claudeFetch(g_pending);
      g_resultReady = true;
      g_fetching = false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

static void requestFetch() {
  if (!g_fetching) {
    g_fetchRequested = true;
    g_lastFetchMs = millis();
    uiSetState("Updating...");
  }
}

// ---- WiFi (non-blocking) ----------------------------------------------------
// A blocking connect would starve pollSerial() and the touch UI whenever the AP
// is down, so we kick off WiFi.begin() and just poll status from loop().
static bool s_wifiUp = false;
static unsigned long s_wifiAttemptMs = 0;

static void wifiBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  s_wifiAttemptMs = millis();
}

// Call ~1x/s. Never blocks. Fetches once the link comes up; retries if it drops.
static void wifiTick() {
  bool up = (WiFi.status() == WL_CONNECTED);
  if (up != s_wifiUp) {
    s_wifiUp = up;
    uiSetNet(up, up ? WiFi.localIP().toString() : String(""));
    if (up) {
      Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
      if (claudeHasToken()) requestFetch();
    } else {
      Serial.println("WiFi lost");
    }
  }
  if (!up && millis() - s_wifiAttemptMs > 15000) {
    WiFi.disconnect();
    wifiBegin();
  }
}

// ---- serial token protocol --------------------------------------------------
static bool decodeBase64(const String &b64, String &out) {
  size_t cap = (b64.length() * 3) / 4 + 4;
  uint8_t *buf = (uint8_t *)malloc(cap + 1);
  if (!buf) return false;
  size_t olen = 0;
  int rc = mbedtls_base64_decode(buf, cap, &olen,
                                 (const uint8_t *)b64.c_str(), b64.length());
  if (rc != 0) {
    free(buf);
    return false;
  }
  buf[olen] = 0;
  out = String((char *)buf);
  free(buf);
  return true;
}

static void handleLine(String line) {
  line.trim();
  if (line.isEmpty()) return;

  if (line.startsWith("PING")) {
    Serial.println("PONG " FW_VERSION);

  } else if (line.startsWith("TOKEN ")) {
    if (g_fetching) {
      Serial.println("ERR busy (fetching) - retry");
      return;
    }
    String json;
    if (!decodeBase64(line.substring(6), json)) {
      Serial.println("ERR base64 decode failed");
      return;
    }
    if (claudeSaveBlob(json)) {
      Serial.printf("OK TOKEN %u\n", (unsigned)json.length());
      uiSetState("Token updated");
      requestFetch();
    } else {
      Serial.println("ERR invalid blob (need cookie + user_agent)");
    }

  } else if (line.startsWith("STATUS")) {
    Serial.printf("STATUS wifi=%d ip=%s org=%s last=%s fetching=%d\n",
                  WiFi.status() == WL_CONNECTED ? 1 : 0,
                  WiFi.localIP().toString().c_str(),
                  claudeOrgName(),
                  g_result.valid ? "ok" : (g_result.error[0] ? "err" : "never"),
                  g_fetching ? 1 : 0);

  } else if (line.startsWith("REFRESH")) {
    requestFetch();
    Serial.println("OK REFRESH");

  } else {
    Serial.println("ERR unknown command");
  }
}

static void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (g_serialLine.length()) {
        handleLine(g_serialLine);
        g_serialLine = "";
      }
    } else if (g_serialLine.length() < 6000) {  // guard against runaway
      g_serialLine += c;
    }
  }
}

// ---- setup / loop -----------------------------------------------------------
void setup() {
  Serial.setRxBufferSize(8192);  // a pushed TOKEN line can be ~1.5-4 KB
  Serial.begin(115200);
  delay(200);
  Serial.println("\n" FW_VERSION " booting");
  Serial.println("serial cmds: PING | TOKEN <b64> | STATUS | REFRESH");

  displayInit();
  uiInit();
  uiSetState("Connecting WiFi...");
  lv_timer_handler();

  wifiBegin();                   // non-blocking; loop() polls the result
  configTime(0, 0, NTP_SERVER);  // UTC; resets_at math depends on this

  if (claudeLoadToken()) {
    Serial.printf("token loaded (org=%s)\n", claudeOrgName());
    // The fetch is kicked off automatically once WiFi comes up (wifiTick).
  } else {
    Serial.println("no token in NVS - push one: claude_token_push.py --serial");
    uiSetState("No token - push from PC");
    setLed(true, false, false);  // red
  }

  g_lastFetchMs = millis();
  xTaskCreatePinnedToCore(fetchTask, "fetch", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  lv_timer_handler();
  pollSerial();

  // Apply a completed background fetch to the UI (LVGL touched only here).
  if (g_resultReady) {
    g_resultReady = false;
    g_result = g_pending;
    uiUpdate(g_result);
    setLed(!g_result.valid, g_result.valid, false);  // green ok / red error
    uiSetNet(WiFi.status() == WL_CONNECTED, WiFi.localIP().toString());
  }

  // Periodic refresh + reconnect.
  static unsigned long lastTick = 0;
  if (millis() - lastTick >= 1000) {
    lastTick = millis();
    wifiTick();

    long sinceFetch = (long)(millis() - g_lastFetchMs);
    long secsLeft = (long)(REFRESH_MS / 1000) - sinceFetch / 1000;
    uiTick(g_fetching ? -1 : secsLeft);

    if (claudeHasToken() && s_wifiUp && !g_fetching &&
        (unsigned long)sinceFetch >= REFRESH_MS) {
      requestFetch();
    }
  }

  if (uiTakeRefreshRequest()) requestFetch();

  delay(5);
}
