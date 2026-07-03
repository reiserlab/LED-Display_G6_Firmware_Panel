#!/usr/bin/env python3
"""build_release — build the G6 panel firmware release/diag catalog into dist/.

This is what release.yml's build job runs (`pixi run release`), so a bench
developer can preview the exact release payload (UF2s + BINs + manifest.json)
before ever pushing a `panel-fw-v*` tag — the CI workflow and the local task
are the same command, not two independently-maintained build paths.

The catalog is DISCOVERED from panel/platformio.ini, not hardcoded here — see
discover_catalog(). Run from the repo root (same convention as g6_flash.py).
Delegates each leg's build to `pixi run pio run -d panel -e <env>` instead of
invoking `pio` itself, so the pixi-environment activation is defined in
exactly one place. Needs `pixi` on PATH — trivially true here since `pixi
run release`/`diag` is what launches this script.

Usage
-----
    pixi run release                                              # release catalog, both formats
    pixi run diag                                                 # diag catalog, both formats
    python panel/tools/build_release.py --only g6-panel-v0.3.1    # one leg, any group
    python panel/tools/build_release.py --list                    # show the discovered catalog

Every build always gets wrapped into an ISP-footer `.bin` (via
make_isp_image.py) for the arena controller's over-SPI panel reflashing, in
addition to its `.uf2`. Both formats live on the SAME manifest.json artifact
entry, as nested `"uf2": {"file", "sha256"}` (for g6-flash / the WebUSB
flasher) and `"bin": {"file", "sha256"}` (for Arena Studio's / the arena
controller's over-SPI push) — see make_manifest.py. `release` and
`diag` both stage into the same dist/, so running both leaves one
manifest.json covering everything built so far; CI only ever runs `release`.

See docs/development/g6_07-panel-programming.md §A.
"""

from __future__ import annotations

import argparse
import configparser
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

MAKE_MANIFEST = Path(".github/scripts/make_manifest.py")
MAKE_ISP_IMAGE = Path("panel/tools/make_isp_image.py")
PLATFORMIO_INI = Path("panel/platformio.ini")

ENV_RE = re.compile(r"^pico_v(\d)(\d)(\d)(?:_(\w+))?$")

# Cosmetic only — the display label for a known variant. Anything not listed
# here still works (falls back to a title-cased version of the variant name);
# this is NOT what decides catalog membership, so a new PlatformIO env never
# needs an entry here to be discovered correctly.
LABELS = {"bcmtest": "BCM self-test", "spidiag": "SPI diagnostics"}


def discover_catalog() -> list[dict]:
    """Discover the release/diag catalogs from panel/platformio.ini.

    An env belongs to the catalog if its name matches `pico_v<3 digits>
    [_variant]` and it `extends` either:
      - `common`            -> group "release": a hardware-rev production
                               build (rev taken from the env's own digits).
      - another `pico_v*` env -> group "diag": a variant build of
                               that rev (bcmtest, spidiag, or any future
                               one — rev taken from the EXTENDED env, not
                               re-parsed from this env's own name).
    This is the single extension point: add a PlatformIO env with the right
    `extends`, and it's picked up automatically here — no separate catalog
    list to keep in sync with platformio.ini.
    """
    cfg = configparser.ConfigParser(interpolation=None)
    if not cfg.read(PLATFORMIO_INI):
        sys.exit(f"build-release: could not read {PLATFORMIO_INI}")

    catalog = []
    for section in cfg.sections():
        if not section.startswith("env:pico_"):
            continue
        env = section[len("env:"):]
        m = ENV_RE.match(env)
        if not m:
            print(f"build-release: skipping [{section}] — doesn't match the "
                  "pico_v<rev>[_variant] naming convention", file=sys.stderr)
            continue
        d1, d2, d3, own_variant = m.groups()
        extends = cfg.get(section, "extends", fallback="").strip()

        if extends == "common":
            group, rev, variant = "release", f"v{d1}.{d2}.{d3}", "production"
        elif extends.startswith("env:pico_v"):
            base_m = ENV_RE.match(extends.split(":", 1)[1])
            if not base_m:
                print(f"build-release: skipping [{section}] — extends "
                      f"'{extends}', which doesn't match the naming convention",
                      file=sys.stderr)
                continue
            bd1, bd2, bd3, _ = base_m.groups()
            group, rev, variant = "diag", f"v{bd1}.{bd2}.{bd3}", own_variant or "unknown"
        else:
            print(f"build-release: skipping [{section}] — extends '{extends}', "
                  "neither 'common' nor another pico_v* env", file=sys.stderr)
            continue

        usb_product = f"G6 Panel {rev[:-2]}"  # "v0.2.1" -> "G6 Panel v0.2"
        label_text = LABELS.get(variant, variant.replace("_", " ").title())
        slug_variant = variant.replace("_", "-")
        slug = f"g6-panel-{rev}" if variant == "production" else f"g6-panel-{rev}-{slug_variant}"

        catalog.append({
            "env": env, "group": group, "rev": rev, "variant": variant,
            "label": f"{rev} — {label_text}", "usb_product": usb_product, "slug": slug,
        })
    return catalog


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def build_leg(entry: dict, out: Path) -> None:
    env = entry["env"]
    print(f"build-release: building {env} …")
    subprocess.run(["pixi", "run", "pio", "run", "-d", "panel", "-e", env], check=True)

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
    ap = argparse.ArgumentParser(
        description="Build the G6 panel firmware release/diag catalog.")
    ap.add_argument("--group", choices=["release", "diag"], default="release",
                     help="which catalog to build (default: release); ignored if --only is given")
    ap.add_argument("--only", metavar="SLUG",
                     help="build only this catalog entry, by slug, from either group "
                          "(fast local iteration)")
    ap.add_argument("--out", default="dist", metavar="DIR",
                     help="output directory (default: dist)")
    ap.add_argument("--list", action="store_true",
                     help="list the discovered catalog (env, group, slug) and exit")
    args = ap.parse_args(argv)

    catalog = discover_catalog()
    if not catalog:
        sys.exit(f"build-release: no envs discovered from {PLATFORMIO_INI}.")

    if args.list:
        for e in catalog:
            print(f"  {e['slug']:<28} group={e['group']:<12} env={e['env']:<20} {e['label']}")
        return 0

    if args.only:
        entries = [e for e in catalog if e["slug"] == args.only]
        if not entries:
            sys.exit(f"build-release: no catalog entry '{args.only}'. "
                      f"Available: {', '.join(e['slug'] for e in catalog)}")
    else:
        entries = [e for e in catalog if e["group"] == args.group]
        if not entries:
            sys.exit(f"build-release: no catalog entries in group '{args.group}'.")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for entry in entries:
        build_leg(entry, out)

    # manifest.json is assembled from whatever artifact-*.json is present in
    # `out` — so `--only`, or running `release` and `diag` into the
    # same dist/, accumulates into one manifest covering everything staged
    # so far (CI only ever runs `release`, so the published manifest only
    # ever sees release-group artifacts).
    env = os.environ.copy()
    env.setdefault("BUILD_DATE", datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%MZ"))
    subprocess.run(
        [sys.executable, str(MAKE_MANIFEST), str(out), str(out / "manifest.json")],
        check=True, env=env,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
