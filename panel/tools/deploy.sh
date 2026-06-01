#!/usr/bin/env bash
# Flash panel/ firmware to a specific G6 panel.
#
# Usage:
#   deploy.sh <USB_SERIAL> <PIO_ENV>
#   e.g.:
#       deploy.sh 319A5199EE357F77 pico_v021     # v0.2.1 panel
#       deploy.sh A5D4B82BA2B9FB51 pico_v031     # v0.3.1 panel
#
# Target board is identified by USB serial number, not the device node (which
# shifts with enumeration order) — so it is safe with multiple panels attached.
#
#   VID:PID         2e8a:0009   (Raspberry Pi VID, panel-firmware USB-serial PID)
#   USB product     "RP2354 20x20 Display Panel"   (set in panel/platformio.ini)
#   serial number   passed as argv[1]
#
# Platform handling:
#   * Linux  — resolves /dev/serial/by-id/... and lets PlatformIO upload.
#   * macOS  — resolves /dev/cu.usbmodem* by USB serial (tools/panel_port.py),
#              builds, then flashes via the earlephilhower 1200-baud BOOTSEL
#              touch + UF2 copy to /Volumes/RP2350 (no button press needed).
#
# A board already in BOOTSEL (PID 0x000f) exposes no CDC serial; on macOS, if
# /Volumes/RP2350 is already mounted the touch is skipped.

set -euo pipefail

if (( $# != 2 )); then
    echo "usage: $(basename "$0") <USB_SERIAL> <PIO_ENV>" >&2
    echo "  e.g. $(basename "$0") 319A5199EE357F77 pico_v021" >&2
    exit 2
fi

TARGET_SERIAL="$1"
PIO_ENV="$2"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PANEL_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PIO="${PIO:-pio}"
PYTHON="${PYTHON:-/usr/bin/python3}"

# ---------------------------------------------------------------------------
case "$(uname -s)" in
Darwin)
    UF2="$PANEL_DIR/.pio/build/$PIO_ENV/firmware.uf2"

    echo "deploy: building $PIO_ENV ..."
    "$PIO" run -d "$PANEL_DIR" -e "$PIO_ENV"
    [[ -f "$UF2" ]] || { echo "deploy: build produced no $UF2" >&2; exit 1; }

    if [[ -d /Volumes/RP2350 ]]; then
        echo "deploy: /Volumes/RP2350 already mounted — board is in BOOTSEL, skipping touch"
    else
        PORT="$("$PYTHON" "$SCRIPT_DIR/panel_port.py" "$TARGET_SERIAL")" || exit 1
        echo "deploy: 1200-baud BOOTSEL touch on $PORT (serial $TARGET_SERIAL)"
        "$PYTHON" - "$PORT" <<'PY'
import serial, sys, time
p = serial.Serial(sys.argv[1], 1200); p.setDTR(False); time.sleep(0.3); p.close()
PY
        echo "deploy: waiting for /Volumes/RP2350 ..."
        for _ in $(seq 1 50); do [[ -d /Volumes/RP2350 ]] && break; sleep 0.2; done
        [[ -d /Volumes/RP2350 ]] || { echo "deploy: RP2350 did not mount" >&2; exit 1; }
    fi

    # NB: plain cp, not `cp -X`. macOS 15's FSKit msdos driver rejects the
    # extended-attribute handling of `cp -X` on the RP2350 BOOTSEL volume with a
    # spurious "Permission denied". It also returns "Permission denied" on an
    # immediate cp right after mount (volume not write-ready yet) — so retry.
    copied=0
    for _ in $(seq 1 20); do
        if cp "$UF2" /Volumes/RP2350/ 2>/dev/null; then copied=1; break; fi
        sleep 0.5
    done
    (( copied == 1 )) || { echo "deploy: UF2 copy to /Volumes/RP2350 failed (FSKit mount race?)" >&2; exit 1; }
    sync
    echo "deploy: flashed $PIO_ENV — panel will re-enumerate in a few seconds"
    exit 0
    ;;
esac

# ---------------------------------------------------------------------------
# Linux (original udev-by-id path).
BY_ID_GLOB='/dev/serial/by-id/usb-Reiser_Lab_RP2354_20x20_Display_Panel_*-if00'
TARGET_BY_ID="/dev/serial/by-id/usb-Reiser_Lab_RP2354_20x20_Display_Panel_${TARGET_SERIAL}-if00"

shopt -s nullglob
matches=( $BY_ID_GLOB )

if (( ${#matches[@]} == 0 )); then
    echo "deploy: no G6 panel found in USB-serial mode." >&2
    echo "        expected ${TARGET_BY_ID}" >&2
    exit 1
fi

if [[ ! -e "$TARGET_BY_ID" ]]; then
    echo "deploy: requested panel (serial ${TARGET_SERIAL}) is not connected." >&2
    echo "        connected:" >&2
    for m in "${matches[@]}"; do echo "          $m" >&2; done
    exit 1
fi

echo "deploy: flashing $PIO_ENV → $TARGET_BY_ID"
exec "$PIO" run -d "$PANEL_DIR" -e "$PIO_ENV" -t upload --upload-port "$TARGET_BY_ID"
