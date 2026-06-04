#!/usr/bin/env bash
# LAB-43 live-measurement demo orchestrator.
#   run_demo.sh check    -> pre-flight: light the panel, confirm photodiode is live
#   run_demo.sh cached    -> build + open dashboard from cached data/ (instant; safe fallback)
#   run_demo.sh sweep     -> live: re-measure the 10-duty on-time sweep, then build + open
#   run_demo.sh live      -> live: full sweep + latency/jitter re-measure, then build + open
# Pre-req (see DEMO.md): panels on production (pico_v031/_v021), arena on the CLEAN
# teensy41 build (NOT -printf), photodiode powered on AD3 Ch1 (1+ and 1-), W1->BNC.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; BIN="$DIR/bin"; DATA="$DIR/data"
PY="${PY:-python3}"; PY314="${PY314:-/opt/homebrew/bin/python3.14}"
MODE="${1:-cached}"
AUDIO="${AUDIO:-1}"   # spoken cues + chimes during the demo; set AUDIO=0 to silence
_say(){ [ "$AUDIO" = "1" ] && ( say "$1" >/dev/null 2>&1 & ) || true; }
_chime(){ [ "$AUDIO" = "1" ] && ( afplay "/System/Library/Sounds/${1:-Glass}.aiff" >/dev/null 2>&1 & ) || true; }
build(){ "$PY" "$BIN/dashboard.py" "$DIR/dashboard.html"; }
open_dash(){ if command -v open >/dev/null; then open "$DIR/dashboard.html"; else echo "open $DIR/dashboard.html"; fi; }
DUTIES="16 32 48 64 85 100 128 160 200 255"

case "$MODE" in
  check)
    "$PY" "$BIN/stream.py" --mode allon
    "$PY314" "$BIN/check.py"
    _chime Ping
    "$PY" "$BIN/stream.py" --mode alloff ;;
  cached)
    build; open_dash ;;
  sweep|live)
    _chime Submarine; _say "Recording started"
    echo "== live duty sweep (photodiode on-time) =="
    for D in $DUTIES; do
      "$PY" "$BIN/stream.py" --mode triggered --duty "$D" >/dev/null
      "$PY314" "$BIN/cap_json.py" "$D"
      cp "/tmp/duty_$D.json" "$DATA/"
    done
    if [ "$MODE" = "live" ]; then
      echo "== live latency + jitter =="
      "$PY" "$BIN/stream.py" --mode triggered --duty 255 >/dev/null
      "$PY314" "$BIN/jit.py"
      cp /tmp/lat_jit.json "$DATA/"
    fi
    "$PY" "$BIN/stream.py" --mode alloff >/dev/null
    _say "Measurement complete"; _chime Glass
    build; open_dash ;;
  *) echo "usage: $(basename "$0") [check|cached|sweep|live]"; exit 2 ;;
esac
