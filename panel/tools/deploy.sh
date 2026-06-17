#!/usr/bin/env bash
# Flash panel/ firmware to a specific G6 panel identified by USB serial number.
#
# Usage:
#   deploy.sh <USB_SERIAL> <PIO_ENV>
#   e.g.:
#       deploy.sh 319A5199EE357F77 pico_v021     # v0.2.1 panel
#       deploy.sh A5D4B82BA2B9FB51 pico_v031     # v0.3.1 panel
#
# Target board is identified by USB serial number, not /dev/ttyACM* (which
# shifts with enumeration order). Match criteria:
#   VID:PID         2e8a:0009   (Raspberry Pi VID, panel-firmware USB-serial PID)
#   USB product     "G6 Panel v0.2" / "G6 Panel v0.3"  (set in panel/platformio.ini)
#   serial number   passed as argv[1]
#
# A board in BOOTSEL mode (PID 0x000f) does not expose this serial, so this
# script only matches panels currently running the panel firmware. If the
# panel is stuck in BOOTSEL, flash it manually with `pio run -d panel -e
# <env> -t upload`.

set -euo pipefail

if (( $# != 2 )); then
    echo "usage: $(basename "$0") <USB_SERIAL> <PIO_ENV>" >&2
    echo "  e.g. $(basename "$0") 319A5199EE357F77 pico_v021" >&2
    exit 2
fi

TARGET_SERIAL="$1"
PIO_ENV="$2"

case "$PIO_ENV" in
    pico_v021*) PRODUCT="G6_Panel_v0.2" ;;
    pico_v031*) PRODUCT="G6_Panel_v0.3" ;;
    *)
        echo "deploy: unknown env '$PIO_ENV' — expected pico_v021* or pico_v031*" >&2
        exit 2
        ;;
esac

BY_ID_GLOB="/dev/serial/by-id/usb-Reiser_Lab_${PRODUCT}_*-if00"
TARGET_BY_ID="/dev/serial/by-id/usb-Reiser_Lab_${PRODUCT}_${TARGET_SERIAL}-if00"

shopt -s nullglob
matches=( $BY_ID_GLOB )

if (( ${#matches[@]} == 0 )); then
    echo "deploy: no G6 panel found in USB-serial mode." >&2
    echo "        expected ${TARGET_BY_ID}" >&2
    exit 1
fi

if (( ${#matches[@]} > 1 )); then
    echo "deploy: refusing to flash — multiple G6 panels are connected:" >&2
    for m in "${matches[@]}"; do echo "          $m" >&2; done
    echo "        Disconnect all but the target (serial ${TARGET_SERIAL}) and retry." >&2
    exit 1
fi

found="${matches[0]}"
if [[ "$found" != "$TARGET_BY_ID" ]]; then
    echo "deploy: connected G6 panel is not the requested target." >&2
    echo "          found:    $found" >&2
    echo "          expected: $TARGET_BY_ID" >&2
    exit 1
fi

echo "deploy: flashing $PIO_ENV → $found"
exec pio run -d panel -e "$PIO_ENV" -t upload --upload-port "$found"
