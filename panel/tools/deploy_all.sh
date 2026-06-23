#!/usr/bin/env bash
# Flash panel firmware to ALL connected panels of the given hardware revision,
# running one pio upload per panel in parallel.
#
# Usage:
#   deploy_all.sh <PIO_ENV>
#   e.g.: deploy_all.sh pico_v021         # flash all v0.2 panels
#         deploy_all.sh pico_v021_spidiag  # flash all v0.2 panels with SPI diag build
#
# Panels are identified by USB product string encoded in /dev/serial/by-id/.
# Requires boards already running panel firmware with the current product
# strings (G6 Panel v0.2 / G6 Panel v0.3). Boards with older firmware showing
# "RP2354 20x20 Display Panel" will not be found; flash them once with
# `pio run -d panel -e <env> -t upload` to bootstrap.

set -euo pipefail

PIO_ENV="${1:?usage: $(basename "$0") <PIO_ENV>}"

case "$PIO_ENV" in
    pico_v021*) GLOB='/dev/serial/by-id/usb-Reiser_Lab_G6_Panel_v0.2_*-if00' ;;
    pico_v031*) GLOB='/dev/serial/by-id/usb-Reiser_Lab_G6_Panel_v0.3_*-if00' ;;
    *)
        echo "deploy_all: unknown env '$PIO_ENV' — expected pico_v021* or pico_v031*" >&2
        exit 2
        ;;
esac

shopt -s nullglob
panels=( $GLOB )

if (( ${#panels[@]} == 0 )); then
    echo "deploy_all[$PIO_ENV]: no panels found" >&2
    echo "  (glob: $GLOB)" >&2
    exit 1
fi

echo "deploy_all[$PIO_ENV]: flashing ${#panels[@]} panel(s)"
for p in "${panels[@]}"; do echo "  $p"; done

failed=0
for port in "${panels[@]}"; do
    echo "deploy_all[$PIO_ENV]: flashing $port"
    pio run -d panel -e "$PIO_ENV" -t upload --upload-port "$port" \
        || failed=$((failed + 1))
done

if (( failed > 0 )); then
    echo "deploy_all[$PIO_ENV]: $failed / ${#panels[@]} flash(es) FAILED" >&2
    exit 1
fi
echo "deploy_all[$PIO_ENV]: all ${#panels[@]} panel(s) done"
