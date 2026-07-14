#!/usr/bin/env python3
"""
Wrap a built panel firmware in the ISP image format the arena controller
expects for in-system programming (`g6-program-panel` / `ISP_*`).

The arena controller reflashes panels over SPI from a single firmware file on
its SD card (`/firmware/panel.bin`). That file is the raw flash image followed
by a 32-byte footer the controller validates before any SPI traffic. See the
Modular-LED-Display dev spec:
  docs/development/g6_03-controller.md  § Panel firmware update (ISP)
  docs/development/g6_01-panel-protocol.md  § In-System Programming (ISP)

ISP footer (32 bytes, little-endian), appended after the image:
  offset  size  field        notes
  0       8     magic        ASCII "G6PANFW\\0"
  8       16    version      ASCII, NUL-padded/truncated; free-form build id
  24      4     image_crc32  uint32 LE — CRC-32 (zlib/IEEE) over the image bytes
                             that precede the footer
  28      4     image_size   uint32 LE — length in bytes of the image preceding
                             the footer

The CRC and size cover only the image (not the footer). The controller streams
those `image_size` bytes to the panel and checks them with `ISP_VERIFY_CRC`
against `image_crc32`.

PlatformIO's earlephilhower RP2350 build emits firmware.uf2 (+ .elf) but no raw
.bin, so by default this reconstructs the flash image from the .uf2. A raw .bin
is used directly if present or pointed at with --bin.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import zlib
from pathlib import Path

MAGIC = b"G6PANFW\x00"          # 8 bytes
VERSION_FIELD_LEN = 16          # bytes
FOOTER_LEN = 32                 # 8 + 16 + 4 + 4

# UF2 block constants (https://github.com/microsoft/uf2)
UF2_BLOCK_SIZE = 512
UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_NOT_MAIN_FLASH = 0x00000001
# Safety cap: a contiguous reconstructed image larger than this is almost
# certainly a parse error (stray high-address block), not real firmware.
MAX_IMAGE_BYTES = 4 * 1024 * 1024


def uf2_to_bin(uf2_path: Path) -> bytes:
    """Reconstruct the contiguous flash image from a UF2 file."""
    data = uf2_path.read_bytes()
    if len(data) % UF2_BLOCK_SIZE != 0:
        sys.exit(f"error: {uf2_path} is not a multiple of {UF2_BLOCK_SIZE} bytes")

    chunks: list[tuple[int, bytes]] = []
    for off in range(0, len(data), UF2_BLOCK_SIZE):
        block = data[off:off + UF2_BLOCK_SIZE]
        start0, start1, flags, addr, payload_size, _blk, _num, _fid = struct.unpack(
            "<IIIIIIII", block[:32]
        )
        if start0 != UF2_MAGIC_START0 or start1 != UF2_MAGIC_START1:
            sys.exit(f"error: bad UF2 magic in block at offset {off}")
        if struct.unpack("<I", block[-4:])[0] != UF2_MAGIC_END:
            sys.exit(f"error: bad UF2 end magic in block at offset {off}")
        if flags & UF2_FLAG_NOT_MAIN_FLASH:
            continue
        if payload_size > 476:
            sys.exit(f"error: implausible UF2 payload_size {payload_size}")
        chunks.append((addr, block[32:32 + payload_size]))

    if not chunks:
        sys.exit(f"error: no main-flash blocks found in {uf2_path}")

    base = min(addr for addr, _ in chunks)
    end = max(addr + len(payload) for addr, payload in chunks)
    size = end - base
    if size > MAX_IMAGE_BYTES:
        sys.exit(
            f"error: reconstructed image is {size} bytes (> {MAX_IMAGE_BYTES}); "
            "likely a non-contiguous UF2 — inspect the build output"
        )

    image = bytearray(b"\xFF" * size)  # 0xFF = erased flash
    for addr, payload in chunks:
        image[addr - base:addr - base + len(payload)] = payload
    return bytes(image)


def default_version(repo: Path) -> str:
    """Compact git build id, e.g. '1a2b3c4d' or '1a2b3c4d-d' when dirty."""
    def git(*args: str) -> str:
        return subprocess.check_output(
            ["git", "-C", str(repo), *args],
            stderr=subprocess.DEVNULL,
        ).decode().strip()

    try:
        rev = git("rev-parse", "--short=8", "HEAD")
        dirty = bool(git("status", "--porcelain"))
        return rev + ("-d" if dirty else "")
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"


def encode_version(version: str) -> bytes:
    raw = version.encode("ascii", errors="replace")
    if len(raw) >= VERSION_FIELD_LEN:
        sys.exit(
            f"error: version '{version}' exceeds {VERSION_FIELD_LEN - 1} chars "
            "(would be truncated in the ISP footer)"
        )
    return raw.ljust(VERSION_FIELD_LEN, b"\x00")


def build_footer(image: bytes, version: str) -> bytes:
    footer = MAGIC + encode_version(version)
    footer += struct.pack("<II", zlib.crc32(image) & 0xFFFFFFFF, len(image))
    assert len(footer) == FOOTER_LEN, len(footer)
    return footer


def main() -> None:
    tools_dir = Path(__file__).resolve().parent
    repo = tools_dir.parents[1]  # .../Panel-Firmware

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--env", help="PlatformIO env name, e.g. pico_v021")
    ap.add_argument("--build-dir", default=str(repo / "panel" / ".pio" / "build"),
                    help="PlatformIO build root (default: panel/.pio/build)")
    ap.add_argument("--bin", help="use this raw .bin directly (skips UF2 reconstruction)")
    ap.add_argument("--out", required=True, help="output ISP image path")
    ap.add_argument("--version", help="version string for the footer "
                    f"(<= {VERSION_FIELD_LEN - 1} ASCII chars; default: git build id)")
    args = ap.parse_args()

    if args.bin:
        image = Path(args.bin).read_bytes()
    else:
        if not args.env:
            ap.error("either --env or --bin is required")
        env_dir = Path(args.build_dir) / args.env
        raw = env_dir / "firmware.bin"
        uf2 = env_dir / "firmware.uf2"
        if raw.is_file():
            image = raw.read_bytes()
        elif uf2.is_file():
            image = uf2_to_bin(uf2)
        else:
            sys.exit(f"error: no firmware.bin or firmware.uf2 in {env_dir} "
                     "(build first, e.g. `pio run -d panel -e <env>`)")

    version = args.version or default_version(repo)
    footer = build_footer(image, version)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(image + footer)

    crc, size = struct.unpack("<II", footer[24:32])
    print(f"ISP image: {out}")
    print(f"  version     : {version}")
    print(f"  image_size  : {size} bytes")
    print(f"  image_crc32 : 0x{crc:08X}")
    print(f"  footer      : 32 bytes  (total file {size + FOOTER_LEN} bytes)")


if __name__ == "__main__":
    main()
