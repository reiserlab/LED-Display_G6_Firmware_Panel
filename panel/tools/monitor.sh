#!/usr/bin/env bash
# Open a serial monitor on a specific G6 panel.
#
# Usage:
#   monitor.sh <USB_SERIAL>
#   e.g.:
#       monitor.sh 319A5199EE357F77     # v0.2.1 panel
#       monitor.sh A5D4B82BA2B9FB51     # v0.3.1 panel
#
# Target board is identified by USB serial number, not /dev/ttyACM* (which
# shifts with enumeration order). Match criteria:
#   VID:PID         2e8a:0009   (Raspberry Pi VID, panel-firmware USB-serial PID)
#   USB product     "RP2354 20x20 Display Panel"   (set in panel/platformio.ini)
#   serial number   passed as argv[1]
#
# Baud rate mirrors panel/src/constants.cpp (BAUDRATE = 115200). The RP2350
# USB-CDC link is rate-agnostic, but pio device monitor defaults to 9600 and
# would garble pyserial's idea of the framing if we left it unset.

set -euo pipefail

if (( $# != 1 )); then
    echo "usage: $(basename "$0") <USB_SERIAL>" >&2
    echo "  e.g. $(basename "$0") 319A5199EE357F77" >&2
    exit 2
fi

TARGET_SERIAL="$1"
BAUD=115200
BY_ID_GLOB='/dev/serial/by-id/usb-Reiser_Lab_RP2354_20x20_Display_Panel_*-if00'
TARGET_BY_ID="/dev/serial/by-id/usb-Reiser_Lab_RP2354_20x20_Display_Panel_${TARGET_SERIAL}-if00"

shopt -s nullglob
matches=( $BY_ID_GLOB )

if (( ${#matches[@]} == 0 )); then
    echo "monitor: no G6 panel found in USB-serial mode." >&2
    echo "         expected ${TARGET_BY_ID}" >&2
    exit 1
fi

if (( ${#matches[@]} > 1 )); then
    echo "monitor: refusing to attach — multiple G6 panels are connected:" >&2
    for m in "${matches[@]}"; do echo "           $m" >&2; done
    echo "         Disconnect all but the target (serial ${TARGET_SERIAL}) and retry." >&2
    exit 1
fi

found="${matches[0]}"
if [[ "$found" != "$TARGET_BY_ID" ]]; then
    echo "monitor: connected G6 panel is not the requested target." >&2
    echo "           found:    $found" >&2
    echo "           expected: $TARGET_BY_ID" >&2
    exit 1
fi

echo "monitor: attaching to $found @ ${BAUD} baud  (Ctrl-C to exit)"
exec pio device monitor --port "$found" --baud "$BAUD"
