#!/usr/bin/env python3
"""pulse_sweep_plot — quick look at a pulse_sweep <out>_summary.csv.

    pulse_sweep_plot.py phase2_summary.csv                  # tables, plus a figure if matplotlib is installed
    pulse_sweep_plot.py phase1_summary.csv --metric as_F6_640
    pulse_sweep_plot.py run_summary.csv --save fig.png --no-show

For each condition, <metric>_mean minus the dark reading is divided by the
nearest reference condition (same channel, closest in sequence, also dark-
subtracted), so slow drift and background cancel. Prints brightness vs pulse
width for groups sharing (channel, pct, on-fraction) — the constant-duty
question — and brightness vs pct for groups sharing (channel, b, r) — the
linearity question, where norm/(pct/100) = 1 means perfectly linear.
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import defaultdict


def load(path: str, metric: str) -> tuple[list[dict], float, dict]:
    with open(path, newline="") as f:
        rows = [r for r in csv.DictReader(f) if r.get(f"{metric}_mean")]
    if not rows:
        sys.exit(f"no rows with {metric}_mean in {path}")
    for r in rows:
        r["idx"] = int(r["cond_idx"])
        r["val"] = float(r[f"{metric}_mean"])
        r["cv"] = float(r[f"{metric}_cv"] or 0)
        r["b"], r["r"], r["p"] = float(r["b_us"]), int(r["r_us"]), int(r["pct"])
    darks = [r["val"] for r in rows if r["tag"] == "dark"]
    dark = darks[0] if darks else 0.0
    refs = defaultdict(list)
    for r in rows:
        if r["is_reference"] == "1":
            refs[r["channel"]].append(r)
    return rows, dark, refs


def normalise(rows: list[dict], dark: float, refs: dict) -> list[dict]:
    out = []
    for r in rows:
        if r["tag"] != "cond":
            continue
        pool = refs.get(r["channel"]) or [x for v in refs.values() for x in v]
        if pool:
            ref = min(pool, key=lambda x: abs(x["idx"] - r["idx"]))
            denom = ref["val"] - dark
            r["norm"] = (r["val"] - dark) / denom if denom else float("nan")
        else:
            r["norm"] = float("nan")
        out.append(r)
    return out


def groups(conds: list[dict], key, vary) -> dict:
    g = defaultdict(list)
    for r in conds:
        g[key(r)].append(r)
    return {k: sorted(v, key=vary) for k, v in g.items() if len({vary(r) for r in v}) > 1}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("summary_csv")
    ap.add_argument("--metric", default="tsl_full", help="summary channel to analyse (default tsl_full)")
    ap.add_argument("--save", metavar="PNG", help="write the figure to a file")
    ap.add_argument("--no-show", action="store_true", help="do not open a window")
    args = ap.parse_args(argv)

    rows, dark, refs = load(args.summary_csv, args.metric)
    conds = normalise(rows, dark, refs)
    print(f"{args.metric}: dark={dark:.1f}, {sum(len(v) for v in refs.values())} reference runs, {len(conds)} conditions")

    const = groups(conds, lambda r: (r["channel"], r["p"], round(float(r["on_fraction"]), 4)), lambda r: r["b"])
    for (ch, pct, frac), g in sorted(const.items()):
        print(f"\nconstant on-fraction {frac * 100:.2f}%  ch={ch} pct={pct}   (brightness vs pulse width)")
        print(f"  {'b_us':>6s} {'r_us':>6s} {'on_us':>8s} {'mean':>10s} {'norm':>7s} {'cv%':>6s}  overrun")
        for r in g:
            print(f"  {r['b']:6g} {r['r']:6d} {float(r['on_us']):8.2f} {r['val']:10.1f} {r['norm']:7.3f} {r['cv']:6.2f}  "
                  f"{'yes' if r['scan_overrun_est'] == '1' else ''}")

    lin = groups(conds, lambda r: (r["channel"], r["b"], r["r"]), lambda r: r["p"])
    for (ch, b, rr), g in sorted(lin.items()):
        print(f"\nlinearity  ch={ch} b={b:g} r={rr}   (norm/(pct/100) = 1 is linear)")
        print(f"  {'pct':>4s} {'mean':>10s} {'norm':>7s} {'lin':>6s} {'cv%':>6s}")
        for r in g:
            lin_ratio = r["norm"] / (r["p"] / 100) if r["p"] else float("nan")
            print(f"  {r['p']:4d} {r['val']:10.1f} {r['norm']:7.3f} {lin_ratio:6.3f} {r['cv']:6.2f}")

    if not const and not lin:
        print("\nno groups with more than one pulse width or more than one pct")
        return 0
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("\n(matplotlib not installed — tables only)")
        return 0
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2))
    ax = axes[0]
    for (ch, pct, frac), g in sorted(const.items()):
        ax.plot([float(r["on_us"]) for r in g], [r["norm"] for r in g], "o-", label=f"ch={ch} pct={pct} {frac * 100:.1f}%")
    ax.set_xscale("log")
    ax.set_xlabel("LED pulse width per row (us)")
    ax.set_ylabel(f"{args.metric} / reference")
    ax.set_title("constant on-fraction")
    ax.grid(True, which="both", alpha=0.3)
    if const:
        ax.legend(fontsize=8)
    ax = axes[1]
    for (ch, b, rr), g in sorted(lin.items()):
        ax.plot([r["p"] for r in g], [r["norm"] for r in g], "o-", label=f"ch={ch} b={b:g} r={rr}")
    ax.plot([0, 100], [0, 1], "k--", lw=0.8, label="linear")
    ax.set_xlabel("pct")
    ax.set_ylabel(f"{args.metric} / reference")
    ax.set_title("linearity")
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    if args.save:
        fig.savefig(args.save, dpi=130)
        print(f"\nsaved {args.save}")
    if not args.no_show:
        plt.show()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
