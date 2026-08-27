#!/usr/bin/env python3
"""Render the market-making lab's per-step CSV into an overview figure.

The CSV is produced by:  orderbook --mm-sim N ... --out mm.csv
It has one row per step with columns:
    t, fair, bid, ask, mid, microprice, bid_size, ask_size, ofi,
    inventory, cash, pnl_mid, pnl_fair, signed_volume

Usage:
    python3 scripts/plot_mm.py mm.csv --out docs/mm_overview.png [--ofi-window 200]

Requires: pandas, matplotlib  (analysis-only tooling; the C++ engine stays
dependency-free).
"""
import argparse
import sys

import matplotlib
matplotlib.use("Agg")  # headless: render to file, never open a window
import matplotlib.pyplot as plt
import pandas as pd


def render(csv_path: str, out_path: str, ofi_window: int) -> None:
    df = pd.read_csv(csv_path)
    if df.empty:
        raise SystemExit(f"{csv_path}: no rows to plot")

    t = df["t"]
    fig, axes = plt.subplots(2, 2, figsize=(13, 8), sharex=True)
    fig.suptitle(f"Market-making lab — {csv_path}", fontsize=13, fontweight="bold")

    # (0,0) Price: latent fair value vs observable mid vs microprice.
    ax = axes[0, 0]
    ax.plot(t, df["fair"], label="fair value (latent)", lw=1.0, color="#d77757")
    ax.plot(t, df["mid"], label="book mid", lw=1.0, color="#0891b2", alpha=0.85)
    ax.plot(t, df["microprice"], label="microprice", lw=0.8, color="#827dbd", alpha=0.7)
    ax.set_title("Price: fair value vs. mid vs. microprice")
    ax.set_ylabel("ticks")
    ax.legend(loc="best", fontsize=8)

    # (0,1) PnL: mark-to-mid vs mark-to-fair (both cumulative, tick*share).
    ax = axes[0, 1]
    ax.axhline(0, color="#888", lw=0.8)
    ax.plot(t, df["pnl_mid"], label="PnL (mark-to-mid)", lw=1.1, color="#0891b2")
    ax.plot(t, df["pnl_fair"], label="PnL (mark-to-fair)", lw=1.1, color="#d77757")
    ax.set_title("Market-maker PnL")
    ax.set_ylabel("ticks * shares")
    ax.legend(loc="best", fontsize=8)

    # (1,0) Inventory over time, with the signed position shaded.
    ax = axes[1, 0]
    ax.axhline(0, color="#888", lw=0.8)
    ax.fill_between(t, df["inventory"], 0, color="#827dbd", alpha=0.35)
    ax.plot(t, df["inventory"], lw=0.9, color="#5b53a3")
    ax.set_title("Inventory (signed position)")
    ax.set_ylabel("shares")
    ax.set_xlabel("step")

    # (1,1) Order-flow imbalance, smoothed (per-step OFI is very noisy).
    ax = axes[1, 1]
    ax.axhline(0, color="#888", lw=0.8)
    smoothed = df["ofi"].rolling(ofi_window, min_periods=1).mean()
    ax.plot(t, smoothed, lw=1.0, color="#0891b2")
    ax.set_title(f"Order-flow imbalance (rolling mean, w={ofi_window})")
    ax.set_ylabel("net signed depth")
    ax.set_xlabel("step")

    fig.tight_layout(rect=(0, 0, 1, 0.97))
    fig.savefig(out_path, dpi=150)
    print(f"wrote {out_path}  ({len(df)} steps)")


def main() -> int:
    p = argparse.ArgumentParser(description="Plot the mm-sim per-step CSV.")
    p.add_argument("csv", help="per-step CSV from orderbook --mm-sim ... --out")
    p.add_argument("--out", default="docs/mm_overview.png", help="output PNG path")
    p.add_argument("--ofi-window", type=int, default=200,
                   help="rolling window (steps) for smoothing OFI")
    args = p.parse_args()
    try:
        render(args.csv, args.out, args.ofi_window)
    except FileNotFoundError:
        print(f"error: no such file: {args.csv}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
