# Quirks, landmines and hard-won knowledge

Everything that cost real debugging time on this project. Read before changing
auth, the UI formatting, or the serial protocol. Last updated 2026-09-13.

Companion doc on the PC side: `~/.scripts/CLAUDE-TOKEN-SOURCES.md`.

---

## 1. Two ways to authenticate, and the deliberate `main`/branch split

| | `browser` (cookies) | `code` (Claude Code OAuth) |
|---|---|---|
| Credential | Firefox `claude.ai` cookies: `sessionKey` + Cloudflare `cf_clearance`/`__cf_bm` | `claudeAiOauth.accessToken` from `~/.claude/.credentials.json` |
| Endpoint | `claude.ai/api/organizations/<org>/usage` | `api.anthropic.com/api/oauth/usage` |
| Auth header | `Cookie:` | `Authorization: Bearer` + `anthropic-beta: oauth-2025-04-20` |
| Lifetime | weeks (sessionKey); CF cookies hours | **~8 h** after the CLI last refreshed it |
| Rate limited | not in practice | **yes, aggressively** — see §2 |
| Per-model limits | yes (`/rate_limits`) | no such endpoint → *Limits* tab stays empty |
| Blob size | ~1.2 KB | ~350 B |

**`main` supports only the cookie path, on purpose.** The Claude Code token
support lives on the branch `claude-code-token` (here and in `~/.scripts`).
Reason: an ~8 h expiry plus a 429-happy endpoint is a bad fit for an always-on
wall display. Don't merge it to `main` without revisiting that trade-off.

The pushed blob carries an `auth` field (`"cookie"` or `"oauth"`) on the branch.
**A blob with no `auth` field is read as a cookie blob**, so blobs already
sitting in a device's NVS keep working after a firmware upgrade — don't break
that default.

The two sources can report *different numbers* legitimately: Firefox and Claude
Code may be logged in as different people on the same team.

---

## 2. The OAuth endpoint rate-limits you into a corner

`api.anthropic.com/api/oauth/usage` returns:

```
HTTP 429   retry-after: 195
{"error":{"type":"rate_limit_error","message":"Rate limited. Please try again later."}}
```

Three consumers share one account budget, and any two of them together will keep
you permanently throttled:

- the CYD — `REFRESH_MS` is **180 s**, i.e. *just inside* the ~195 s window;
- `claudeUsageWatch` — `watch -n 180` on the PC;
- **every** `claude_token_export.py` / `claude_token_push.py` run, because each
  verifies the blob against the live API before shipping it. `--no-verify` skips
  that call.

A crash loop makes this much worse: each reboot fires another fetch, which is
how a genuine 200 window got burned through in seconds (see §4).

The cookie path talks to claude.ai instead and is not subject to this.

**How the device reacts (branch `claude-code-token`).** A 429 carries no new
figures, so blanking the screen would throw away good data for a transient
condition. Instead `loop()` keeps the previous `UsageData` and only sets
`throttled`, so the bars stay put; the header gains an orange ` - Throttled`
after the org name, and the RGB LED goes amber. The *Updated:* timestamp keeps
showing the last **successful** fetch, which is the honest staleness signal. The
flag clears automatically on the next good fetch. `STATUS` reports it as `thr=1`.

Note the merge only kicks in when there is something to keep — a 429 with no
prior valid result still shows the error screen.

Check how long the OAuth token has left:

```bash
date -d @$(( $(jq .claudeAiOauth.expiresAt ~/.claude/.credentials.json) / 1000 ))
```

Other OAuth payload differences that the firmware has to tolerate: no org id
(the endpoint is account-scoped, so skip org resolution), no `/rate_limits`, and
`extra_usage.monthly_limit` is `null` — there is no budget total to render, only
the amount used.

---

## 3. Cloudflare vs the ESP32 (cookie path)

`cf_clearance` is bound to the original client's IP **and TLS fingerprint**. The
ESP32's handshake is not Firefox's, so claude.ai's bot management may answer
`403` even with a perfectly valid `sessionKey`. If it persists:

- keep the CYD on the same LAN / public IP as the PC;
- push with `--full` (ship every cookie) instead of `--essential`;
- last resort: push a blob whose `base_url` points at a small PC-side proxy —
  the firmware uses `base_url` verbatim, so no reflash is needed.

`__cf_bm`/`cf_clearance` are re-merged in RAM from response `Set-Cookie` headers
to survive rotation within a session; the NVS copy is untouched, so a reboot
reloads whatever the PC last pushed.

TLS cert validation is **off** (`setInsecure()`) — fine for a LAN gadget, but it
means a MITM would go unnoticed.

---

## 4. ⚠ Never put `%f` in an `lv_*_set_text_fmt` call

This one crash-looped the device for minutes and looked exactly like an auth
bug. `lv_label_set_text_fmt` routes through **LVGL's own** printf, which only
handles floats when `LV_SPRINTF_USE_FLOAT` is set. LVGL 8.4 defaults it to `0`
and `include/lv_conf.h` never sets it.

So in:

```c
lv_label_set_text_fmt(lbl, "%.2f/%.2f %s", used, total, currency);   // DON'T
```

neither `%.2f` consumes its `double`. The trailing `%s` then reads the wrong
vararg slot and dereferences it:

```
Guru Meditation Error: Core 1 panic'ed (LoadProhibited). Exception was unhandled.
EXCVADDR: 0x00000000
Rebooting...            # ~every 10 s: boot -> WiFi -> fetch OK -> crash
```

Do this instead (fixed in `src/ui.cpp`, commit `7515e5c`):

```c
char buf[40];
snprintf(buf, sizeof(buf), "%.2f/%.2f %s", used, total, currency);
lv_label_set_text(lbl, buf);
```

**Why it hid for so long:** that line only runs after a *successful* fetch with
extra usage enabled. While the stored token was stale, every fetch failed early
and the UI never reached it. The bug surfaced the moment auth started working —
so a "new auth method broke the device" conclusion would have been wrong.

Corollary: a latent crash behind a success path is invisible for exactly as long
as the thing in front of it is broken. When a fix suddenly produces panics,
suspect the newly-reachable code, not only the fix.

---

## 5. Serial protocol quirks

| PC sends | CYD replies |
|---|---|
| `PING` | `PONG claude-cyd 1.0` |
| `TOKEN <base64>` | `OK TOKEN <bytes>` / `ERR <reason>` |
| `STATUS` | `STATUS wifi=1 ip=… org=… last=ok fetching=0` |
| `REFRESH` | `OK REFRESH` |

- **Opening the port resets the board** (RTS). Every probe therefore reboots the
  device and restarts its boot → WiFi → fetch cycle. To observe behaviour over
  time, hold **one** connection open and poll `STATUS` on it, instead of
  reconnecting — otherwise you only ever measure the first 4 seconds of uptime.
- **`REFRESH` returns `OK` immediately**; the fetch runs on another task. A
  `STATUS` sent right after races it and reports the *previous* result. Seeing a
  stale `last=err` straight after a push means "not finished yet", not "failed".
  Wait for `fetching=0` *and* allow a few seconds.
- `TOKEN` lines are ~1.6 KB of base64 — `Serial.setRxBufferSize(8192)` in
  `setup()` exists for that; don't shrink it.
- On the `claude-code-token` branch `STATUS` also reports `http=<code>` and
  `err=<text>`. Without those, a failure is just `last=err` and you have to read
  the LCD to learn why. Worth porting to `main` if you ever debug there.
- The USB serial device can **change or disappear** (`/dev/ttyUSB1` vanished
  mid-session and only `ttyUSB0` — a different device — was left). Always
  confirm the port before flashing: `udevadm info -q property -n /dev/ttyUSBx`
  (CYD is a CH340: `ID_VENDOR_ID=1a86`, `ID_MODEL_ID=7523`). Flashing the wrong
  port writes firmware to whatever else is plugged in.
- CLI argument order: the source is a **positional** arg, so put it before the
  flags — `claude_token_push.py browser --serial /dev/ttyUSB1`. Writing
  `--serial browser` would swallow `browser` as the port name.

---

## 6. `secrets.h` is git-ignored, so its format changes bite silently

`secrets.h` moved from a single `WIFI_SSID`/`WIFI_PASS` pair to a `WIFI_CREDS[]`
list (multi-AP, strongest-signal-wins). Because the real file is git-ignored,
pulling that change leaves your local copy in the old format and the build dies
with something that looks unrelated:

```
src/main.cpp:92:28: error: 'WIFI_CRED_COUNT' was not declared in this scope
src/main.cpp:93:19: error: 'WIFI_CREDS' was not declared in this scope
```

Fix: re-copy `src/secrets.h.example` and put your networks back. Whenever you
change the shape of `secrets.h`, update `secrets.h.example` **and** say so in
the commit message, because nobody's local file follows automatically.

---

## 7. Debugging recipes

**Decode a Guru Meditation backtrace.** Copy the `Backtrace:` addresses and run
them against the ELF that produced them (check `ELF file SHA256` in the panic
matches your build):

```bash
~/.platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-addr2line \
  -pfiaC -e .pio/build/cyd/firmware.elf 0x400f51d1 0x400f55a1 0x400e4a7c
```

That turned an opaque `LoadProhibited` into `ui.cpp:347` in one step.

**Watch without rebooting in a loop.** Open the port once, sleep past the boot,
then poll — see §5. Reconnecting per sample resets the board every time.

**Prove auth separately from rendering.** Hit the endpoint from the PC with the
same headers the device uses. If the PC gets `200` and the device does not, it's
the device; if both get `429`, it's the account budget and you just have to wait.

**Don't trust a single `STATUS` after a push.** See the race in §5.

**Force an HTTP error without burning API quota.** The firmware uses `base_url`
verbatim, so you can point it at a local server that returns whatever you want
to test. It must speak **TLS** (the client is a `WiFiClientSecure`), but a
self-signed cert is fine because cert validation is off:

```bash
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem \
        -days 2 -subj "/CN=<pc-ip>"
# serve 429 on :8443, then push a blob with base_url = https://<pc-ip>:8443
```

That is how the throttle handling above was verified end to end: real fetch →
fake 429 (figures kept, `thr=1`) → real blob restored (`thr=0`). Much better
than waiting for the real endpoint to rate-limit you, and it costs no quota.

---

## 8. Miscellaneous

- Time is UTC (`configTime(0, 0, …)`); `resets_at` maths depends on it, and
  `resetInSeconds()` returns `-1` until the clock syncs (`now < 1700000000`).
- No PSRAM on this ESP32-WROOM — the LVGL draw buffer is sized for internal RAM.
- The blob lives in NVS and survives reboots; you only re-push when it expires.
- `scripts/__pycache__/` appears whenever `claude_token_push.py` imports the
  exporter. It is untracked noise; leave it or add it to `.gitignore`.
