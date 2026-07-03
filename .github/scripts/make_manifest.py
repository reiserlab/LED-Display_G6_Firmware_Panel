#!/usr/bin/env python3
"""Aggregate per-build artifact-*.json files (one per release.yml matrix leg)
into the firmware manifest.json — the build catalog consumed by g6-flash CLI,
the WebUSB browser flasher, and Arena Studio's over-SPI panel-firmware push.

Each entry is a *selectable build*:
    rev          hardware revision (v0.2.1 / v0.3.1)
    variant      production | bcmtest | <future debug/feature build>
    label        human label shown in the flasher dropdown
    usb_product  expected USB product string (post-flash verify)
    uf2          optional {file, sha256} — for g6-flash / the WebUSB flasher
    bin          optional {file, sha256} — the ISP-footer image, for Arena
                 Studio's / the arena controller's over-SPI push
    default      true on exactly one entry that HAS a `uf2` (the flasher's
                 initial selection); a bin-only entry is never the default

Production entries additionally carry (optional; consumed by the web console's
ISP firmware-push flow, ignored by the UF2 flasher):
    isp_file     footered ISP image (<slug>-isp.bin) alongside this manifest —
                 raw flash image + 32-byte G6PANFW footer (make_isp_image.py),
                 for arena SET_FIRMWARE_FILE (0xE0) -> g6-program-panel (0xC8)
    isp_sha256   integrity check for isp_file

Top-level version / commit / built identify the release. Adding a new build is
purely a new matrix leg in release.yml — it flows through here automatically, so
this stays the single extension point for future builds.

    usage: make_manifest.py <incoming_dir> <out_path>
Env: GITHUB_REF_NAME (tag), GITHUB_SHA (commit), BUILD_DATE (UTC timestamp).
"""
import glob
import json
import os
import sys

# Sort/default policy: production builds first, newest hardware rev first, so
# the default (first production v0.3.1 build) lands at the top of the dropdown.
REV_ORDER = {"v0.3.1": 0, "v0.2.1": 1}
DEFAULT_REV = "v0.3.1"


def main() -> int:
    incoming, out = sys.argv[1], sys.argv[2]
    builds = [
        json.load(open(f))
        for f in glob.glob(os.path.join(incoming, "**", "artifact-*.json"), recursive=True)
    ]
    if not builds:
        sys.exit(f"make_manifest: no artifact-*.json found under {incoming!r}")

    builds.sort(key=lambda b: (
        b.get("variant") != "production",       # production first
        REV_ORDER.get(b.get("rev"), 99),        # then newest rev
        b.get("variant", ""),
    ))

    # Only a build that HAS a uf2 can be the flasher's default selection — a
    # bin-only build (no "uf2" key) is never flasher-selectable, so it's
    # excluded from the candidate pool entirely.
    uf2_builds = [b for b in builds if "uf2" in b]
    default = next(
        (b for b in uf2_builds if b.get("rev") == DEFAULT_REV and b.get("variant") == "production"),
        uf2_builds[0] if uf2_builds else builds[0],
    )
    for b in builds:
        b["default"] = b.get("env") == default.get("env")

    manifest = {
        "version": os.environ.get("GITHUB_REF_NAME", "dev"),
        "commit": (os.environ.get("GITHUB_SHA") or "")[:7],
        "built": os.environ.get("BUILD_DATE", ""),
        "artifacts": builds,  # the build catalog
    }

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w") as f:
        json.dump(manifest, f, indent=2)
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
