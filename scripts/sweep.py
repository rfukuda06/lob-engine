#!/usr/bin/env python3
"""Sweep the market-making lab across informed-flow fractions, averaged over seeds.

For each informed-flow fraction it runs the sim over K seeds (parsing the
machine-readable `--json` summary), then reports each metric as mean +/-
standard error across seeds. This turns a single-path number into a result
with a confidence band, and regenerates the README's adverse-selection table.

Usage:
    python3 scripts/sweep.py \
        --binary ./build/orderbook --policy inventory --steps 20000 \
        --informed 0 15 30 --seeds 8 --out docs/sweep_results.csv

Requires only the Python standard library (no numpy/pandas).
"""
import argparse
import csv
import json
import math
import statistics
import subprocess
import sys


def run_one(binary: str, steps: int, seed: int, policy: str, informed: int) -> dict:
    """Run the sim once and return its parsed JSON summary."""
    cmd = [binary, "--mm-sim", str(steps), "--seed", str(seed),
           "--policy", policy, "--informed-frac", str(informed), "--json"]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"binary failed ({proc.returncode}) for seed={seed} "
                         f"informed={informed}:\n{proc.stderr or proc.stdout}")
    try:
        return json.loads(proc.stdout.strip())
    except json.JSONDecodeError as e:
        raise SystemExit(f"could not parse --json output: {e}\ngot: {proc.stdout!r}")


def mean_se(values: list) -> tuple:
    """Mean and standard error (sample std / sqrt n); se is 0 for n < 2."""
    m = statistics.fmean(values)
    se = statistics.stdev(values) / math.sqrt(len(values)) if len(values) > 1 else 0.0
    return m, se


def main() -> int:
    p = argparse.ArgumentParser(description="Multi-seed informed-flow sweep.")
    p.add_argument("--binary", default="./build/orderbook")
    p.add_argument("--policy", default="inventory", choices=["inventory", "as"])
    p.add_argument("--steps", type=int, default=20000)
    p.add_argument("--informed", type=int, nargs="+", default=[0, 15, 30],
                   help="informed-flow percentages to sweep")
    p.add_argument("--seeds", type=int, default=8, help="number of seeds per point")
    p.add_argument("--seed-base", type=int, default=1, help="first seed (uses base..base+seeds-1)")
    p.add_argument("--out", default="docs/sweep_results.csv", help="tidy results CSV")
    args = p.parse_args()

    seeds = list(range(args.seed_base, args.seed_base + args.seeds))
    rows = []          # one aggregated row per informed fraction
    metric_keys = None

    for informed in args.informed:
        runs = [run_one(args.binary, args.steps, s, args.policy, informed) for s in seeds]
        if metric_keys is None:
            metric_keys = list(runs[0].keys())
        agg = {"informed_pct": informed, "n_seeds": len(seeds)}
        for k in metric_keys:
            m, se = mean_se([r[k] for r in runs])
            agg[f"{k}_mean"] = m
            agg[f"{k}_se"] = se
        rows.append(agg)
        print(f"informed={informed:>3}%  fills={agg['fills_mean']:.0f}  "
              f"pnl_fair={agg['final_pnl_fair_mean']:+.0f}  "
              f"adv_sel={agg['adverse_selection_mean']:.2f}", file=sys.stderr)

    # Tidy CSV with mean+se for every metric.
    fieldnames = ["informed_pct", "n_seeds"]
    for k in metric_keys:
        fieldnames += [f"{k}_mean", f"{k}_se"]
    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {args.out}", file=sys.stderr)

    # Markdown table matching the README's "adverse-selection gradient".
    policy_name = "Avellaneda–Stoikov" if args.policy == "as" else "inventory-skew"
    print(f"\n<!-- {policy_name} maker, {args.steps} steps, "
          f"{len(seeds)} seeds (mean ± standard error) -->")
    print("| Informed flow | MM fills | Final PnL vs. fair (ticks·shares) | "
          "Adverse selection (ticks) | Max \\|inventory\\| |")
    print("|---|---|---|---|---|")
    for r in rows:
        print(f"| {r['informed_pct']}% "
              f"| {r['fills_mean']:.0f} ± {r['fills_se']:.0f} "
              f"| {r['final_pnl_fair_mean']:+.0f} ± {r['final_pnl_fair_se']:.0f} "
              f"| {r['adverse_selection_mean']:.1f} ± {r['adverse_selection_se']:.1f} "
              f"| {r['max_abs_inventory_mean']:.0f} ± {r['max_abs_inventory_se']:.0f} |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
