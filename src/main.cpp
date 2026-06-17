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

// ---- WiFi (multi-AP) --------------------------------------------------------
// On boot and after a drop we scan, pick the strongest WIFI_CREDS entry that's
// actually in range, then poll WiFi.begin()'s status (which is non-blocking).
// The scan itself is SYNCHRONOUS: arduino-esp32 2.0.x's async scan
// (scanNetworks(true)) returns 0 results on this build even though the radio is
// fine, so we take the ~2 s blocking hit. It only happens while disconnected
// (nothing is animating then), and we paint the "Scanning…" frame first.
static bool s_wifiUp = false;

enum WifiPhase { WIFI_IDLE, WIFI_CONNECTING };
static WifiPhase s_wifiPhase = WIFI_IDLE;
static unsigned long s_wifiPhaseMs = 0;

static const unsigned long WIFI_RESCAN_GAP_MS = 3000;   // pause between rounds

// Round-robin cursor for the blind fallback (used if the scan can't name any of
// our SSIDs — e.g. a hidden network or a momentary scan miss).
static size_t s_blindIdx = 0;

static void wifiBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);  // we manage (re)connection via the scan loop
  WiFi.disconnect();
  s_wifiPhase = WIFI_IDLE;
  s_wifiPhaseMs = millis() - WIFI_RESCAN_GAP_MS - 1;  // scan on the first tick
}

// Of the APs the scan found, return the index into WIFI_CREDS of the strongest
// one we have a password for, or -1 if none of our networks are in range.
static int wifiPickKnown(int found) {
  int best = -1, bestRssi = -9999;
  for (int i = 0; i < found; i++) {
    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    for (size_t k = 0; k < WIFI_CRED_COUNT; k++) {
      if (ssid == WIFI_CREDS[k].ssid && rssi > bestRssi) {
        bestRssi = rssi;
        best = (int)k;
      }
    }
  }
  return best;
}

// Blocking: scan, choose the best known AP (or blind-try one), kick off connect.
static void wifiScanAndConnect() {
  uiSetState("Scanning WiFi...");
  lv_timer_handler();                       // paint the frame before we block
  int found = WiFi.scanNetworks(false /* sync */, false /* show_hidden */);
  if (found < 0) found = 0;                 // WIFI_SCAN_FAILED -> nothing

  // Log everything the radio saw — quickest way to spot a 5 GHz-only AP
  // (invisible to the ESP32) or an SSID typo.
  Serial.printf("WiFi: scan found %d AP(s):\n", found);
  for (int i = 0; i < found; i++) {
    String s = WiFi.SSID(i);
    Serial.printf("  %2d) rssi=%4d  ch=%2d  %s\n", i, WiFi.RSSI(i),
                  WiFi.channel(i), s.length() ? s.c_str() : "<hidden>");
  }

  int k = wifiPickKnown(found);
  WiFi.scanDelete();
  if (k < 0) {
    // None of our SSIDs were named in the scan. Don't give up — blind-try each
    // credential in turn (covers hidden SSIDs and momentary scan misses).
    k = (int)(s_blindIdx % WIFI_CRED_COUNT);
    s_blindIdx++;
    Serial.printf("WiFi: no listed SSID in scan; blind try %s\n",
                  WIFI_CREDS[k].ssid);
  } else {
    Serial.printf("WiFi: connecting to %s\n", WIFI_CREDS[k].ssid);
  }
  uiSetState((String("Connecting ") + WIFI_CREDS[k].ssid).c_str());
  WiFi.begin(WIFI_CREDS[k].ssid, WIFI_CREDS[k].pass);
  s_wifiPhase = WIFI_CONNECTING;
  s_wifiPhaseMs = millis();
}

// Call ~1x/s. Fetches once the link comes up; rescans if it drops or times out.
static void wifiTick() {
  bool up = (WiFi.status() == WL_CONNECTED);
  if (up != s_wifiUp) {
    s_wifiUp = up;
    uiSetNet(up, up ? WiFi.localIP().toString() : String(""));
    if (up) {
      Serial.printf("WiFi connected: %s (%s)\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      s_wifiPhase = WIFI_IDLE;
      if (claudeHasToken()) requestFetch();
    } else {
      Serial.println("WiFi lost");
      s_wifiPhase = WIFI_IDLE;
      s_wifiPhaseMs = millis();
    }
  }
  if (up) return;

  switch (s_wifiPhase) {
    case WIFI_CONNECTING:
      if (millis() - s_wifiPhaseMs > WIFI_TIMEOUT_MS) {
        Serial.println("WiFi: connect timed out, rescanning");
        WiFi.disconnect();
        s_wifiPhase = WIFI_IDLE;
        s_wifiPhaseMs = millis();
      }
      break;
    case WIFI_IDLE:
    default:
      if (millis() - s_wifiPhaseMs > WIFI_RESCAN_GAP_MS) wifiScanAndConnect();
      break;
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
