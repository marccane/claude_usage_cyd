# Claude.ai usage monitor — CYD edition

Shows your Claude.ai usage (the same data as `claudeUsageWatch` / `claude_usage.py`)
on a **CYD** (Cheap Yellow Display, ESP32-2432S028) with an LVGL UI and touch.

```
   PC (Firefox logged in)                          CYD (ESP32 + 2.8" LCD)
 ┌──────────────────────────┐                   ┌───────────────────────────┐
 │ claude_token_export.py   │   USB serial      │  stores blob in NVS        │
 │  reads cookies.sqlite ───┼── TOKEN <b64> ───►│  fetches /usage over HTTPS │
 │ claude_token_push.py     │                   │  draws LVGL bars + touch   │
 └──────────────────────────┘                   └───────────────────────────┘
```

The ESP32 has no access to the Firefox cookie DB, so the PC extracts the working
session cookies into a small JSON "blob" and pushes it over USB. The CYD then
calls `claude.ai/api` itself (**direct mode**) and renders the result.

---

## Part 1 — PC scripts (`~/.scripts/`)

Both reuse the cookie logic from `claude_usage.py`.

### `claude_token_export.py` — make the auth blob
Reads the working `claude.ai` cookies (sessionKey + Cloudflare `cf_clearance`/
`__cf_bm`), resolves the team org id, verifies it against the live API, and emits
a compact JSON blob.

```bash
claude_token_export.py            # pretty summary + base64 payload
claude_token_export.py --json     # one-line compact JSON
claude_token_export.py --base64   # base64(JSON) — exactly what gets pushed
claude_token_export.py --essential# only the cookies the API needs (~1.2 KB, default for push)
```

### `claude_token_push.py` — show it / push it to the CYD
```bash
claude_token_push.py                  # just print the blob (no device)
claude_token_push.py --serial         # autodetect the CYD port and push
claude_token_push.py --serial /dev/ttyUSB0
claude_token_push.py --serial --refresh   # push, then force an immediate fetch
claude_token_push.py --serial --status    # push, then read device status
claude_token_push.py --full           # ship every cookie (if --essential gets 403)
```

Serial protocol (115200 baud, newline-delimited):

| PC sends            | CYD replies                                   |
|---------------------|-----------------------------------------------|
| `PING`              | `PONG claude-cyd 1.0`                          |
| `TOKEN <base64>`    | `OK TOKEN <bytes>` / `ERR <reason>`            |
| `STATUS`            | `STATUS wifi=1 ip=… org=… last=ok fetching=0`  |
| `REFRESH`           | `OK REFRESH`                                    |

> The session cookie is long-lived; `cf_clearance`/`__cf_bm` are not. Re-run the
> push whenever the device starts showing `HTTP 403`. A handy alias:
> ```bash
> alias claudeCydPush='claude_token_push.py --serial --refresh'
> ```

---

## Part 2 — Firmware (`~/projects/claude_usage_cyd/`)

PlatformIO + Arduino + LVGL 8 + TFT_eSPI + XPT2046 + ArduinoJson 7.

### Build & flash
`pio` isn't on your PATH yet. Install it user-level (no sudo):

```bash
python3 -m venv ~/.venvs/pio && ~/.venvs/pio/bin/pip install platformio
ln -s ~/.venvs/pio/bin/pio ~/.local/bin/pio      # or add the venv bin to PATH
```

Then:

```bash
cd ~/projects/claude_usage_cyd
cp src/secrets.h.example src/secrets.h     # already created — edit WiFi creds
$EDITOR src/secrets.h
pio run -t upload && pio device monitor
```

First build downloads the ESP32 toolchain (~1 GB) and takes a few minutes.

### First-run flow
1. Flash the firmware and open the monitor.
2. It connects to WiFi, syncs time (NTP, UTC), and reports `no token in NVS`.
3. From the PC: `claude_token_push.py --serial --refresh`.
4. The CYD stores the blob, fetches usage, and the bars appear. The token
   survives reboots (NVS) — you only push again when it expires.

### Display driver — IMPORTANT
`platformio.ini` is set for the **ST7789** panel on the 2-USB / USB-C CYD
revision. If the screen is **blank, garbled, or colour-wrong**:

- Colours inverted (photo-negative) → flip `TFT_INVERSION_ON` ↔ `TFT_INVERSION_OFF`.
- Red/blue swapped → flip `TFT_RGB_ORDER=TFT_BGR` ↔ `TFT_RGB`.
- Totally wrong/blank → it's probably the **classic ILI9341** board: comment the
  three `ST7789` lines and uncomment `-D ILI9341_2_DRIVER`.

### Touch off-target?
Adjust `TOUCH_RAW_MIN/MAX_X/Y` in `include/config.h`. (Resistive XPT2046 on its
own SPI bus — pins in `config.h`.)

---

## The UI

Three swipeable tabs:

- **Usage** — colour-coded bars (green <60 %, amber <85 %, red ≥85 %, same as the
  script) for the 5-hour, 7-day, Opus-7d, Sonnet-7d windows and the extra-usage
  `€` budget, each with its reset countdown.
- **Limits** — per-model-group rate limits (`tier_model_rate_limiters`).
- **Info** — user/org, WiFi+IP, last-update time, next-refresh countdown, a
  **Refresh now** button and a **brightness** slider. Firmware version at the bottom.

On-board RGB LED = health: green = last fetch OK, red = error/no token, blue =
fetching.

## Touchscreen ideas (the panel is resistive, so big targets work best)

Implemented: tab swiping, *Refresh now*, brightness slider.

Easy next steps:
- **Tap a bar** to toggle `%` ↔ absolute (used/limit, exact reset time).
- **Auto-brightness** from the LDR on GPIO34 (`displayReadLDR()` is already wired)
  — dim at night, plus a tap-to-wake screensaver to avoid OLED-style burn-in.
- **Pull-to-refresh** / long-press anywhere = force fetch.
- **History sparkline**: keep the last N samples in RAM and draw an `lv_chart`
  so you can see usage trending toward a limit.
- **Alert mode**: flash the RGB LED red + a full-screen banner when any window
  crosses 90 %, so it's a glanceable "stop coding" light.
- **Multi-account**: long-press the header to cycle stored blobs (personal/work)
  — the export script already supports selecting orgs.
- **Settings page**: on-device WiFi picker so `secrets.h` isn't compiled in.

---

## Notes & caveats

- **Cloudflare (direct mode):** `cf_clearance` is bound to the original client's
  IP and TLS fingerprint. The ESP32's TLS handshake differs from Firefox, so
  claude.ai's bot-management *may* return `403` even with a valid token. If that
  happens persistently:
  - Make sure the CYD is on the **same LAN / public IP** as the PC.
  - Try `--full` (ship every cookie).
  - Fallback: point the firmware at a small PC proxy by pushing a blob whose
    `base_url` is `http://<pc-ip>:8088` — the device code already uses
    `base_url` verbatim, so no reflash is needed. (Proxy server not included yet;
    ask if you want it.)
- **TLS cert validation** is disabled (`setInsecure()`); fine for a hobby LAN
  gadget. Pin the claude.ai CA if you want strict validation.
- `__cf_bm`/`cf_clearance` are refreshed best-effort from response `Set-Cookie`
  headers in RAM to extend longevity within a session (not persisted).
- No PSRAM on the CYD ESP32-WROOM — the LVGL draw buffer is sized for internal RAM.
```
