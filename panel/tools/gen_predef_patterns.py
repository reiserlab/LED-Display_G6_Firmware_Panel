"""
Build-time generator for the V1 predefined-pattern flash blob.

Composes PE/CE/ERR error glyphs from the 5x7 font in font_5x7.py and emits:
  - panel/src/predef_patterns_blob.h    (the binary blob as a C++ uint8_t[]
                                         array; #include'd into predef_patterns.cpp)
  - panel/src/predef_patterns_table.h   (slot constants for C++ to reference)
  - panel/src/predef_patterns.bin       (debugging artifact; not compiled in)

V1 catalog:
  slot   0    : "ERR"          (generic fallback error glyph)
  slot   1..99: "PE01".."PE99" (panel errors; only PE01-04 populated)
  slot 100..199: "CE00".."CE99" (controller errors; only CE00 populated)

Unprogrammed slots have TOC `length = 0`; the runtime loader falls back to
slot 0 ("ERR") when an unprogrammed slot is requested.

Blob format (little-endian throughout):
  Header (16 bytes):
    [0..4)   magic:        4 ASCII bytes "G6PP"
    [4..6)   version:      u16  (= 0x0001)
    [6..8)   pattern_count u16
    [8..12)  toc_offset:   u32  (bytes from blob start to entries[])
    [12..16) reserved:     u32  (= 0)
  TOC entries (8 bytes each, pattern_count of them):
    [0..4)   offset:       u32  (bytes from blob start to payload, or 0 if absent)
    [4..6)   length:       u16  (payload size in bytes, or 0 if absent)
    [6..7)   format:       u8   (0 = Gray_2 packed MSB-first; 1 = Gray_16 nibble-packed)
    [7..8)   reserved:     u8   (= 0)
  Payload (variable):
    [0]      rows:               u8
    [1]      cols:               u8
    [2]      format:              u8 (= TOC format)
    [3]      default_duty_cycle: u8
    [4..]    packed pixel data
"""

import argparse
import os
import struct
import sys

# Resolve the directory containing this script. Under PlatformIO's SConscript,
# `__file__` is undefined (the script is exec'd directly), so fall back to
# the env's project dir. Both paths need to add `tools/` to sys.path so that
# `import font_5x7` resolves.
try:
    _SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
except NameError:
    # Under PlatformIO; resolve via SCons env (set up below in the
    # try-Import block too).
    _SCRIPT_DIR = None

if _SCRIPT_DIR is None:
    try:
        Import("env")  # noqa: F821  (provided by PlatformIO/SCons)
        _SCRIPT_DIR = os.path.join(env.subst("$PROJECT_DIR"), "tools")  # noqa: F821
    except NameError:
        _SCRIPT_DIR = os.getcwd()

sys.path.insert(0, _SCRIPT_DIR)
from font_5x7 import GLYPH_HEIGHT, GLYPH_WIDTH, glyph_pixel, has_glyph
HERE = _SCRIPT_DIR

# ---------------- Panel and layout constants ----------------

PANEL_ROWS = 20
PANEL_COLS = 20

# Two 5x7 characters per row, two rows of characters.
#   cols:  4 (margin) + 5 (glyph) + 2 (gap) + 5 (glyph) + 4 (margin) = 20
#   rows:  2 (margin) + 7 (top)   + 2 (gap) + 7 (bot)   + 2 (margin) = 20
TOP_ROW_OFFSET = 2
BOT_ROW_OFFSET = 11
LEFT_COL_OFFSET = 4
INTER_CHAR_COL = LEFT_COL_OFFSET + GLYPH_WIDTH + 2  # = 11
DEFAULT_DUTY_CYCLE = 192  # ~75% — visible but not blinding

# ---------------- Blob format constants ----------------

MAGIC = b"G6PP"
VERSION = 0x0001
HEADER_BYTES = 16
TOC_ENTRY_BYTES = 8

FORMAT_GRAY_2 = 0
# FORMAT_GRAY_16 = 1  # reserved for V3

# ---------------- Catalog ----------------

SLOT_ERR = 0
SLOT_PE_BASE = 1     # PE01 -> slot 1, PE02 -> slot 2, ...
SLOT_CE_BASE = 100   # CE00 -> slot 100, CE01 -> slot 101, ...
TOTAL_SLOTS = 200    # slot 0 + PE01-99 (slots 1-99) + CE00-99 (slots 100-199)


def _build_catalog():
    """Return list of (slot_index, text) tuples for V1-populated slots.

    Unpopulated slots are *not* in the returned list; they appear as
    zero-length entries in the TOC.
    """
    catalog = []
    catalog.append((SLOT_ERR, "ERR"))

    # V1 populated panel-error codes
    for n in (1, 2, 3, 4, 5):
        catalog.append((SLOT_PE_BASE + (n - 1), "PE{:02d}".format(n)))

    # V1 populated controller-error codes
    for n in (0,):
        catalog.append((SLOT_CE_BASE + n, "CE{:02d}".format(n)))

    return catalog


# ---------------- Glyph composition ----------------

def compose_2_char(text):
    """Render `text` (1-3 chars) into a 20x20 mono pixel matrix.

    1 char  -> centered on top row only
    2 chars -> top row pair (PE / CE)
    3 chars -> first 2 on top row, third on bottom row centered (used for "ERR")
    4 chars -> first 2 on top, next 2 on bottom (used for "PE01" etc.)
    """
    if not text:
        raise ValueError("text must be non-empty")
    if len(text) > 4:
        raise ValueError("text too long: {!r}".format(text))

    grid = [[0] * PANEL_COLS for _ in range(PANEL_ROWS)]
    for ch in text:
        if not has_glyph(ch):
            raise KeyError("no glyph for {!r} (text={!r})".format(ch, text))

    def _draw(ch, row_off, col_off):
        for r in range(GLYPH_HEIGHT):
            for c in range(GLYPH_WIDTH):
                if glyph_pixel(ch, r, c):
                    grid[row_off + r][col_off + c] = 1

    if len(text) == 4:
        _draw(text[0], TOP_ROW_OFFSET, LEFT_COL_OFFSET)
        _draw(text[1], TOP_ROW_OFFSET, INTER_CHAR_COL)
        _draw(text[2], BOT_ROW_OFFSET, LEFT_COL_OFFSET)
        _draw(text[3], BOT_ROW_OFFSET, INTER_CHAR_COL)
    elif len(text) == 3:
        # PE/CE-style: first 2 chars on top, third centered on bottom.
        _draw(text[0], TOP_ROW_OFFSET, LEFT_COL_OFFSET)
        _draw(text[1], TOP_ROW_OFFSET, INTER_CHAR_COL)
        center_col = (PANEL_COLS - GLYPH_WIDTH) // 2  # = 7
        _draw(text[2], BOT_ROW_OFFSET, center_col)
    elif len(text) == 2:
        _draw(text[0], TOP_ROW_OFFSET, LEFT_COL_OFFSET)
        _draw(text[1], TOP_ROW_OFFSET, INTER_CHAR_COL)
    else:  # len 1
        center_col = (PANEL_COLS - GLYPH_WIDTH) // 2
        _draw(text[0], TOP_ROW_OFFSET, center_col)

    return grid


def pack_gray_2(grid):
    """Pack a 20x20 binary grid into 50 bytes, MSB-first within each byte.

    Pixel order: row-major (row 0 first), MSB of byte 0 = pixel(0, 0).
    """
    bits = []
    for r in range(PANEL_ROWS):
        for c in range(PANEL_COLS):
            bits.append(1 if grid[r][c] else 0)
    out = bytearray()
    for i in range(0, len(bits), 8):
        b = 0
        for j in range(8):
            b = (b << 1) | bits[i + j]
        out.append(b)
    return bytes(out)


def ascii_art(grid):
    """Render the 20x20 grid as ASCII for debugging."""
    return "\n".join(
        "".join("#" if grid[r][c] else "." for c in range(PANEL_COLS))
        for r in range(PANEL_ROWS)
    )


# ---------------- Blob assembly ----------------

def build_blob(catalog, total_slots=TOTAL_SLOTS):
    """Return the full binary blob for the given catalog."""
    # Build payload bytes first
    payloads = {}  # slot -> bytes
    for slot, text in catalog:
        grid = compose_2_char(text)
        pixels = pack_gray_2(grid)
        payload_header = struct.pack(
            "<BBBB",
            PANEL_ROWS,
            PANEL_COLS,
            FORMAT_GRAY_2,
            DEFAULT_DUTY_CYCLE,
        )
        payloads[slot] = payload_header + pixels

    # Layout: [Header][TOC][Payloads]
    toc_offset = HEADER_BYTES
    payload_region_start = toc_offset + total_slots * TOC_ENTRY_BYTES

    toc_entries = bytearray()
    payload_region = bytearray()
    cursor = payload_region_start
    for slot in range(total_slots):
        if slot in payloads:
            pl = payloads[slot]
            toc_entries += struct.pack(
                "<IHBB",
                cursor,             # offset
                len(pl),            # length
                FORMAT_GRAY_2,      # format
                0,                  # reserved
            )
            payload_region += pl
            cursor += len(pl)
        else:
            toc_entries += struct.pack("<IHBB", 0, 0, 0, 0)

    header = struct.pack(
        "<4sHHII",
        MAGIC,
        VERSION,
        total_slots,
        toc_offset,
        0,  # reserved
    )

    blob = header + bytes(toc_entries) + bytes(payload_region)
    return blob


# ---------------- Slot-constant C++ header ----------------

def build_blob_h(blob):
    """Emit `predef_patterns_blob.h` with the blob as a uint8_t array.

    A generated header is more portable across PlatformIO's
    assembler/include-path configurations than `.incbin`. The .cpp file
    includes this header exactly once (anonymous-namespace internal linkage).
    """
    lines = [
        "// Auto-generated by panel/tools/gen_predef_patterns.py — do not edit.",
        "#ifndef PREDEF_PATTERNS_BLOB_H",
        "#define PREDEF_PATTERNS_BLOB_H",
        "#include <stdint.h>",
        "",
        "namespace predef_blob_data {",
        "",
        "constexpr uint32_t kPredefBlobSize = {};".format(len(blob)),
        "",
        "constexpr uint8_t kPredefBlob[kPredefBlobSize] = {",
    ]
    # 16 bytes per source line, hex literal
    for i in range(0, len(blob), 16):
        chunk = blob[i:i + 16]
        hex_bytes = ", ".join("0x{:02X}".format(b) for b in chunk)
        lines.append("    " + hex_bytes + ",")
    lines.append("};")
    lines.append("")
    lines.append("}  // namespace predef_blob_data")
    lines.append("")
    lines.append("#endif  // PREDEF_PATTERNS_BLOB_H")
    lines.append("")
    return "\n".join(lines)


def build_header_h(catalog, total_slots=TOTAL_SLOTS):
    lines = [
        "// Auto-generated by panel/tools/gen_predef_patterns.py — do not edit.",
        "#ifndef PREDEF_PATTERNS_TABLE_H",
        "#define PREDEF_PATTERNS_TABLE_H",
        "#include <stdint.h>",
        "",
        "// Total slot count baked into the blob.",
        "constexpr uint32_t PREDEF_TOTAL_SLOTS = {};".format(total_slots),
        "",
        "// V1 reserved slot numbering. Slot 0 is the canonical error-glyph slot",
        "// (per g6_01-panel-protocol.md V3 spec); 1..99 are PE codes; 100..199",
        "// are CE codes; 200+ are reserved for V3 test/calibration patterns.",
        "constexpr uint32_t PREDEF_SLOT_ERR     = 0;",
        "constexpr uint32_t PREDEF_SLOT_PE_BASE = 1;     // PE01 -> slot 1",
        "constexpr uint32_t PREDEF_SLOT_CE_BASE = 100;   // CE00 -> slot 100",
        "",
        "// V1 panel-error code mnemonics, for use at error-trigger call sites.",
    ]
    for slot, text in catalog:
        if text.startswith("PE") and len(text) == 4:
            n = int(text[2:])
            lines.append(
                "constexpr uint32_t PREDEF_SLOT_PE{:02d} = PREDEF_SLOT_PE_BASE + {};".format(n, n - 1)
            )
        elif text.startswith("CE") and len(text) == 4:
            n = int(text[2:])
            lines.append(
                "constexpr uint32_t PREDEF_SLOT_CE{:02d} = PREDEF_SLOT_CE_BASE + {};".format(n, n)
            )
    lines.append("")
    lines.append("#endif  // PREDEF_PATTERNS_TABLE_H")
    lines.append("")
    return "\n".join(lines)


# ---------------- CLI ----------------

def _write_outputs(src_dir):
    """Generate all output files into `src_dir`."""
    out_bin = os.path.join(src_dir, "predef_patterns.bin")
    out_blob_h = os.path.join(src_dir, "predef_patterns_blob.h")
    out_table_h = os.path.join(src_dir, "predef_patterns_table.h")

    catalog = _build_catalog()
    blob = build_blob(catalog)
    blob_h = build_blob_h(blob)
    table_h = build_header_h(catalog)

    with open(out_bin, "wb") as f:
        f.write(blob)
    with open(out_blob_h, "w") as f:
        f.write(blob_h)
    with open(out_table_h, "w") as f:
        f.write(table_h)

    print("predef-patterns: wrote {} bytes to {}".format(len(blob), out_bin))
    print("predef-patterns: wrote {} bytes of C++ to {}".format(len(blob_h), out_blob_h))
    print("predef-patterns: wrote slot-constant header to {}".format(out_table_h))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--src-dir",
        default=os.path.join(HERE, os.pardir, "src"),
        help="Directory to write predef_patterns.bin + .h files into.",
    )
    parser.add_argument("--dump-ascii", action="store_true",
                        help="Print ASCII art of every populated slot.")
    args = parser.parse_args()

    _write_outputs(os.path.abspath(args.src_dir))

    if args.dump_ascii:
        catalog = _build_catalog()
        for slot, text in catalog:
            print("--- slot {} = {!r} ---".format(slot, text))
            print(ascii_art(compose_2_char(text)))
            print()


# PlatformIO `extra_scripts` import: SCons provides `Import("env")`. We write
# the outputs at script-load time so they exist before SCons enumerates the
# build's source files. This is fast (single-digit ms) and idempotent.
try:
    Import("env")  # noqa: F821  (provided by PlatformIO/SCons)
    _src_dir = env.subst("$PROJECT_SRC_DIR")  # noqa: F821
    _write_outputs(_src_dir)
except NameError:
    # Not running inside PlatformIO; CLI usage only.
    pass


if __name__ == "__main__":
    main()
