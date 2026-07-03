#!/usr/bin/env python3
"""build_release — build the G6 panel firmware release catalog into dist/.

This is what release.yml's build job runs (`pixi run release`), so a bench
developer can preview the exact release payload (UF2s + BINs + manifest.json)
before ever pushing a `panel-fw-v*` tag — the CI workflow and the local task
are the same command, not two independently-maintained build paths.

Run from the repo root (same convention as g6_flash.py). Delegates
each leg's build to the matching `pixi run build21`/`build31`/`build21-bcmtest`/
`build31-bcmtest` task (see pixi.toml) instead of invoking `pio` itself, so the
build command for a given env is defined in exactly one place. Needs `pixi` on
PATH — trivially true here since `pixi run release` is what launches this
script.

Usage
-----
    pixi run release                                             # whole catalog, both formats
    python panel/tools/build_release.py --only g6-panel-v0.3.1   # one leg, both formats

Every build always gets wrapped into an ISP-footer `.bin` (via
make_isp_image.py) for the arena controller's over-SPI panel reflashing, in
addition to its `.uf2` — one build pipeline, one task, both formats every
time. Both formats live on the SAME manifest.json artifact entry, as nested
`"uf2": {"file", "sha256"}` (for g6-flash / the WebUSB flasher) and
`"bin": {"file", "sha256"}` (for Arena Studio's / the arena controller's
over-SPI push) — see make_manifest.py.

See docs/development/g6_07-panel-programming.md §A.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

MAKE_MANIFEST = Path(".github/scripts/make_manifest.py")
MAKE_ISP_IMAGE = Path("panel/tools/make_isp_image.py")

# The release catalog: one entry per selectable build. `task` names the pixi
# task (pixi.toml) that builds `env` — the single place that command lives;
# this script never invokes `pio` directly. Adding a build here is the single
# extension point for the catalog itself — it flows through to the GitHub
# Release, GitHub Pages, g6-flash's `--variant` choices, and the WebUSB
# flasher dropdown automatically via manifest.json (a new `env` still needs
# its own build task added to pixi.toml first).
CATALOG = [
    # Production builds (deployable). v0.3.1 is the flasher's default.
    {"env": "pico_v031", "task": "build31", "rev": "v0.3.1", "variant": "production",
     "label": "v0.3.1 — Production", "usb_product": "G6 Panel v0.3",
     "slug": "g6-panel-v0.3.1"},
    {"env": "pico_v021", "task": "build21", "rev": "v0.2.1", "variant": "production",
     "label": "v0.2.1 — Production", "usb_product": "G6 Panel v0.2",
     "slug": "g6-panel-v0.2.1"},
    # BCM self-test builds (bench / bring-up): auto-run a visible 60 s pattern
    # cycle + a serial console; NO SPI ingest — re-flash a production build
    # before deploying. _spidiag / _psramtest stay bench-only and are
    # intentionally not part of the catalog.
    {"env": "pico_v031_bcmtest", "task": "build31-bcmtest", "rev": "v0.3.1", "variant": "bcmtest",
     "label": "v0.3.1 — BCM self-test", "usb_product": "G6 Panel v0.3",
     "slug": "g6-panel-v0.3.1-bcmtest"},
    {"env": "pico_v021_bcmtest", "task": "build21-bcmtest", "rev": "v0.2.1", "variant": "bcmtest",
     "label": "v0.2.1 — BCM self-test", "usb_product": "G6 Panel v0.2",
     "slug": "g6-panel-v0.2.1-bcmtest"},
]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def build_leg(entry: dict, out: Path) -> None:
    env = entry["env"]
    print(f"build-release: building {env} (pixi run {entry['task']}) …")
    subprocess.run(["pixi", "run", entry["task"]], check=True)

    # The panel build also runs tools/gen_predef_patterns.py (extra_scripts)
    # and enforces the 2 MiB cap, so an oversize/broken image fails here.
    uf2_src = Path("panel/.pio/build") / env / "firmware.uf2"
    if not uf2_src.is_file():
        sys.exit(f"build-release: {uf2_src} not found after building {env}")

    uf2_dest = out / f"{entry['slug']}.uf2"
    shutil.copyfile(uf2_src, uf2_dest)
    uf2_digest = sha256(uf2_dest)
    print(f"build-release: staged {uf2_dest.name} ({uf2_digest[:12]}…)")

    # Reuses the firmware.uf2 just built above — no second build. Same
    # artifact entry as the uf2 (not a separate one) — g6-flash and the
    # WebUSB flasher only ever look at `uf2`; Arena Studio's ISP push only
    # ever looks at `bin`.
    bin_dest = out / f"{entry['slug']}.bin"
    subprocess.run(
        [sys.executable, str(MAKE_ISP_IMAGE), "--env", env, "--out", str(bin_dest)],
        check=True,
    )
    bin_digest = sha256(bin_dest)
    print(f"build-release: staged {bin_dest.name} ({bin_digest[:12]}…)")

    artifact = {
        "rev": entry["rev"], "variant": entry["variant"], "env": env,
        "label": entry["label"], "usb_product": entry["usb_product"],
        "uf2": {"file": uf2_dest.name, "sha256": uf2_digest},
        "bin": {"file": bin_dest.name, "sha256": bin_digest},
    }
    (out / f"artifact-{entry['slug']}.json").write_text(json.dumps(artifact, indent=2))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Build the G6 panel firmware release catalog.")
    ap.add_argument("--only", metavar="SLUG",
                     help="build only this catalog entry, by slug (fast local "
                          "iteration); default: build the whole catalog")
    ap.add_argument("--out", default="dist", metavar="DIR",
                     help="output directory (default: dist)")
    args = ap.parse_args(argv)

    entries = CATALOG
    if args.only:
        entries = [e for e in CATALOG if e["slug"] == args.only]
        if not entries:
            sys.exit(f"build-release: no catalog entry '{args.only}'. "
                      f"Available: {', '.join(e['slug'] for e in CATALOG)}")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for entry in entries:
        build_leg(entry, out)

    # manifest.json is assembled from whatever artifact-*.json is present in
    # `out` — so `--only` on top of an existing dist/ regenerates just one
    # leg and still produces a manifest covering everything staged so far.
    env = os.environ.copy()
    env.setdefault("BUILD_DATE", datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%MZ"))
    subprocess.run(
        [sys.executable, str(MAKE_MANIFEST), str(out), str(out / "manifest.json")],
        check=True, env=env,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
