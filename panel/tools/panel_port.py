#!/usr/bin/env python3
"""Resolve a G6 panel's USB-CDC device path by USB serial number.

Cross-platform (Linux /dev/ttyACM*, macOS /dev/cu.usbmodem*) via pyserial's
list_ports, so the same targeting works on the bench and on CI. Prints the
resolved device path on stdout; on failure prints a diagnostic to stderr and
exits non-zero.

Match criteria (mirrors panel/platformio.ini USB identity):
    VID      0x2E8A   (Raspberry Pi / RP2350)
    product  starts with "G6 Panel" (per-rev: "G6 Panel v0.2" / "G6 Panel v0.3")
    serial   == argv[1]

A panel in BOOTSEL mode (mass-storage PID, no CDC serial) is intentionally not
matched — this only resolves panels currently running the firmware.

    usage: panel_port.py <USB_SERIAL>
"""
import sys

PANEL_VID = 0x2E8A
# Per-rev usb_product is "G6 Panel v0.2" / "G6 Panel v0.3" (platformio.ini);
# match the common prefix so one resolver serves both revisions.
PRODUCT_PREFIX = "G6 Panel"


def main():
    if len(sys.argv) != 2:
        print("usage: panel_port.py <USB_SERIAL>", file=sys.stderr)
        sys.exit(2)
    target = sys.argv[1]
    try:
        from serial.tools import list_ports
    except ImportError:
        print("panel_port: pyserial not available (pip install pyserial)", file=sys.stderr)
        sys.exit(3)

    panels = [p for p in list_ports.comports()
              if p.vid == PANEL_VID and (p.product or "").startswith(PRODUCT_PREFIX)]
    if not panels:
        print("panel_port: no G6 panel in USB-serial mode "
              "(VID 2e8a, product 'G6 Panel v0.x')", file=sys.stderr)
        sys.exit(1)

    match = [p for p in panels if p.serial_number == target]
    if not match:
        avail = ", ".join(p.serial_number or "?" for p in panels)
        print(f"panel_port: serial {target} not connected; available: {avail}",
              file=sys.stderr)
        sys.exit(1)
    if len(match) > 1:
        print(f"panel_port: multiple panels share serial {target}?!", file=sys.stderr)
        sys.exit(1)

    print(match[0].device)


if __name__ == "__main__":
    main()
