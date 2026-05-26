"""
Round-trip test for the predefined-pattern generator.

Generates the blob in-memory, parses it back as the firmware would, and
verifies that every populated slot's unpacked pixel grid matches the
direct render from the font.

Run from repo root:
    python3 panel/tools/test_gen_predef_patterns.py
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import gen_predef_patterns as gen


def parse_blob(blob):
    """Mirror what the C++ loader will do; return {slot: pixel_grid}."""
    if blob[0:4] != gen.MAGIC:
        raise ValueError("bad magic: {!r}".format(blob[0:4]))
    version, pattern_count, toc_offset, _reserved = struct.unpack_from(
        "<HHII", blob, 4
    )
    if version != gen.VERSION:
        raise ValueError("bad version: {}".format(version))

    populated = {}
    for slot in range(pattern_count):
        entry_off = toc_offset + slot * gen.TOC_ENTRY_BYTES
        offset, length, fmt, _ = struct.unpack_from("<IHBB", blob, entry_off)
        if length == 0:
            continue  # unprogrammed
        if fmt != gen.FORMAT_GRAY_2:
            raise ValueError("slot {}: unexpected format {}".format(slot, fmt))
        payload = blob[offset:offset + length]
        rows, cols, fmt2, _duty = struct.unpack_from("<BBBB", payload, 0)
        assert fmt2 == fmt
        pixels = payload[4:]
        # Unpack Gray_2 (1 bit per pixel, MSB-first within byte, row-major).
        grid = [[0] * cols for _ in range(rows)]
        bit_idx = 0
        for r in range(rows):
            for c in range(cols):
                byte_idx = bit_idx // 8
                bit_pos = 7 - (bit_idx % 8)
                grid[r][c] = (pixels[byte_idx] >> bit_pos) & 1
                bit_idx += 1
        populated[slot] = grid
    return populated


def main():
    catalog = gen._build_catalog()
    blob = gen.build_blob(catalog)

    # Structural assertions
    assert blob[0:4] == gen.MAGIC, "magic missing"
    version, count, toc_off, _ = struct.unpack_from("<HHII", blob, 4)
    assert version == gen.VERSION
    assert count == gen.TOTAL_SLOTS
    assert toc_off == gen.HEADER_BYTES, toc_off
    expected_payload_region_start = gen.HEADER_BYTES + gen.TOTAL_SLOTS * gen.TOC_ENTRY_BYTES
    populated_count = len(catalog)
    expected_payload_bytes = populated_count * (4 + 50)  # per-payload header + Gray_2 50B
    assert len(blob) == expected_payload_region_start + expected_payload_bytes, (
        "blob length {} != expected {}".format(
            len(blob), expected_payload_region_start + expected_payload_bytes,
        )
    )

    # Parse back and compare pixel-by-pixel against direct font render.
    populated = parse_blob(blob)
    assert set(populated.keys()) == {slot for slot, _ in catalog}, (
        "parsed slot set differs from catalog"
    )
    for slot, text in catalog:
        expected_grid = gen.compose_2_char(text)
        got_grid = populated[slot]
        assert got_grid == expected_grid, (
            "slot {} ({!r}) pixel mismatch".format(slot, text)
        )

    # Spot-check: confirm an unprogrammed slot is correctly zero-length.
    unprog_slot = 50  # PE50 — not populated
    entry_off = toc_off + unprog_slot * gen.TOC_ENTRY_BYTES
    _off, length, fmt, _ = struct.unpack_from("<IHBB", blob, entry_off)
    assert length == 0, "expected unprogrammed slot 50 to have length=0"
    assert fmt == 0, "expected unprogrammed slot 50 to have format=0"

    print("OK: blob is {} bytes, {} populated slots".format(len(blob), populated_count))


if __name__ == "__main__":
    main()
