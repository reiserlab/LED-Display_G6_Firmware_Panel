#!/opt/homebrew/bin/python3.14
"""~30 s narrated demo of the three G6 display modes driven by an EXTERNAL trigger.

AD3 W1 -> arena BNC J4 -> U3 (DIR=LOW) -> J30 shunt -> EINT fanout -> all panels.
Speaks (macOS `say`) and plays a tone before each phase, for a screen/video recording:

    1. STANDARD  (Persistent 0x31) — always on, no trigger.
    2. GATED     (0x33)            — display follows EINT level (high=show, low=blank).
    3. TRIGGERED (0x32)            — advances on EINT edges; held-high-no-edges stays dark;
                                     8 kHz edges -> bright.

Requires homebrew python3.14 (DWF). Arena must be on USB; panels powered + seated.
"""
import ctypes, time, subprocess
import serial
from serial.tools import list_ports

# ---- self-contained V1 all-on STREAM_FRAME builder (mirrors stream_trigger_test.py) ----
def build_allon_stream(cmd, duty=255):
    payload = bytes([0xFF] * 200 + [duty & 0xFF])          # 20x20 all px=15 + duty
    ones = bin(0x01).count("1") + bin(cmd).count("1") + sum(bin(b).count("1") for b in payload)
    header = 0x01 | ((ones & 1) << 7)                      # V1 header + even parity
    block = bytes([header, cmd]) + payload                 # 203 B
    frame = bytes([ord('F'), ord('R'), 0, 0]) + block * 20  # 4 + 20*203 = 4064
    n = len(frame)
    return bytes([0x32, n & 0xFF, (n >> 8) & 0xFF]) + frame  # STREAM_FRAME

PERSIST, GATED, TRIGGERED = 0x31, 0x33, 0x32

# ---- audio ----
def tone(name): subprocess.run(["afplay", f"/System/Library/Sounds/{name}.aiff"])
def say(t):     subprocess.run(["say", "-r", "192", t])
def cue(t, text): tone(t); say(text)

# ---- AD3 W1 ----
dwf = ctypes.cdll.LoadLibrary("/Library/Frameworks/dwf.framework/dwf")
_n = ctypes.c_int(); dwf.FDwfEnum(ctypes.c_int(0), ctypes.byref(_n))
_h = ctypes.c_int(); dwf.FDwfDeviceConfigOpen(ctypes.c_int(0), ctypes.c_int(1), ctypes.byref(_h))
_ch = ctypes.c_int(0); _nd = ctypes.c_int(0)
def w1_dc(v):
    dwf.FDwfAnalogOutNodeEnableSet(_h,_ch,_nd,ctypes.c_int(1)); dwf.FDwfAnalogOutNodeFunctionSet(_h,_ch,_nd,ctypes.c_byte(0))
    dwf.FDwfAnalogOutNodeAmplitudeSet(_h,_ch,_nd,ctypes.c_double(0.0)); dwf.FDwfAnalogOutNodeOffsetSet(_h,_ch,_nd,ctypes.c_double(v))
    dwf.FDwfAnalogOutConfigure(_h,_ch,ctypes.c_int(1))
def w1_sq(f):
    dwf.FDwfAnalogOutNodeEnableSet(_h,_ch,_nd,ctypes.c_int(1)); dwf.FDwfAnalogOutNodeFunctionSet(_h,_ch,_nd,ctypes.c_byte(2))
    dwf.FDwfAnalogOutNodeFrequencySet(_h,_ch,_nd,ctypes.c_double(f)); dwf.FDwfAnalogOutNodeAmplitudeSet(_h,_ch,_nd,ctypes.c_double(1.65))
    dwf.FDwfAnalogOutNodeOffsetSet(_h,_ch,_nd,ctypes.c_double(1.65)); dwf.FDwfAnalogOutNodeSymmetrySet(_h,_ch,_nd,ctypes.c_double(50.0))
    dwf.FDwfAnalogOutConfigure(_h,_ch,ctypes.c_int(1))
def w1_off(): dwf.FDwfAnalogOutConfigure(_h,_ch,ctypes.c_int(0))

# ---- arena ----
_arena = next((p.device for p in list_ports.comports() if p.vid == 0x16C0), None)
_a = serial.Serial(_arena, 115200, timeout=1.0); time.sleep(0.3); _a.reset_input_buffer()
def stream(cmd):
    _a.write(build_allon_stream(cmd)); _a.flush(); time.sleep(0.2); _a.reset_input_buffer()
def arena_off():
    _a.write(bytes([0x01,0x30])); _a.flush(); time.sleep(0.1); _a.write(bytes([0x01,0x00])); _a.flush()

# ---- the demo ----
try:
    arena_off(); w1_dc(0.0)
    say("G6 arena external trigger demo.")

    cue("Submarine", "Standard mode. Persistent display, always on, no trigger needed.")
    stream(PERSIST); w1_dc(0.0)
    time.sleep(4)

    cue("Ping", "Gated mode. The display follows the trigger level. Trigger high shows the image, low blanks it.")
    stream(GATED)
    w1_dc(3.3); time.sleep(2.0)
    w1_dc(0.0); time.sleep(1.5)
    say("And strobing the trigger.")
    w1_sq(3.0); time.sleep(3.0)

    cue("Glass", "Triggered mode. The display advances on trigger edges. Held high with no edges, it stays dark.")
    stream(TRIGGERED)
    w1_dc(3.3); time.sleep(2.5)
    say("Now eight kilohertz trigger edges.")
    w1_sq(8000.0); time.sleep(3.5)

    cue("Hero", "Demo complete. External triggering validated in both gated and triggered modes.")
finally:
    arena_off(); w1_off(); dwf.FDwfDeviceClose(_h); _a.close()
