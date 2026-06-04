#!/usr/bin/env python3
"""Arena (Teensy) stream control for the LAB-43 demo — system python3 (pyserial).
Robust against the arena debug build: drains serial first, write-only (no response
parsing). Modes: alloff | allon | triggered (GS2 all-on; duty sets per-row on-time).
  python3 stream.py --mode triggered --duty 85
"""
import argparse, time, serial
from serial.tools import list_ports
GS2 = 53; NUM = 20

def frame(cmd, duty):
    b = bytearray(GS2); b[1] = cmd
    for i in range(2, 52): b[i] = 0xFF          # 50 pixel bytes = all-on
    b[52] = duty & 0xFF                          # duty_cycle
    pd = b[2:GS2]
    ones = bin(0x01).count("1") + bin(cmd).count("1") + sum(bin(x).count("1") for x in pd)
    b[0] = 0x01 | ((ones & 1) << 7)              # version | even-parity
    body = bytearray(b"FR\x00\x00") + bytes(b) * NUM
    return bytes([0x32, len(body) & 0xFF, (len(body) >> 8) & 0xFF]) + bytes(body)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["alloff", "allon", "triggered"], default="alloff")
    ap.add_argument("--duty", type=int, default=255)
    ap.add_argument("--refresh", type=int, default=200)
    a = ap.parse_args()
    ports = [x.device for x in list_ports.comports() if x.vid == 0x16C0]
    if not ports:
        raise SystemExit("arena (VID 0x16C0 / Teensy) not found on USB")
    s = serial.Serial(ports[0], 115200, timeout=0.3); time.sleep(0.35); s.reset_input_buffer()
    t = time.time()
    while time.time() - t < 0.5:                 # drain any debug-build chatter
        if not s.read(512): break
    def send(f, st=0.2): s.write(bytes(f)); s.flush(); time.sleep(st); s.reset_input_buffer()
    send([0x01, 0x00])                           # ALL_OFF (clean state)
    if a.mode != "alloff":
        cmd = 0x12 if a.mode == "triggered" else 0x10   # 0x12 GS2 Triggered / 0x10 GS2 Oneshot
        send(frame(cmd, a.duty), 0.35)
        send([0x03, 0x16, a.refresh & 0xFF, a.refresh >> 8])  # SET_REFRESH
    s.close()
    print(f"arena: {a.mode}" + ("" if a.mode == "alloff" else f" duty={a.duty} refresh={a.refresh}Hz"))

if __name__ == "__main__":
    main()
