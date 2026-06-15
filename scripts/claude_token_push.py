#!/usr/bin/env python3
"""
claude_token_push.py — show the Claude.ai auth blob and push it to the CYD.

This is the "update the token from the PC" half. It extracts the working
Firefox cookies (via claude_token_export.py), prints a human-readable summary
to the screen, and — with --serial — sends the blob to the ESP32/CYD over USB
so the firmware can refresh its stored token without re-flashing.

Serial line protocol (newline-delimited, 115200 baud):
    ->  PING
    <-  PONG <fw-version>
    ->  TOKEN <base64-of-compact-json>
    <-  OK TOKEN <bytes-stored>        (or)  ERR <reason>
    ->  STATUS
    <-  STATUS wifi=<0|1> ip=<...> last=<ok|err|never> org=<name>
    ->  REFRESH
    <-  OK REFRESH

Usage:
    claude_token_push.py                  # just print the blob + summary (no device)
    claude_token_push.py --serial         # autodetect CYD port and push
    claude_token_push.py --serial /dev/ttyUSB0
    claude_token_push.py --serial --status   # push, then query device status
    claude_token_push.py --serial --refresh  # push, then trigger an immediate fetch
    claude_token_push.py --full           # ship every claude.ai cookie (default: essential)
    claude_token_push.py --baud 115200

DEPENDS: claude_token_export.py (same dir), pyserial
"""

import argparse
import importlib.util
import sys
import time
from pathlib import Path

# Load the sibling exporter regardless of CWD / PATH ordering.
_EXPORT_PATH = Path(__file__).resolve().parent / "claude_token_export.py"
_spec = importlib.util.spec_from_file_location("claude_token_export", _EXPORT_PATH)
exporter = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(exporter)

BANNER_WAIT = 2.0  # seconds to let the ESP32 finish (re)booting after port open


def autodetect_port() -> str | None:
    """Pick the most likely CYD serial port (CH340 / CP210x / generic USB UART)."""
    try:
        from serial.tools import list_ports
    except ImportError:
        return None
    ports = list(list_ports.comports())
    if not ports:
        return None
    keys = ("ch340", "cp210", "silicon labs", "qinheng", "usb-serial",
            "usb serial", "ft232", "esp32")
    for p in ports:
        hay = f"{p.description} {p.manufacturer or ''} {p.product or ''}".lower()
        if any(k in hay for k in keys):
            return p.device
    # Fall back to the first ttyUSB/ttyACM we see.
    for p in ports:
        if "ttyusb" in p.device.lower() or "ttyacm" in p.device.lower():
            return p.device
    return ports[0].device


def _send_line(ser, line: str) -> None:
    ser.write((line + "\n").encode("utf-8"))
    ser.flush()


def _read_reply(ser, timeout: float = 8.0, expect_prefixes=("OK", "ERR")) -> str:
    """Read lines until one starts with an expected prefix or timeout."""
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", "replace").strip()
        if not line:
            continue
        last = line
        if any(line.startswith(p) for p in expect_prefixes):
            return line
        # Anything else (boot logs, banners) is echoed so the user can see it.
        print(f"    cyd: {line}")
    return last or "(no reply — timed out)"


def push_serial(blob: dict, port: str, baud: int,
                do_status: bool, do_refresh: bool) -> int:
    try:
        import serial  # pyserial
    except ImportError:
        print("Error: pyserial not installed.  pacman -S python-pyserial", file=sys.stderr)
        return 1

    b64 = exporter.blob_base64(blob)
    print(f"\nOpening {port} @ {baud} …")
    try:
        ser = serial.Serial(port, baud, timeout=1)
    except serial.SerialException as e:
        print(f"Error: cannot open {port}: {e}", file=sys.stderr)
        return 1

    with ser:
        # Opening the port usually resets the board — give it time, then flush.
        time.sleep(BANNER_WAIT)
        ser.reset_input_buffer()

        _send_line(ser, "PING")
        pong = _read_reply(ser, timeout=4.0, expect_prefixes=("PONG",))
        if pong.startswith("PONG"):
            print(f"  device: {pong}")
        else:
            print(f"  warning: no PONG (got: {pong!r}). Pushing anyway…")

        print(f"  sending TOKEN ({len(b64)} b64 bytes / {len(exporter.blob_json(blob))} json) …")
        _send_line(ser, f"TOKEN {b64}")
        reply = _read_reply(ser, timeout=10.0)
        print(f"  -> {reply}")
        ok = reply.startswith("OK")

        if ok and do_refresh:
            _send_line(ser, "REFRESH")
            print(f"  -> {_read_reply(ser, timeout=20.0)}")
        if ok and do_status:
            _send_line(ser, "STATUS")
            print(f"  -> {_read_reply(ser, timeout=6.0, expect_prefixes=('STATUS', 'ERR'))}")

    return 0 if ok else 2


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial", nargs="?", const="AUTO", metavar="PORT",
                    help="push to the CYD (autodetect port if none given)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--full", action="store_true",
                    help="ship every claude.ai cookie (default: essential set only)")
    ap.add_argument("--no-verify", dest="verify", action="store_false",
                    help="skip the live /account + /usage check before pushing")
    ap.add_argument("--status", action="store_true", help="query device status after push")
    ap.add_argument("--refresh", action="store_true",
                    help="trigger an immediate API fetch on the device after push")
    args = ap.parse_args()

    try:
        blob = exporter.build_blob(verify=args.verify, essential_only=not args.full)
    except Exception as e:
        print(f"Error extracting token: {e}", file=sys.stderr)
        sys.exit(1)

    exporter.print_summary(blob)

    if args.serial is None:
        print("(no --serial given — printed only. Re-run with --serial to push.)\n")
        return

    port = args.serial
    if port == "AUTO":
        port = autodetect_port()
        if not port:
            print("Error: no serial port found. Pass one explicitly: --serial /dev/ttyUSB0",
                  file=sys.stderr)
            sys.exit(1)
        print(f"Autodetected port: {port}")

    sys.exit(push_serial(blob, port, args.baud, args.status, args.refresh))


if __name__ == "__main__":
    main()
