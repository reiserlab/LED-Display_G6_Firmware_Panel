#!/usr/bin/env python3
"""LAB-39 CIPO confirmation capture WITH a liveness/receive proof.

Drives the panel ('z'/'d') and the arena master (stream + the DEBUG_SERIAL
all-sets MISO capture) in one run, so the CIPO bytes are only ever interpreted
alongside hard proof the panel was alive and receiving in the SAME window:

  1. panel 'z'  (zero the silent counters)
  2. master: set clock, set refresh, STREAM a fixed GS16 frame
  3. read the master's '[spi] CIPO set..' debug lines for `seconds`
  4. panel 'd'  -> msgs>0 proves the panel was receiving (and arming CIPO)
  5. master ALL_OFF + restore

Requires the master flashed with env teensy41-printf (-DDEBUG_SERIAL) carrying
the all-sets 16-byte MISO dump. A live, wired, correctly-driving panel must show
a NON-ZERO byte 0 on its set/bus (tx_buf_[0] is never 0 — it is 0x81 sentinel or
a 0x01|parity header). All-zero while msgs>0 => the confirmation is not reaching
the master's MISO pins (12/1) at bytes 0..15.

    ./cipo_capture.py --mhz 5 --seconds 3
"""
import argparse, sys, time
from serial.tools import list_ports
import serial

NUM_PANELS = 20; GS16_BLOCK = 203; GS16_CMD = 0x30


def u16le(n): return [n & 0xFF, (n >> 8) & 0xFF]


def build_stream():
    block = bytearray(GS16_BLOCK); block[1] = GS16_CMD
    for i in range(2, GS16_BLOCK - 1):
        block[i] = 0xFF          # all pixels max -> panel fully lit (visible)
    block[-1] = 0xFF             # duty_cycle
    ones = bin(0x01).count("1") + bin(GS16_CMD).count("1") + sum(bin(b).count("1") for b in block[2:])
    block[0] = 0x01 | ((ones & 1) << 7)
    body = bytearray(b"FR\x00\x00") + bytes(block) * NUM_PANELS
    return bytes([0x32, len(body) & 0xFF, (len(body) >> 8) & 0xFF]) + bytes(body)


def find(vid):
    return next((p.device for p in list_ports.comports() if p.vid == vid), None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mhz", type=int, default=5)
    ap.add_argument("--seconds", type=float, default=3.0)
    args = ap.parse_args()

    mport = find(0x16C0); pport = find(0x2E8A)
    if not mport or not pport:
        print(f"need both ports; master={mport} panel={pport}", file=sys.stderr); sys.exit(1)
    print(f"# master={mport} panel={pport}  clock={args.mhz}MHz")

    m = serial.Serial(mport, 115200, timeout=2.0)
    p = serial.Serial(pport, 115200, timeout=2.0)
    time.sleep(0.4); m.reset_input_buffer(); p.reset_input_buffer()

    def mcmd(frame):
        m.write(bytes(frame)); m.flush()
        h = m.read(1)
        if h: m.read(h[0])

    try:
        mcmd([0x01, 0x00]); time.sleep(0.2); m.reset_input_buffer()  # stop any stream
        p.write(b"z"); p.flush()                                     # zero panel counters
        mcmd([0x03, 0x17, *u16le(args.mhz)])                         # clock
        mcmd([0x03, 0x16, *u16le(1500)])                             # refresh
        mcmd(build_stream())                                         # start streaming
        time.sleep(0.4); m.reset_input_buffer()

        buf = b""; t0 = time.time()
        while time.time() - t0 < args.seconds:
            ch = m.read(512)
            if ch: buf += ch

        mcmd([0x01, 0x00])  # stop stream so the panel 'd' dump isn't perturbed

        # Panel dump = proof of life + receive count.
        p.reset_input_buffer(); p.write(b"d"); p.flush(); time.sleep(1.2)
        pdump = p.read(2048).decode("ascii", "replace")
    finally:
        mcmd([0x03, 0x17, *u16le(10)]); mcmd([0x01, 0x1A])
        m.close(); p.close()

    import re
    msgs = re.search(r"msgs=(\d+)", pdump)
    msgs = int(msgs.group(1)) if msgs else None
    rej = re.search(r"reject_any=(\d+)", pdump)
    rej = int(rej.group(1)) if rej else None
    print(f"\n# panel receive proof: msgs={msgs} reject_any={rej}  "
          f"({'ALIVE+RECEIVING' if msgs else 'NOT RECEIVING — result invalid'})")

    cipo = [l.strip().split("] ", 1)[-1] for l in buf.decode("ascii", "replace").splitlines()
            if "CIPO" in l]
    uniq = []
    for l in cipo:
        if l not in uniq: uniq.append(l)
    print(f"# CIPO debug lines ({len(cipo)} total, unique):")
    for l in uniq[:12]:
        print("  " + l)
    if msgs and all("all sets/buses read 0x00" in l for l in uniq) and uniq:
        print("\n=> VERDICT: panel was receiving but the master saw 0x00 on CIPO across "
              "all sets/buses — the confirmation is NOT reaching the master.")
    elif msgs:
        print("\n=> see the byte dumps above: byte index of the 0x30 cmd shows where the "
              "confirmation landed (0..2 = OK; ~8 = TX-FIFO shift).")


if __name__ == "__main__":
    main()
