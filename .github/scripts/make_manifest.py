#!/usr/bin/env python3
"""Aggregate per-build artifact-*.json files (one per release.yml matrix leg)
into the firmware manifest.json — the build catalog consumed by both the
g6-flash CLI and the WebUSB browser flasher.

Each entry is a *selectable build*:
    rev          hardware revision (v0.2.1 / v0.3.1)
    variant      production | bcmtest | <future debug/feature build>
    label        human label shown in the flasher dropdown
    file         UF2 filename (alongside this manifest)
    sha256       integrity check
    usb_product  expected USB product string (post-flash verify)
    default      true on exactly one entry (the flasher's initial selection)

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

# Sort/default policy: production builds first, newest hardware rev first, so the
# default (first production v0.3.1) lands at the top of the dropdown.
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

    default = next(
        (b for b in builds if b.get("rev") == DEFAULT_REV and b.get("variant") == "production"),
        builds[0],
    )
    for b in builds:
        b["default"] = b["file"] == default["file"]

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
