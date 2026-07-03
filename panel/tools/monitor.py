#!/usr/bin/env python3
"""monitor — open a serial console on a specific G6 panel, cross-platform.

Panels are matched by USB vendor ID + product ID + product-string prefix +
serial number via pyserial's port enumeration, which works identically on
Linux/macOS/Windows — not the old monitor.sh's Linux-specific
/dev/serial/by-id/ glob, and not by the stale product string it matched
("RP2354 20x20 Display Panel", which no longer exists on current firmware;
panel/platformio.ini now sets usb_product to "G6 Panel v0.2"/"G6 Panel
v0.3" — same as g6_flash.py). VID+PID alone is NOT enough to identify a
panel: panel_controller/ (the bench harness, one panel acting as a fake SPI
controller for another) intentionally shares the exact same VID:PID
(0x2E8A:0x0009), differing only in its USB product string ("G6 Panel-as-
Controller Bench Harness") — so both machines can be plugged in at once
without this tool mistaking one for the other. Delegates the actual terminal
session to `pio device monitor`, which is already cross-platform.

Usage
-----
    monitor.py --serial 319A5199EE357F77       # attach to that panel
    monitor.py --list                          # show connected panels
    pixi run monitor -- --serial <SERIAL>      # via the pixi task

Requires `pyserial`, already installed transitively by `platformio`.
"""

from __future__ import annotations

import argparse
import subprocess
import sys

try:
    from serial.tools import list_ports
except ImportError:
    sys.exit("monitor: pyserial not found. It ships with `platformio` — run "
             "`pixi run release` (or any pio command) once, or `pip install pyserial`.")

RP_VID = 0x2E8A          # Raspberry Pi USB vendor id (matches g6_flash.py's RP_VID)
PID_APP = 0x0009         # running panel firmware: USB-serial device (matches g6_flash.py)
PRODUCT_PREFIX = "G6 Panel v"  # "G6 Panel v0.2"/"v0.3" — NOT "G6 Panel-as-Controller
                               # Bench Harness" (panel_controller/), which shares VID:PID
BAUD = 115200            # panel/src/constants.cpp BAUDRATE — the RP2350 USB-CDC link is
                         # rate-agnostic, but pio device monitor defaults to 9600.


def find_panels() -> list:
    return [p for p in list_ports.comports()
            if p.vid == RP_VID and p.pid == PID_APP
            and (p.product or "").startswith(PRODUCT_PREFIX)]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="monitor",
        description="Open a serial console on a specific G6 panel (cross-platform).",
    )
    ap.add_argument("--serial", metavar="SERIAL",
                     help="USB serial number of the panel to attach to")
    ap.add_argument("--baud", type=int, default=BAUD, metavar="N",
                     help=f"baud rate (default: {BAUD}, matches constants.cpp)")
    ap.add_argument("--list", action="store_true",
                     help="list connected panels and exit")
    args = ap.parse_args(argv)

    panels = find_panels()

    if args.list:
        if not panels:
            print("No G6 panels connected.")
        for p in panels:
            print(f"  {p.device}  serial={p.serial_number}  product={p.product!r}")
        return 0

    if not args.serial:
        sys.exit("monitor: --serial is required (use --list to find one).")

    matches = [p for p in panels if p.serial_number == args.serial]
    if not matches:
        available = ", ".join(p.serial_number for p in panels if p.serial_number) or "none"
        sys.exit(f"monitor: no G6 panel with serial {args.serial}. Connected: {available}")
    if len(matches) > 1:
        sys.exit(f"monitor: multiple ports report serial {args.serial} — "
                  + ", ".join(p.device for p in matches))

    port = matches[0].device
    print(f"monitor: attaching to {port} @ {args.baud} baud (Ctrl-C to exit)")
    return subprocess.call(["pio", "device", "monitor", "--port", port, "--baud", str(args.baud)])


if __name__ == "__main__":
    raise SystemExit(main())
