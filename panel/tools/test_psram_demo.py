"""
Host-side reference model + tests for the LAB-41/42 V2 PSRAM demo.

This locks the two contracts the panel firmware, the arena firmware, the
panel_controller bench harness, and the on-device self-test all have to agree
on:

  1. The V2 "display PSRAM index" wire format (header version, parity, CRC-8,
     payload sizes, little-endian index).
  2. The 100-frame demo animation catalog (H-bar sweep, V-bar sweep, drifting
     checkerboard) and its Gray_16 nibble packing — the same layout the
     firmware's psram_store::build_demo_frame() + Message Gray_16 codec produce.

Pure Python, no firmware import — it is the independent reference the on-device
read-back self-test (PSRAM_SELFTEST build) is checked against by eye, and that
documents the format for the arena team.

Run either way:
    python3 panel/tools/test_psram_demo.py
    pytest  panel/tools/test_psram_demo.py
"""

# ---------------------------------------------------------------------------
# Constants — must match panel/src/protocol.{h,cpp} and psram_store.h
# ---------------------------------------------------------------------------
PANEL_SIZE = 20
FRAME_COUNT = 100
FRAME_BYTES = PANEL_SIZE * PANEL_SIZE // 2 + 1  # 200 nibble bytes + 1 duty = 201
HEADER_SIZE = 2

CMD_PROTOCOL_V1 = 0x01
CMD_PROTOCOL_V2 = 0x02

CMD_ID_DISPLAY_PSRAM = 0x50          # Oneshot, implicit duty
CMD_ID_DISPLAY_PSRAM_PERSIST = 0x51  # Persistent, implicit duty
CMD_ID_DISPLAY_PSRAM_DUTY = 0x60     # Oneshot, explicit duty
CMD_ID_DISPLAY_PSRAM_DUTY_PERSIST = 0x61

PAYLOAD_DISPLAY_PSRAM = 2       # 16-bit LE index
PAYLOAD_DISPLAY_PSRAM_DUTY = 3  # 16-bit LE index + 1 duty

# Demo layout (psram_store.h)
H_SWEEP_BEGIN = 0
V_SWEEP_BEGIN = 40
CHECKER_BEGIN = 80


# ---------------------------------------------------------------------------
# Wire helpers (mirror panel/src/message.cpp)
# ---------------------------------------------------------------------------
def popcount(x):
    return bin(x & 0xFF).count("1")


def parity_bit(version_byte, cmd, payload):
    """Even parity over version bits[0..6] + cmd + payload (Message::calculate_parity_bit)."""
    ones = popcount(version_byte & 0x7F) + popcount(cmd)
    for b in payload:
        ones += popcount(b)
    return ones & 1


def crc8_autosar(data):
    """CRC-8/AUTOSAR: poly 0x2F, init 0xFF, refin/refout false, xorout 0xFF."""
    crc = 0xFF
    for b in data:
        crc ^= b & 0xFF
        for _ in range(8):
            crc = ((crc << 1) ^ 0x2F) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc ^ 0xFF


def build_psram_frame(index, cmd_id=CMD_ID_DISPLAY_PSRAM_PERSIST, duty=None):
    """Build a full V2 display-PSRAM-index wire frame (header..payload)."""
    explicit = (cmd_id & 0xF0) == 0x60
    payload = [index & 0xFF, (index >> 8) & 0xFF]
    if explicit:
        assert duty is not None, "0x6x commands require an explicit duty"
        payload.append(duty & 0xFF)
    p = parity_bit(CMD_PROTOCOL_V2, cmd_id, payload)
    header = CMD_PROTOCOL_V2 | (p << 7)
    return bytes([header, cmd_id] + payload)


# ---------------------------------------------------------------------------
# Gray_16 nibble packing (mirror Message::from/to_pattern_gray_16)
# ---------------------------------------------------------------------------
def pack_gray_16(grid, duty):
    """20x20 grid of 0..15 -> 200 nibble bytes (2 px/byte, even px = high nibble) + duty."""
    out = bytearray(FRAME_BYTES)
    pixel = 0
    for i in range(PANEL_SIZE):
        for j in range(PANEL_SIZE):
            v = grid[i][j] & 0x0F
            byte = pixel // 2
            if pixel % 2 == 0:
                out[byte] = (out[byte] & 0x0F) | (v << 4)
            else:
                out[byte] = (out[byte] & 0xF0) | v
            pixel += 1
    out[FRAME_BYTES - 1] = duty & 0xFF
    return bytes(out)


def unpack_gray_16(record):
    """Inverse of pack_gray_16 -> (grid, duty)."""
    grid = [[0] * PANEL_SIZE for _ in range(PANEL_SIZE)]
    pixel = 0
    for i in range(PANEL_SIZE):
        for j in range(PANEL_SIZE):
            byte = pixel // 2
            grid[i][j] = (record[byte] >> 4) if pixel % 2 == 0 else (record[byte] & 0x0F)
            pixel += 1
    return grid, record[FRAME_BYTES - 1]


# ---------------------------------------------------------------------------
# Demo catalog reference (mirror psram_store::build_demo_frame)
# ---------------------------------------------------------------------------
def demo_grid(index):
    """Return (grid, duty) for global frame index 0..99."""
    grid = [[0] * PANEL_SIZE for _ in range(PANEL_SIZE)]
    if index < V_SWEEP_BEGIN:                       # [0..39] horizontal bar
        f = index - H_SWEEP_BEGIN
        r = f if f < 20 else 39 - f                 # 0..19, 19..0
        for c in range(PANEL_SIZE):
            grid[r][c] = 15
        return grid, 255
    if index < CHECKER_BEGIN:                       # [40..79] vertical bar
        f = index - V_SWEEP_BEGIN
        c = f if f < 20 else 39 - f
        for r in range(PANEL_SIZE):
            grid[r][c] = 15
        return grid, 255
    p = index - CHECKER_BEGIN                        # [80..99] drifting checker
    for i in range(PANEL_SIZE):
        for j in range(PANEL_SIZE):
            grid[i][j] = 15 if (((i + p) // 2 + (j + p) // 2) & 1) else 0
    return grid, 192


def demo_record(index):
    grid, duty = demo_grid(index)
    return pack_gray_16(grid, duty)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
def test_crc8_check_value():
    assert crc8_autosar(b"123456789") == 0xDF


def test_payload_sizes_and_index_le():
    f51 = build_psram_frame(0x1234, CMD_ID_DISPLAY_PSRAM_PERSIST)
    assert len(f51) == HEADER_SIZE + PAYLOAD_DISPLAY_PSRAM
    assert f51[1] == 0x51
    assert f51[2] == 0x34 and f51[3] == 0x12      # little-endian index

    f61 = build_psram_frame(7, CMD_ID_DISPLAY_PSRAM_DUTY_PERSIST, duty=128)
    assert len(f61) == HEADER_SIZE + PAYLOAD_DISPLAY_PSRAM_DUTY
    assert f61[1] == 0x61
    assert f61[2] == 7 and f61[3] == 0 and f61[4] == 128


def test_parity_is_even_and_valid():
    for idx in (0, 1, 99, 0x00FF, 0x0100, 0xFFFF):
        frame = build_psram_frame(idx, CMD_ID_DISPLAY_PSRAM_PERSIST)
        masked = bytes([frame[0] & 0x7F]) + frame[1:]
        ones = sum(popcount(b) for b in masked) + ((frame[0] >> 7) & 1)
        assert ones % 2 == 0, "header parity must make total 1-bits even (idx=%d)" % idx


def test_mode_in_low_nibble():
    for base in (0x50, 0x60):
        for mode in range(4):
            frame = build_psram_frame(3, base | mode,
                                      duty=(64 if base == 0x60 else None))
            assert (frame[1] & 0x0F) == mode
            assert (frame[1] & 0xF0) == base


def test_gray16_pack_roundtrip():
    for idx in (0, 19, 20, 39, 40, 59, 79, 80, 90, 99):
        grid, duty = demo_grid(idx)
        rec = pack_gray_16(grid, duty)
        assert len(rec) == FRAME_BYTES
        back_grid, back_duty = unpack_gray_16(rec)
        assert back_grid == grid and back_duty == duty


def test_demo_catalog_geometry():
    assert FRAME_COUNT == 100

    # H-bar sweep: frame f lights exactly one full row; row index sweeps 0..19..0.
    for f in range(40):
        grid, duty = demo_grid(f)
        assert duty == 255
        lit_rows = [r for r in range(PANEL_SIZE) if all(grid[r][c] == 15 for c in range(PANEL_SIZE))]
        assert lit_rows == [f if f < 20 else 39 - f]
        # nothing else lit
        assert sum(1 for r in range(PANEL_SIZE) for c in range(PANEL_SIZE) if grid[r][c]) == PANEL_SIZE
    assert demo_grid(0)[0] == demo_grid(39)[0]   # round-trip endpoints match (row 0)

    # V-bar sweep: frame lights exactly one full column; col sweeps 0..19..0.
    for f in range(40, 80):
        grid, _ = demo_grid(f)
        k = f - 40
        col = k if k < 20 else 39 - k
        lit_cols = [c for c in range(PANEL_SIZE) if all(grid[r][c] == 15 for r in range(PANEL_SIZE))]
        assert lit_cols == [col]

    # Checkerboard: 20 frames, drifting, half-ish lit, duty 192, values in {0,15}.
    prev = None
    for f in range(80, 100):
        grid, duty = demo_grid(f)
        assert duty == 192
        vals = {grid[i][j] for i in range(PANEL_SIZE) for j in range(PANEL_SIZE)}
        assert vals <= {0, 15}
        lit = sum(1 for i in range(PANEL_SIZE) for j in range(PANEL_SIZE) if grid[i][j])
        assert 150 <= lit <= 250            # ~half of 400
        if prev is not None:
            assert grid != prev             # it actually drifts frame-to-frame
        prev = grid


def test_all_indices_buildable():
    seen = set()
    for idx in range(FRAME_COUNT):
        rec = demo_record(idx)
        assert len(rec) == FRAME_BYTES
        seen.add(idx)
    assert len(seen) == FRAME_COUNT


def main():
    tests = [v for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
    for t in tests:
        t()
        print("ok:", t.__name__)
    print("OK: %d V2/PSRAM reference tests passed" % len(tests))


if __name__ == "__main__":
    main()
