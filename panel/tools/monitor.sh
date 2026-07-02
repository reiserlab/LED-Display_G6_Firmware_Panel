#!/usr/bin/env bash
# Open a serial monitor on a specific G6 panel.
#
# Usage:
#   monitor.sh <USB_SERIAL>
#   e.g.:
#       monitor.sh 319A5199EE357F77     # v0.2.1 panel
#       monitor.sh A5D4B82BA2B9FB51     # v0.3.1 panel
#
# Target board is identified by USB serial number, not the device node (which
# shifts with enumeration order). Match: VID 2e8a, product "G6 Panel v0.x"
# (per-rev usb_product in platformio.ini), serial == argv[1].
#
#   * Linux — resolves /dev/serial/by-id/...
#   * macOS — resolves /dev/cu.usbmodem* by USB serial (tools/panel_port.py).
#
# Baud mirrors panel/src/constants.cpp (BAUDRATE = 115200). The RP2350 USB-CDC
# link is rate-agnostic, but pio device monitor defaults to 9600.

set -euo pipefail

if (( $# != 1 )); then
    echo "usage: $(basename "$0") <USB_SERIAL>" >&2
    echo "  e.g. $(basename "$0") 319A5199EE357F77" >&2
    exit 2
fi

TARGET_SERIAL="$1"
BAUD=115200
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PIO="${PIO:-pio}"
PYTHON="${PYTHON:-/usr/bin/python3}"

case "$(uname -s)" in
Darwin)
    PORT="$("$PYTHON" "$SCRIPT_DIR/panel_port.py" "$TARGET_SERIAL")" || exit 1
    echo "monitor: attaching to $PORT @ ${BAUD} baud  (Ctrl-C to exit)"
    exec "$PIO" device monitor --port "$PORT" --baud "$BAUD"
    ;;
esac

# Linux (original udev-by-id path). Monitor gets only a serial (no env), so
# glob both per-rev products (G6_Panel_v0.2 / G6_Panel_v0.3).
BY_ID_GLOB='/dev/serial/by-id/usb-Reiser_Lab_G6_Panel_v0.*-if00'
TARGET_BY_ID_GLOB="/dev/serial/by-id/usb-Reiser_Lab_G6_Panel_v0.*_${TARGET_SERIAL}-if00"

shopt -s nullglob
matches=( $BY_ID_GLOB )

if (( ${#matches[@]} == 0 )); then
    echo "monitor: no G6 panel found in USB-serial mode." >&2
    echo "         expected ${TARGET_BY_ID_GLOB}" >&2
    exit 1
fi

target_matches=( $TARGET_BY_ID_GLOB )
if (( ${#target_matches[@]} == 0 )); then
    echo "monitor: requested panel (serial ${TARGET_SERIAL}) is not connected." >&2
    echo "         connected:" >&2
    for m in "${matches[@]}"; do echo "           $m" >&2; done
    exit 1
fi
TARGET_BY_ID="${target_matches[0]}"

echo "monitor: attaching to $TARGET_BY_ID @ ${BAUD} baud  (Ctrl-C to exit)"
exec "$PIO" device monitor --port "$TARGET_BY_ID" --baud "$BAUD"
