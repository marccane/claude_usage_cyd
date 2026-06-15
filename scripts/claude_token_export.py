#!/usr/bin/env python3
"""
claude_token_export.py — extract the minimal Claude.ai auth blob for the ESP32/CYD.

The CYD (Cheap Yellow Display) has no access to the Firefox cookie DB, so this
pulls the *working* claude.ai cookies (sessionKey + Cloudflare cf_clearance/
__cf_bm + friends), resolves the team org id, verifies the blob actually works
against the API, and emits a compact JSON object the firmware stores in NVS.

It deliberately reuses the same cookie-reading approach as claude_usage.py — the
cookies that make that script work are exactly the ones the ESP32 needs.

Blob schema (compact JSON):
  {
    "base_url":   "https://claude.ai/api",
    "user_agent": "Mozilla/5.0 (... Firefox/138.0)",
    "cookie":     "sessionKey=...; cf_clearance=...; __cf_bm=...; ...",
    "org_id":     "<uuid>",
    "org_name":   "Acme Inc",
    "name":       "Ada",
    "exported_at":"2026-06-15T12:00:00+00:00"
  }

Usage:
  claude_token_export.py                 # pretty summary + the compact JSON
  claude_token_export.py --json          # ONLY the compact JSON (one line)
  claude_token_export.py --base64        # base64(JSON) — exactly what gets pushed
  claude_token_export.py -o blob.json    # write compact JSON to a file
  claude_token_export.py --no-verify     # skip the live API check
  claude_token_export.py --essential     # only ship cookies the API truly needs

CALLERS: claude_token_push.py, ~/.aliases (optional)
SEE ALSO: claude_usage.py (the original Firefox-only viewer)
"""

import argparse
import base64
import json
import shutil
import sqlite3
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

import urllib.request
import urllib.error

CLAUDE_API = "https://claude.ai/api"

# Must match a User-Agent that Cloudflare has already issued cf_clearance for.
# cf_clearance is bound to IP + UA (and sometimes TLS fingerprint); the ESP32 has
# to send this exact string or Cloudflare may 403 it. Kept identical to
# claude_usage.py, which is known-good on this machine.
USER_AGENT = "Mozilla/5.0 (X11; Linux x86_64; rv:138.0) Gecko/20100101 Firefox/138.0"

HEADERS = {
    "User-Agent": USER_AGENT,
    "Accept": "application/json",
    "Referer": "https://claude.ai/",
}

# The cookies the API/Cloudflare actually care about. --essential keeps only
# these to shrink the blob; default ships every claude.ai cookie (known-good).
ESSENTIAL_COOKIES = {
    "sessionKey",
    "sessionKeyLC",
    "cf_clearance",
    "__cf_bm",
    "lastActiveOrg",
    "activitySessionId",
}


def find_firefox_cookies() -> Path:
    candidates = list(Path.home().glob(".mozilla/firefox/*/cookies.sqlite"))
    if not candidates:
        raise FileNotFoundError(
            "No Firefox cookies.sqlite found under ~/.mozilla/firefox/"
        )
    return max(candidates, key=lambda p: p.stat().st_mtime)


def get_claude_cookies(db_path: Path, essential_only: bool = False) -> str:
    """Return a Cookie header value containing claude.ai cookies.

    The sessionKey alone isn't enough: Cloudflare's bot-management needs
    cf_clearance / __cf_bm, and missing them causes intermittent 403s.
    """
    with tempfile.NamedTemporaryFile(suffix=".sqlite", delete=False) as tmp:
        tmp_path = Path(tmp.name)
    shutil.copy2(db_path, tmp_path)
    try:
        conn = sqlite3.connect(tmp_path)
        cur = conn.cursor()
        cur.execute("SELECT name, value FROM moz_cookies WHERE host LIKE '%claude.ai%'")
        rows = cur.fetchall()
        conn.close()
        if not rows:
            raise RuntimeError("No claude.ai cookies found. Are you logged in?")
        if not any(name == "sessionKey" for name, _ in rows):
            raise RuntimeError(
                "No claude.ai sessionKey cookie found. Are you logged in?"
            )
        if essential_only:
            rows = [(n, v) for n, v in rows if n in ESSENTIAL_COOKIES]
        return "; ".join(f"{n}={v}" for n, v in rows)
    finally:
        tmp_path.unlink(missing_ok=True)


def get_json(url: str, cookie_header: str) -> dict:
    req = urllib.request.Request(url, headers={**HEADERS, "Cookie": cookie_header})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"HTTP {e.code} for {url}") from e


def select_team_org(memberships: list) -> dict:
    if not memberships:
        raise RuntimeError("No memberships found on this account.")
    matches = [
        m for m in memberships if "raven" in m["organization"].get("capabilities", [])
    ]
    if not matches:
        available = [m["organization"]["name"] for m in memberships]
        raise RuntimeError(f"No team org found. Available: {available}")
    if len(matches) > 1:
        ambiguous = [m["organization"]["name"] for m in matches]
        raise RuntimeError(f"Ambiguous team match: {ambiguous}")
    return matches[0]["organization"]


def build_blob(verify: bool = True, essential_only: bool = False) -> dict:
    """Extract everything the ESP32 needs to fetch usage on its own."""
    db = find_firefox_cookies()
    cookie = get_claude_cookies(db, essential_only=essential_only)

    org_id = ""
    org_name = ""
    name = ""
    if verify:
        account = get_json(f"{CLAUDE_API}/account", cookie)
        org = select_team_org(account["memberships"])
        org_id = org["uuid"]
        org_name = org["name"]
        name = account.get("display_name") or account.get("full_name", "") or ""
        # Prove the blob can actually reach the usage endpoint before we ship it.
        get_json(f"{CLAUDE_API}/organizations/{org_id}/usage", cookie)

    return {
        "base_url": CLAUDE_API,
        "user_agent": USER_AGENT,
        "cookie": cookie,
        "org_id": org_id,
        "org_name": org_name,
        "name": name,
        "exported_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
    }


def blob_json(blob: dict) -> str:
    """Compact, deterministic JSON — what the firmware parses."""
    return json.dumps(blob, separators=(",", ":"), ensure_ascii=False)


def blob_base64(blob: dict) -> str:
    return base64.b64encode(blob_json(blob).encode("utf-8")).decode("ascii")


def _redact(cookie: str) -> str:
    out = []
    for part in cookie.split("; "):
        n, _, v = part.partition("=")
        out.append(f"{n}={v[:6]}…({len(v)})" if v else n)
    return "; ".join(out)


def print_summary(blob: dict) -> None:
    j = blob_json(blob)
    b64 = blob_base64(blob)
    print(f"\n{'─'*60}")
    print("  Claude.ai → CYD auth blob")
    print(f"{'─'*60}")
    print(f"  User      : {blob['name'] or '(unverified)'}")
    print(f"  Org       : {blob['org_name'] or '(unverified)'}")
    print(f"  Org ID    : {blob['org_id'] or '(unverified)'}")
    print(f"  Base URL  : {blob['base_url']}")
    print(f"  User-Agent: {blob['user_agent']}")
    print(f"  Cookies   : {_redact(blob['cookie'])}")
    print(f"  JSON size : {len(j)} bytes   (NVS string limit ≈ 4000)")
    print(f"  Exported  : {blob['exported_at']}")
    print(f"{'─'*60}")
    print("  base64 (push payload):\n")
    print(f"  {b64}")
    print(f"\n{'─'*60}")
    print("  Push to the CYD:  claude_token_push.py --serial")
    print(f"{'─'*60}\n")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--json", action="store_true", help="print only the compact JSON")
    g.add_argument("--base64", action="store_true", help="print only base64(JSON)")
    ap.add_argument("-o", "--out", metavar="FILE", help="write compact JSON to FILE")
    ap.add_argument("--no-verify", dest="verify", action="store_false",
                    help="skip the live /account + /usage check")
    ap.add_argument("--essential", action="store_true",
                    help="ship only the cookies the API needs (smaller blob)")
    args = ap.parse_args()

    try:
        blob = build_blob(verify=args.verify, essential_only=args.essential)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    if args.out:
        Path(args.out).write_text(blob_json(blob) + "\n", encoding="utf-8")
        print(f"Wrote {args.out} ({len(blob_json(blob))} bytes)", file=sys.stderr)

    if args.json:
        print(blob_json(blob))
    elif args.base64:
        print(blob_base64(blob))
    elif not args.out:
        print_summary(blob)


if __name__ == "__main__":
    main()
