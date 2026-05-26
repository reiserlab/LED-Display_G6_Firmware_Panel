"""
5x7 fixed-width bitmap font.

Hand-rolled for the V1 panel error-glyph subsystem. Includes only the
characters needed for PE/CE/ERR codes: digits 0-9 plus letters C, E, P, R.

Each glyph is 5 columns wide, 7 rows tall. Rows are top-to-bottom, columns
left-to-right. '1' = pixel on, '0' = pixel off. Stored as a list of 7 ints
(one per row), with bits 4..0 representing columns 0..4 (MSB-leftmost).

The PE/CE example in the spec at g6_01-panel-protocol.md:371-392 uses a
slightly different (taller, square-ish) glyph style. The 5x7 form here is
the conventional minimum readable size and lets two characters fit on each
of the panel's two 20x10 half-rows with margin.
"""

# Each glyph: 7 rows × 5 columns, MSB-leftmost in the low 5 bits of each row.
_GLYPHS_5x7 = {
    '0': [
        0b01110,
        0b10001,
        0b10011,
        0b10101,
        0b11001,
        0b10001,
        0b01110,
    ],
    '1': [
        0b00100,
        0b01100,
        0b00100,
        0b00100,
        0b00100,
        0b00100,
        0b01110,
    ],
    '2': [
        0b01110,
        0b10001,
        0b00001,
        0b00010,
        0b00100,
        0b01000,
        0b11111,
    ],
    '3': [
        0b11110,
        0b00001,
        0b00001,
        0b01110,
        0b00001,
        0b00001,
        0b11110,
    ],
    '4': [
        0b00010,
        0b00110,
        0b01010,
        0b10010,
        0b11111,
        0b00010,
        0b00010,
    ],
    '5': [
        0b11111,
        0b10000,
        0b11110,
        0b00001,
        0b00001,
        0b10001,
        0b01110,
    ],
    '6': [
        0b00110,
        0b01000,
        0b10000,
        0b11110,
        0b10001,
        0b10001,
        0b01110,
    ],
    '7': [
        0b11111,
        0b00001,
        0b00010,
        0b00100,
        0b01000,
        0b01000,
        0b01000,
    ],
    '8': [
        0b01110,
        0b10001,
        0b10001,
        0b01110,
        0b10001,
        0b10001,
        0b01110,
    ],
    '9': [
        0b01110,
        0b10001,
        0b10001,
        0b01111,
        0b00001,
        0b00010,
        0b01100,
    ],
    'C': [
        0b01110,
        0b10001,
        0b10000,
        0b10000,
        0b10000,
        0b10001,
        0b01110,
    ],
    'E': [
        0b11111,
        0b10000,
        0b10000,
        0b11110,
        0b10000,
        0b10000,
        0b11111,
    ],
    'P': [
        0b11110,
        0b10001,
        0b10001,
        0b11110,
        0b10000,
        0b10000,
        0b10000,
    ],
    'R': [
        0b11110,
        0b10001,
        0b10001,
        0b11110,
        0b10100,
        0b10010,
        0b10001,
    ],
}

GLYPH_WIDTH = 5
GLYPH_HEIGHT = 7


def has_glyph(ch):
    return ch in _GLYPHS_5x7


def glyph_rows(ch):
    """Return a list of 7 ints (one per row); bits 4..0 = columns 0..4."""
    if ch not in _GLYPHS_5x7:
        raise KeyError("no glyph for {!r}".format(ch))
    return list(_GLYPHS_5x7[ch])


def glyph_pixel(ch, row, col):
    """Return 0 or 1 for the pixel at (row, col) of glyph `ch`."""
    rows = glyph_rows(ch)
    return (rows[row] >> (GLYPH_WIDTH - 1 - col)) & 1


def ascii_art(ch):
    """Debug helper: return a printable ASCII rendering of a glyph."""
    out = []
    for row in glyph_rows(ch):
        line = "".join(
            "#" if (row >> (GLYPH_WIDTH - 1 - c)) & 1 else "."
            for c in range(GLYPH_WIDTH)
        )
        out.append(line)
    return "\n".join(out)


if __name__ == "__main__":
    for ch in sorted(_GLYPHS_5x7):
        print("=== {!r} ===".format(ch))
        print(ascii_art(ch))
        print()
