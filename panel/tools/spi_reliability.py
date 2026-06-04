#!/usr/bin/env python3
"""LAB-39 single-panel SPI reliability benchmark (PL022+DMA path, PR #4).

Drives BOTH ends of the bus so the measurement window is self-bracketing:

  * arena master  (Teensy, USB VID 0x16C0) — sets the SPI clock, streams a fixed
    frame free-running at a forced refresh, and exposes a frames-sent counter.
  * panel under test (RP2350, USB VID 0x2E8A) running a *_spidiag build — the
    ported silent counter: 'z' zeroes the window, 'd' dumps msgs / reject_any /
    per-gate fails / last-32 fail ring. No serial output while streaming, so the
    timing is production-identical.

Procedure: stream at the target clock, 'z' the panel, read master frames-sent
(c1), dwell, read master frames-sent (c2), 'd' the panel. Then:

    sent      = c2 - c1                 (master-side, ground truth denominator)
    received  = panel msgs              (whole CS-framed messages the panel saw)
    missed    = sent - received         (frames the panel never framed at all)
    reject_any= within-frame corruption (parity/length/protocol/unknown)

DoD (LAB-39): reject_any ~= 0 AND missed ~= 0 at the target clock, on both revs.

    # autodetect both ports by USB VID:
    ./spi_reliability.py --mhz 25 --gs gs16 --seconds 30
    # or pin them explicitly:
    ./spi_reliability.py --master-port /dev/cu.usbmodem121699401 \
                         --panel-port  /dev/cu.usbmodem1101 --mhz 25
    # master-only (you drive 'z'/'d' on the panel yourself):
    ./spi_reliability.py --mhz 25 --no-panel
"""

import argparse
import re
import sys
import time

# --- Arena master command opcodes (Arena src/commands.h) -------------------
ALL_OFF_CMD           = 0x00
SET_REFRESH_RATE_CMD  = 0x16
SET_SPI_CLOCK_CMD     = 0x17
GET_SPI_CLOCK_CMD     = 0x18
GET_FRAMES_SENT_CMD   = 0x19
RESET_FRAMES_SENT_CMD = 0x1A
STREAM_FRAME_CMD      = 0x32

# --- Frame geometry (Arena src/constants.h) --------------------------------
NUM_PANELS  = 20
GS16_BLOCK  = 203
GS2_BLOCK   = 53
GS16_CMD    = 0x30
GS2_CMD     = 0x10

MASTER_VID = 0x16C0   # Teensy
PANEL_VID  = 0x2E8A   # RP2350 (Raspberry Pi)


# ---------------------------------------------------------------------------
# Arena master: binary framing [length, cmd, params...]; the length byte counts
# everything after itself. Response [length, status, echo_cmd, ...payload].
# ---------------------------------------------------------------------------
class Master:
    def __init__(self, port):
        import serial
        self.s = serial.Serial(port, 115200, timeout=2.0)
        time.sleep(0.3)
        self.s.reset_input_buffer()
        self.port = port

    def _read_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.s.read(n - len(buf))
            if not chunk:
                break
            buf += chunk
        return buf

    def command(self, frame):
        self.s.write(bytes(frame))
        self.s.flush()
        hdr = self._read_exact(1)
        if not hdr:
            return None
        body = self._read_exact(hdr[0])
        if len(body) < 2:
            return None
        return body[0], body[1], body[2:]

    def get_frames_sent(self):
        resp = self.command([0x01, GET_FRAMES_SENT_CMD])
        ts = time.perf_counter()
        if not resp or len(resp[2]) < 4:
            raise RuntimeError("bad GET_FRAMES_SENT reply")
        p = resp[2]
        return (p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24), ts

    def get_spi_clock_mhz(self):
        resp = self.command([0x01, GET_SPI_CLOCK_CMD])
        if not resp or len(resp[2]) < 2:
            raise RuntimeError("bad GET_SPI_CLOCK reply")
        return resp[2][0] | resp[2][1] << 8

    def close(self):
        self.s.close()


def u16le(n):
    return [n & 0xFF, (n >> 8) & 0xFF]


def build_stream_frame(gs16):
    """Size-correct stream frame with a valid V1 parity header per panel block.
    Pixels are all-max so the panel lights FULLY ON — a visible liveness check
    (and the parity bit below is recomputed to match)."""
    block_len = GS16_BLOCK if gs16 else GS2_BLOCK
    cmd = GS16_CMD if gs16 else GS2_CMD
    block = bytearray(block_len)
    block[1] = cmd
    for i in range(2, block_len - 1):
        block[i] = 0xFF          # all pixel bytes max -> panel fully lit (visible)
    block[block_len - 1] = 0xFF  # duty_cycle
    pixels_and_duty = block[2:block_len]
    ones = bin(0x01 & 0x7F).count("1") + bin(cmd).count("1") \
        + sum(bin(b).count("1") for b in pixels_and_duty)
    block[0] = 0x01 | ((ones & 1) << 7)  # version | parity-in-bit7
    body = bytearray(b"FR\x00\x00") + bytes(block) * NUM_PANELS
    return bytes([STREAM_FRAME_CMD, len(body) & 0xFF, (len(body) >> 8) & 0xFF]) \
        + bytes(body)


# ---------------------------------------------------------------------------
# Panel: USB-CDC console of a *_spidiag build. 'z' zero, 'd' dump.
# ---------------------------------------------------------------------------
class Panel:
    def __init__(self, port):
        import serial
        self.s = serial.Serial(port, 115200, timeout=2.0)
        time.sleep(0.3)
        self.s.reset_input_buffer()
        self.port = port

    def zero(self):
        self.s.reset_input_buffer()
        self.s.write(b"z")
        self.s.flush()

    def dump(self, settle=1.5):
        """Send 'd' and collect the dump burst (bounded by the '=== end ===')."""
        self.s.reset_input_buffer()
        self.s.write(b"d")
        self.s.flush()
        deadline = time.time() + settle + 2.0
        buf = b""
        while time.time() < deadline:
            chunk = self.s.read(256)
            if chunk:
                buf += chunk
                if b"=== end ===" in buf:
                    break
        return buf.decode("ascii", "replace")

    def close(self):
        self.s.close()


def parse_dump(text):
    """Pull the headline counters out of a SPI_DIAG dump."""
    def grab(key, default=None):
        m = re.search(rf"{key}=(\d+)", text)
        return int(m.group(1)) if m else default
    rev = re.search(r"PANEL_REV=(\d+)", text)
    return {
        "panel_rev": int(rev.group(1)) if rev else None,
        "msgs": grab("msgs"),
        "reject_any": grab("reject_any"),
        "parity_fail": grab("parity_fail"),
        "length_fail": grab("length_fail"),
        "protocol_fail": grab("protocol_fail"),
        "unknown_cmd": grab("unknown_cmd"),
    }


def autodetect(vid):
    from serial.tools import list_ports
    return [p.device for p in list_ports.comports() if p.vid == vid]


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--master-port", help="arena master CDC (default: autodetect VID 0x16C0)")
    ap.add_argument("--panel-port", help="panel CDC (default: autodetect VID 0x2E8A)")
    ap.add_argument("--no-panel", action="store_true",
                    help="drive master only; you run 'z'/'d' on the panel yourself")
    ap.add_argument("--mhz", type=int, default=25, help="SPI clock MHz (default 25)")
    ap.add_argument("--gs", choices=["gs16", "gs2"], default="gs16",
                    help="grayscale mode / block size (default gs16)")
    ap.add_argument("--seconds", type=float, default=30.0,
                    help="measurement window seconds (default 30)")
    ap.add_argument("--refresh", type=int, default=2000,
                    help="forced refresh Hz; keep above achievable fps so the bus "
                         "is transfer-bound and continuously busy (default 2000)")
    args = ap.parse_args()
    gs16 = args.gs == "gs16"

    # Resolve ports.
    mport = args.master_port
    if not mport:
        found = autodetect(MASTER_VID)
        if len(found) != 1:
            print(f"master autodetect found {found}; pass --master-port", file=sys.stderr)
            sys.exit(1)
        mport = found[0]
    pport = None
    if not args.no_panel:
        pport = args.panel_port
        if not pport:
            found = autodetect(PANEL_VID)
            if len(found) != 1:
                print(f"panel autodetect found {found}; pass --panel-port or --no-panel",
                      file=sys.stderr)
                sys.exit(1)
            pport = found[0]

    master = Master(mport)
    panel = Panel(pport) if pport else None
    print(f"# master : {mport}")
    print(f"# panel  : {pport or '(none — drive z/d manually)'}")
    print(f"# clock  : {args.mhz} MHz   mode: {args.gs.upper()}   "
          f"window: {args.seconds}s   refresh: {args.refresh} Hz")

    try:
        # Stream a fixed frame (resets master counter, arms free-run), force the
        # refresh high so transmission is transfer-bound, set the target clock.
        master.command(build_stream_frame(gs16))
        master.command([0x03, SET_REFRESH_RATE_CMD, *u16le(args.refresh)])
        master.command([0x03, SET_SPI_CLOCK_CMD, *u16le(args.mhz)])
        time.sleep(0.5)  # settle
        applied = master.get_spi_clock_mhz()
        print(f"# master SPI clock applied: {applied} MHz")
        if applied != args.mhz:
            print(f"!! requested {args.mhz} MHz but master reports {applied} MHz", file=sys.stderr)

        # Self-bracketing window: zero the panel, baseline the master counter
        # right next to it; dwell; read the master counter, then dump the panel.
        if panel:
            panel.zero()
        c1, _ = master.get_frames_sent()
        time.sleep(args.seconds)
        c2, _ = master.get_frames_sent()
        sent = c2 - c1
        print(f"\nmaster frames sent in window : {sent}")

        if panel:
            text = panel.dump()
            d = parse_dump(text)
            print("--- panel SPI_DIAG dump ---")
            print(text.strip())
            print("---------------------------")
            recv = d["msgs"]
            if recv is None:
                print("!! could not parse panel dump", file=sys.stderr)
            else:
                missed = sent - recv
                rej = d["reject_any"] or 0
                pct = (100.0 * rej / recv) if recv else 0.0
                misspct = (100.0 * missed / sent) if sent else 0.0
                print(f"\nRESULT  rev={d['panel_rev']}  clock={applied}MHz  {args.gs}")
                print(f"  sent={sent}  received={recv}  missed={missed} ({misspct:.2f}%)")
                print(f"  reject_any={rej} ({pct:.2f}%)  "
                      f"parity={d['parity_fail']} length={d['length_fail']} "
                      f"protocol={d['protocol_fail']} unknown={d['unknown_cmd']}")
                ok = (rej == 0 and abs(missed) <= max(5, int(0.001 * sent)))
                print(f"  DoD reject~0 & missed~0: {'PASS' if ok else 'FAIL'}")
        else:
            print("(no panel port) — send 'z' before the window and 'd' after on "
                  "the panel CDC to read msgs/reject_any.")
    finally:
        # Restore a clean idle state on the master regardless of how we exit.
        master.command([0x01, ALL_OFF_CMD])
        master.command([0x03, SET_REFRESH_RATE_CMD, *u16le(300)])
        master.command([0x03, SET_SPI_CLOCK_CMD, *u16le(10)])
        master.command([0x01, RESET_FRAMES_SENT_CMD])
        master.close()
        if panel:
            panel.close()


if __name__ == "__main__":
    main()
