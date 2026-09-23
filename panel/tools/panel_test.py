#!/usr/bin/env python3
"""panel_test — bench CLI for the STAGE2_SELFTEST (pico_v0*_bcmtest) firmware.

Drives one color channel of a multi-color G6 test panel at a time. The panel
LEDs sit in 2x2 quartets, one LED per color channel; channel = 2*(lr%2)+(lc%2)
(= schematic column % 4, see src/layout.cpp). Intensity is a percent that maps
onto the pattern duty_cycle (0..255). Requires a panel flashed with a bcmtest
env — production firmware has no serial pattern commands.

Usage
-----
    panel_test.py led 2 50            # channel 2 at 50%
    panel_test.py all 50              # every channel at 50%
    panel_test.py cycle 50 1000       # 0->1->2->3, 1 s each, 50%
    panel_test.py quad 2 on5 on5 a10 b10
                                      # ch0+1 always on at 5%; ch2 in TL+BR and
                                      # ch3 in TR+BL 10x10 quadrants at 10%,
                                      # swapping every 2 s. 'off' = channel dark.
    panel_test.py off                 # everything dark
    panel_test.py raw 'p3,7'          # pass any selftest command through
    panel_test.py                     # interactive REPL (same verbs)

Auto-picks the panel if exactly one is connected; otherwise use
--serial <USB serial> (see --list) or --port /dev/cu.usbmodemXXXX.
"""

from __future__ import annotations

import argparse
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("panel_test: pyserial not found (ships with platformio; or `pip install pyserial`).")

RP_VID = 0x2E8A
PID_APP = 0x0009
PRODUCT_PREFIX = "G6 Panel v"
BAUD = 115200
NUM_CHANNELS = 4
REPLY_WAIT_S = 0.3


def find_panels() -> list:
    return [p for p in list_ports.comports()
            if p.vid == RP_VID and p.pid == PID_APP
            and (p.product or "").startswith(PRODUCT_PREFIX)]


def pick_port(serial_no: str | None, port: str | None) -> str:
    if port:
        return port
    panels = find_panels()
    if serial_no:
        m = [p for p in panels if p.serial_number == serial_no]
        if not m:
            avail = ", ".join(p.serial_number or "?" for p in panels) or "none"
            sys.exit(f"panel_test: no panel with serial {serial_no}. Connected: {avail}")
        return m[0].device
    if len(panels) == 1:
        return panels[0].device
    if not panels:
        sys.exit("panel_test: no G6 panel connected.")
    sys.exit("panel_test: several panels connected; pick one with --serial:\n" +
             "\n".join(f"  {p.device}  serial={p.serial_number}" for p in panels))


def pct_arg(s: str) -> int:
    v = int(s)
    if not 0 <= v <= 100:
        raise ValueError(f"percent out of range: {v}")
    return v


def quad_spec(s: str) -> str:
    """'on5' / 'on:5' -> 's5', 'a10' -> 'a10', 'b10' -> 'b10', 'off' -> 'x'."""
    t = s.lower().replace(":", "")
    if t in ("off", "x", "0"):
        return "x"
    for prefix, role in (("on", "s"), ("s", "s"), ("a", "a"), ("b", "b")):
        if t.startswith(prefix) and t[len(prefix):].isdigit():
            return f"{role}{pct_arg(t[len(prefix):])}"
    raise ValueError(f"bad channel spec '{s}' (want on<pct>, a<pct>, b<pct>, or off)")


def to_firmware_cmd(words: list[str]) -> str:
    """Translate a CLI verb into the single-line selftest serial command."""
    if not words:
        raise ValueError("empty command")
    verb, args = words[0].lower(), words[1:]
    if verb in ("led", "ch", "channel"):
        if len(args) != 2:
            raise ValueError("usage: led <ch 0..3> <pct 0..100>")
        ch = int(args[0])
        if not 0 <= ch < NUM_CHANNELS:
            raise ValueError(f"channel out of range: {ch}")
        return f"c{ch},{pct_arg(args[1])}"
    if verb == "all":
        if len(args) != 1:
            raise ValueError("usage: all <pct 0..100>")
        return f"a{pct_arg(args[0])}"
    if verb == "cycle":
        if len(args) not in (1, 2):
            raise ValueError("usage: cycle <pct 0..100> [ms per channel]")
        pct = pct_arg(args[0])
        ms = int(args[1]) if len(args) == 2 else 1000
        return f"y{pct},{ms}"
    if verb == "quad":
        # quad <seconds> <ch0> <ch1> <ch2> <ch3>; each ch spec is on<pct>, a<pct>, b<pct>, or off
        if len(args) != 5:
            raise ValueError("usage: quad <seconds per flip> <ch0> <ch1> <ch2> <ch3>\n"
                             "       ch spec: on<pct> (always on) | a<pct> (TL+BR) | b<pct> (TR+BL) | off\n"
                             "       e.g. quad 2 on5 on5 a10 b10")
        ms = int(round(float(args[0]) * 1000))
        if not 50 <= ms <= 60000:
            raise ValueError("flip period must be 0.05..60 seconds")
        return "q" + ",".join(quad_spec(s) for s in args[1:]) + f",{ms}"
    if verb in ("off", "stop"):
        return "y0"
    if verb == "raw":
        if not args:
            raise ValueError("usage: raw <selftest command>")
        return " ".join(args)
    if verb in ("help", "?"):
        return "?"
    raise ValueError(f"unknown verb '{verb}' (led/all/cycle/quad/off/raw/help)")


def send(ser: serial.Serial, cmd: str, wait_s: float = REPLY_WAIT_S, echo: bool = True) -> list[str]:
    """Send one selftest command; return the reply lines (printed too unless echo=False)."""
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    ser.flush()
    replies: list[str] = []
    deadline = time.monotonic() + wait_s
    while time.monotonic() < deadline:
        line = ser.readline()
        if line:
            text = line.decode(errors="replace").rstrip()
            replies.append(text)
            if echo:
                print("  <", text)
            deadline = time.monotonic() + 0.1
    return replies


def repl(ser: serial.Serial) -> None:
    print("panel_test REPL — verbs: led <ch> <pct> | all <pct> | cycle <pct> [ms] | "
          "quad <sec> <ch0> <ch1> <ch2> <ch3> (on<pct>|a<pct>|b<pct>|off) | off | raw <cmd> | quit")
    while True:
        try:
            line = input("panel> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if not line:
            continue
        if line.lower() in ("q", "quit", "exit"):
            return
        try:
            cmd = to_firmware_cmd(line.split())
        except ValueError as e:
            print(f"  ! {e}")
            continue
        send(ser, cmd)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="panel_test", description=__doc__.split("\n\n")[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial", metavar="SERIAL", help="USB serial number of the panel")
    ap.add_argument("--port", metavar="DEV", help="serial device path (overrides --serial)")
    ap.add_argument("--baud", type=int, default=BAUD)
    ap.add_argument("--list", action="store_true", help="list connected panels and exit")
    ap.add_argument("words", nargs="*", help="command (omit for interactive REPL)")
    args = ap.parse_args(argv)

    if args.list:
        panels = find_panels()
        if not panels:
            print("No G6 panels connected.")
        for p in panels:
            print(f"  {p.device}  serial={p.serial_number}  product={p.product!r}")
        return 0

    port = pick_port(args.serial, args.port)
    try:
        ser = serial.Serial(port, args.baud, timeout=0.05)
    except serial.SerialException as e:
        sys.exit(f"panel_test: cannot open {port}: {e}")

    with ser:
        if args.words:
            try:
                cmd = to_firmware_cmd(args.words)
            except ValueError as e:
                sys.exit(f"panel_test: {e}")
            print(f"  > {cmd}")
            send(ser, cmd)
        else:
            repl(ser)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
