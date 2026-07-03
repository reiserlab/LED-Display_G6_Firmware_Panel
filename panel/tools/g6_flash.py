#!/usr/bin/env python3
"""g6-flash — streamlined flashing for G6 LED-display panels (RP2350 / RP2354).

One tool to program new (blank) panels and re-flash old ones, a single panel or a
bench full of them, without a PlatformIO build environment. Built on `picotool`.

Why this exists
---------------
The old bench scripts `deploy.sh` / `deploy_all.sh` (retired — their functionality
now lives here, see the `deploy*` pixi tasks) drove `pio ... -t upload` and could
only see panels that were *already running* firmware (USB-serial mode), so they
couldn't touch a brand-new/blank board, and `deploy_all.sh` flashed sequentially.
`picotool` closes both gaps: it can reboot a running panel into BOOTSEL itself
(`reboot -f -u`) and flash a board that is already in BOOTSEL — so the same code
path handles new and old panels — and we flash many in parallel.

What it does NOT do
-------------------
Panels are stateless: every panel of a given hardware revision gets the IDENTICAL
binary (no per-panel ID/address is burned in — addressing is the arena
controller's job at runtime). So there is no provisioning step. The ONE thing the
operator must get right is the hardware revision, because the two revs need
different binaries and the rev CANNOT be detected over USB on a blank board.
`--rev` is therefore mandatory, and every flash is verified afterwards by reading
back the panel's USB product string.

Platform: Linux (enumerates via sysfs, like the existing by-id scripts). Requires
`picotool`: this tool prefers the copy PlatformIO already vendors under
`~/.platformio/packages/tool-picotool*/` (present for anyone who ran `pixi run
release`/`diag`, which build via `pio` under the hood), falling back to PATH
(e.g. from an activated virtual environment, or a system install).

Usage
-----
    # Re-flash every connected v0.3.1 panel from the latest published firmware:
    g6_flash.py --rev v0.3.1

    # Flash one specific board (physical USB port) with a locally built UF2:
    g6_flash.py --rev v0.2.1 --uf2 panel/.pio/build/pico_v021/firmware.uf2 --port 3-1.4

    # Flash one specific bench board by USB serial number (survives port/hub moves):
    g6_flash.py --rev v0.2.1 --uf2 panel/.pio/build/pico_v021/firmware.uf2 --serial 319A5199EE357F77

    # See what would happen without touching anything:
    g6_flash.py --rev v0.3.1 --dry-run

See `docs/development/g6_07-panel-programming.md` (Modular-LED-Display repo) and the
parent README's "Flash & monitor" section.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

# --- Constants tied to the firmware (keep in sync with panel/platformio.ini) ----

RP_VID = "2e8a"  # Raspberry Pi USB vendor id (board_build...usb_vid = 0x2E8A)
PID_APP = "0009"  # running panel firmware: USB-serial device
PID_BOOTSEL = "000f"  # RP2350 bootrom: USB mass-storage / PICOBOOT

# rev -> (PlatformIO env, USB product string prefix). Product strings are
# "G6 Panel v0.2" / "G6 Panel v0.3" (major.minor only) — match by prefix, the
# same way deploy.sh maps env -> product.
REVS = {
    "v0.2.1": {"env": "pico_v021", "usb_product": "G6 Panel v0.2"},
    "v0.3.1": {"env": "pico_v031", "usb_product": "G6 Panel v0.3"},
}

# Default firmware source (GitHub Releases of the panel firmware repo).
FW_REPO = "reiserlab/LED-Display_G6_Firmware_Panel"
CACHE_DIR = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "g6-flash"

REENUMERATE_TIMEOUT_S = 25.0
REENUMERATE_POLL_S = 0.25


# --- Device enumeration (Linux sysfs) -------------------------------------------


@dataclass
class Panel:
    """A connected RP2350 board, keyed by its stable physical USB port path.

    The port path (sysfs dir name, e.g. "3-1.4") survives the BOOTSEL<->app
    re-enumeration, so we use it as the device identity. bus/address (USB
    busnum/devnum) are what picotool targets and change on re-enumeration.
    """

    port: str  # sysfs port path, e.g. "3-1.4" — stable across re-enumeration
    bus: int  # USB bus number (picotool --bus)
    address: int  # USB device address (picotool --address)
    pid: str  # "0009" (app) or "000f" (bootsel)
    serial: str | None = None
    product: str | None = None

    @property
    def in_bootsel(self) -> bool:
        return self.pid == PID_BOOTSEL

    @property
    def label(self) -> str:
        what = self.product or ("BOOTSEL" if self.in_bootsel else "?")
        return f"port {self.port} (bus {self.bus} addr {self.address}, {what})"


def _read(path: Path) -> str | None:
    try:
        return path.read_text().strip()
    except OSError:
        return None


def enumerate_panels() -> list[Panel]:
    """Find all connected RP2350 boards (running firmware OR in BOOTSEL)."""
    root = Path("/sys/bus/usb/devices")
    if not root.is_dir():
        sys.exit("g6-flash: /sys/bus/usb/devices not found — this tool is Linux-only.")

    panels: list[Panel] = []
    for dev in sorted(root.iterdir()):
        if _read(dev / "idVendor") != RP_VID:
            continue
        pid = _read(dev / "idProduct")
        if pid not in (PID_APP, PID_BOOTSEL):
            continue
        busnum = _read(dev / "busnum")
        devnum = _read(dev / "devnum")
        if not (busnum and devnum):
            continue
        panels.append(
            Panel(
                port=dev.name,
                bus=int(busnum),
                address=int(devnum),
                pid=pid,
                serial=_read(dev / "serial"),
                product=_read(dev / "product"),
            )
        )
    return panels


def wait_for_port(port: str, want_pid: str | None, timeout: float) -> Panel | None:
    """Poll sysfs until the panel on `port` reappears (optionally in want_pid)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for p in enumerate_panels():
            if p.port == port and (want_pid is None or p.pid == want_pid):
                return p
        time.sleep(REENUMERATE_POLL_S)
    return None


# --- Firmware artifact resolution -----------------------------------------------


def _http_json(url: str) -> dict:
    req = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json"})
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def _download(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(url, headers={"Accept": "application/octet-stream"})
    with urllib.request.urlopen(req, timeout=120) as r, open(dest, "wb") as f:
        shutil.copyfileobj(r, f)


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def resolve_uf2(rev: str, fw_version: str | None, local_uf2: str | None,
                variant: str = "production") -> Path:
    """Return a path to the UF2 to flash for `rev`/`variant` — local or cached release.

    Release layout (produced by release.yml): each release carries one UF2 per
    selectable build plus a manifest.json catalog whose entries describe
    rev/variant/usb_product plus an optional nested `uf2: {file, sha256}` (and
    `bin: {file, sha256}` for the ISP image, which we never look at here). We
    pick the entry matching `rev` AND `variant` (default 'production') that
    HAS a `uf2` build, download into the per-version cache, and verify its
    sha256 against the manifest.
    """
    if local_uf2:
        p = Path(local_uf2)
        if not p.is_file():
            sys.exit(f"g6-flash: --uf2 not found: {p}")
        return p

    base = f"https://api.github.com/repos/{FW_REPO}/releases"
    rel = _http_json(f"{base}/tags/{fw_version}" if fw_version else f"{base}/latest")
    tag = rel["tag_name"]
    assets = {a["name"]: a["browser_download_url"] for a in rel.get("assets", [])}

    if "manifest.json" not in assets:
        sys.exit(f"g6-flash: release {tag} has no manifest.json asset.")
    cache = CACHE_DIR / tag
    manifest_path = cache / "manifest.json"
    if not manifest_path.is_file():
        _download(assets["manifest.json"], manifest_path)
    manifest = json.loads(manifest_path.read_text())

    # Entries predating the variant field are treated as 'production'. A
    # "bin"-only entry (no "uf2" key) is an ISP image for the arena
    # controller, never something g6-flash can flash over USB — excluded.
    artifacts = [a for a in manifest.get("artifacts", []) if a.get("uf2")]
    entry = next((a for a in artifacts
                  if a.get("rev") == rev and a.get("variant", "production") == variant), None)
    if not entry:
        avail = sorted({f"{a.get('rev')}/{a.get('variant', 'production')}" for a in artifacts})
        sys.exit(f"g6-flash: release {tag} has no {rev}/{variant} build. "
                 f"Available: {', '.join(avail) or 'none'}")

    fname = entry["uf2"]["file"]
    if fname not in assets:
        sys.exit(f"g6-flash: release {tag} is missing UF2 asset '{fname}'.")
    uf2 = cache / fname
    if not uf2.is_file():
        print(f"g6-flash: downloading {fname} from firmware release {tag} …")
        _download(assets[fname], uf2)

    expect = entry["uf2"].get("sha256")
    if expect:
        got = _sha256(uf2)
        if got != expect:
            uf2.unlink(missing_ok=True)
            sys.exit(f"g6-flash: sha256 mismatch for {fname}\n  expected {expect}\n  got      {got}")
    print(f"g6-flash: firmware {tag} {rev}/{variant} -> {uf2}")
    return uf2


# --- picotool wrappers ----------------------------------------------------------


def find_picotool() -> str | None:
    """Locate the picotool binary: PlatformIO's vendored copy first, else PATH.

    picotool isn't a conda-forge package, so `pixi run` doesn't install it — but
    PlatformIO already vendors a copy for anyone who has built with `pio`/`pixi run
    release`/`diag`. Prefer that known-good copy over whatever a system
    install on PATH happens to be, falling back to PATH if PlatformIO hasn't
    fetched one.
    """
    for candidate in sorted(Path.home().glob(".platformio/packages/tool-picotool*/picotool")):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    return shutil.which("picotool")


def picotool_missing_message() -> str:
    """Error text for main() when neither the PlatformIO cache nor PATH has picotool.

    Steers towards pixi (this project's toolchain manager) first — `pixi run
    release`/`diag` fetches PlatformIO's picotool as a side effect (they
    build via `pio` under the hood), which is also how find_picotool()'s fallback
    gets populated. OS package manager is offered as a fallback for people
    without pixi; conda-forge is deliberately not suggested (picotool isn't a
    conda-forge package).
    """
    where = "in PlatformIO's package cache (~/.platformio/packages/tool-picotool*/) or on PATH"
    if shutil.which("pixi"):
        primary = ("Run `pixi run release` once so PlatformIO fetches its own "
                   "copy, then retry `pixi run flash21`/`flash31`.")
    else:
        primary = ("Install pixi (https://pixi.sh) — it manages this project's "
                   "toolchain, and `pixi run release` will fetch picotool for "
                   "you; then use `pixi run flash21`/`flash31`.")
    return (f"g6-flash: 'picotool' not found {where}. {primary}\n"
            "  Fallback without pixi: install picotool via your OS's package manager "
            "(install steps differ between Windows/macOS/Linux).")


_PICOTOOL_PATH: str | None = None


def _picotool(*args: str) -> subprocess.CompletedProcess:
    global _PICOTOOL_PATH
    if _PICOTOOL_PATH is None:
        _PICOTOOL_PATH = find_picotool() or "picotool"
    return subprocess.run(
        [_PICOTOOL_PATH, *args], capture_output=True, text=True, check=False
    )


def reboot_to_bootsel(panel: Panel) -> None:
    """Force a running panel into BOOTSEL (no button press)."""
    cp = _picotool("reboot", "-f", "-u", "--bus", str(panel.bus), "--address", str(panel.address))
    # picotool returns nonzero if the device already vanished into BOOTSEL — tolerate it.
    if cp.returncode != 0 and "not in BOOTSEL" not in (cp.stderr + cp.stdout):
        # Best-effort: re-enumeration check below is the real gate.
        pass


def load_uf2(panel: Panel, uf2: Path, execute: bool) -> subprocess.CompletedProcess:
    args = ["load", "--bus", str(panel.bus), "--address", str(panel.address)]
    if execute:
        args.append("-x")
    args.append(str(uf2))
    return _picotool(*args)


# --- Per-panel flash flow -------------------------------------------------------


@dataclass
class Result:
    panel: Panel
    ok: bool
    detail: str
    verified_product: str | None = None


def flash_one(panel: Panel, rev: str, uf2: Path, execute: bool, dry_run: bool) -> Result:
    want_product = REVS[rev]["usb_product"]

    if dry_run:
        action = "reboot->BOOTSEL then load" if not panel.in_bootsel else "load"
        return Result(panel, True, f"DRY-RUN: would {action} {uf2.name}")

    # 1) Get the board into BOOTSEL.
    target = panel
    if not panel.in_bootsel:
        reboot_to_bootsel(panel)
        target = wait_for_port(panel.port, PID_BOOTSEL, REENUMERATE_TIMEOUT_S)
        if target is None:
            return Result(panel, False, "timed out waiting for BOOTSEL after reboot")

    # 2) Flash.
    cp = load_uf2(target, uf2, execute)
    if cp.returncode != 0:
        return Result(panel, False, f"picotool load failed: {(cp.stderr or cp.stdout).strip()[:200]}")

    if not execute:
        return Result(panel, True, "flashed (not executed; power-cycle to run + verify)")

    # 3) Verify: panel re-enumerates as app-mode with the expected product string.
    booted = wait_for_port(panel.port, PID_APP, REENUMERATE_TIMEOUT_S)
    if booted is None:
        return Result(panel, False, "flashed but panel did not re-enumerate as firmware")
    if not (booted.product or "").startswith(want_product):
        return Result(
            panel, False,
            f"WRONG REV? flashed {rev} but panel reports '{booted.product}' (expected '{want_product}*')",
            booted.product,
        )
    return Result(panel, True, "flashed + verified", booted.product)


# --- CLI ------------------------------------------------------------------------


def select_targets(panels: list[Panel], rev: str, port: str | None, serial: str | None,
                   force: bool) -> list[Panel]:
    if port:
        chosen = [p for p in panels if p.port == port]
        if not chosen:
            sys.exit(f"g6-flash: no RP2350 board on port {port}. Connected: "
                     + (", ".join(p.port for p in panels) or "none"))
        return chosen

    if serial:
        # Only a board already running firmware exposes its serial over sysfs
        # (a blank/BOOTSEL board doesn't) — same limitation the old deploy.sh
        # had; use --port for a blank board instead.
        chosen = [p for p in panels if p.serial == serial]
        if not chosen:
            sys.exit(f"g6-flash: no RP2350 board with serial {serial}. Connected serials: "
                     + (", ".join(p.serial for p in panels if p.serial) or "none"))
        return chosen

    if not panels:
        sys.exit("g6-flash: no RP2350 panels found (none in firmware or BOOTSEL mode).")

    # Loud warning if a *running* board reports a different rev than --rev. We
    # cannot block (a blank/BOOTSEL board reports nothing, and a genuine cross-rev
    # re-flash is legitimate), but a silent wrong-rev batch is the headline footgun.
    want_product = REVS[rev]["usb_product"]
    mismatched = [p for p in panels if p.product and not p.product.startswith(want_product)]
    if mismatched and not force:
        print(f"\n  ⚠  {len(mismatched)} connected panel(s) currently report a DIFFERENT rev than --rev {rev}:")
        for p in mismatched:
            print(f"       {p.label}")
        print(f"     Expected product prefix '{want_product}'. If these really are {rev}")
        print(f"     hardware (e.g. blank/mis-flashed), re-run with --force. Otherwise fix --rev.\n")
        sys.exit("g6-flash: aborting on rev mismatch (use --force to override).")
    return panels


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="g6-flash",
        description="Flash G6 LED-display panels (RP2350) — new or existing, one or many.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Realistic batch ceiling: ~10–20 panels per EXTERNALLY-POWERED hub. The\n"
               "limit is post-flash LED-matrix inrush + USB re-enumeration, not flashing.\n"
               "For larger trays use --no-exec, then power-cycle in small groups.",
    )
    ap.add_argument("--rev", required=True, choices=sorted(REVS),
                    help="panel hardware revision (MANDATORY — selects the binary; "
                         "cannot be auto-detected on a blank board)")
    ap.add_argument("--variant", default="production", metavar="NAME",
                    help="firmware build variant to flash (default: production; "
                         "e.g. 'bcmtest' for the BCM self-test build)")
    ap.add_argument("--uf2", metavar="PATH",
                    help="flash a local UF2 instead of a published release (for firmware devs)")
    ap.add_argument("--fw-version", metavar="TAG",
                    help="firmware release tag to flash (default: latest)")
    target = ap.add_mutually_exclusive_group()
    target.add_argument("--port", metavar="PORT",
                    help="flash only the board on this sysfs USB port (e.g. 3-1.4); "
                         "default: all connected panels")
    target.add_argument("--serial", metavar="SERIAL",
                    help="flash only the board with this USB serial number — stable "
                         "across which port/hub it's plugged into, but only visible on "
                         "a board already running firmware (a blank/BOOTSEL board "
                         "exposes no serial; use --port for that case)")
    ap.add_argument("--jobs", type=int, default=4, metavar="N",
                    help="max panels flashed in parallel (default: 4)")
    ap.add_argument("--no-exec", action="store_true",
                    help="load firmware but do NOT execute it (reduces simultaneous "
                         "power-on inrush; power-cycle later to run + verify)")
    ap.add_argument("--force", action="store_true",
                    help="suppress the running-panel rev-mismatch guard")
    ap.add_argument("--dry-run", action="store_true",
                    help="show what would be flashed without touching any board")
    ap.add_argument("--list", action="store_true",
                    help="list connected panels and exit")
    args = ap.parse_args(argv)

    if not args.dry_run and not find_picotool():
        sys.exit(picotool_missing_message())

    panels = enumerate_panels()

    if args.list:
        if not panels:
            print("No RP2350 panels connected.")
        for p in panels:
            print(f"  {p.label}  serial={p.serial}")
        return 0

    targets = select_targets(panels, args.rev, args.port, args.serial, args.force)
    uf2 = resolve_uf2(args.rev, args.fw_version, args.uf2, args.variant)
    execute = not args.no_exec

    print(f"\ng6-flash: flashing {len(targets)} panel(s) as {args.rev} "
          f"({'parallel x%d' % args.jobs if len(targets) > 1 else 'single'})\n")

    results: list[Result] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futs = {pool.submit(flash_one, p, args.rev, uf2, execute, args.dry_run): p for p in targets}
        for fut in concurrent.futures.as_completed(futs):
            r = fut.result()
            mark = "✓" if r.ok else "✗"
            print(f"  {mark} {r.panel.label}: {r.detail}")
            results.append(r)

    ok = sum(1 for r in results if r.ok)
    failed = len(results) - ok
    print(f"\ng6-flash: {ok}/{len(results)} panel(s) OK"
          + (f", {failed} FAILED" if failed else ""))
    if failed:
        for r in results:
            if not r.ok:
                print(f"    FAILED  {r.panel.label}: {r.detail}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
