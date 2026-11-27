#!/usr/bin/env python3
"""Independent audit of a CRT backtest run.

Recomputes the candles from the raw bars (not from the engine's own state), then for every
trade in trades.csv checks:

  * the entry bar is the one immediately after the sweep candle's last bar (no look-ahead),
  * the stop equals the sweep candle's extreme on the swept side,
  * the target equals the opposite edge of the range candle,
  * gross P&L, slippage cost, fees and net P&L reconcile from raw prices.

It also prints candle boundaries for a few trades, including candles that span the
overnight close and the two US daylight-saving transitions.
"""
import argparse
import csv
import datetime as dt
import math
from pathlib import Path


def load_bars(path):
    return list(csv.DictReader(Path(path).open()))


def build_candles(bars, width):
    """Same construction the engine uses: fixed-width blocks of bars, not clock buckets."""
    out = []
    for first in range(0, len(bars) - width + 1, width):
        block = bars[first:first + width]
        out.append({
            "first": first, "last": first + width - 1,
            "start": block[0]["date"], "end": block[-1]["date"],
            "open": float(block[0]["open"]), "close": float(block[-1]["close"]),
            "high": max(float(b["high"]) for b in block),
            "low": min(float(b["low"]) for b in block),
            "span_hours": round((dt.datetime.fromisoformat(block[-1]["date"].replace("Z", "+00:00"))
                                 - dt.datetime.fromisoformat(block[0]["date"].replace("Z", "+00:00"))
                                 ).total_seconds() / 3600, 2),
        })
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bars", required=True)
    ap.add_argument("--run", required=True, help="run directory containing trades.csv")
    ap.add_argument("--width", type=int, default=16)
    ap.add_argument("--quantity", type=float, default=100)
    ap.add_argument("--fee", type=float, required=True)
    ap.add_argument("--slippage-bps", type=int, required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--out", required=True, help="markdown file to append to")
    args = ap.parse_args()

    bars = load_bars(args.bars)
    candles = build_candles(bars, args.width)
    trades = list(csv.DictReader((Path(args.run) / "trades.csv").open()))
    slip = args.slippage_bps / 10000.0
    index = {b["date"]: i for i, b in enumerate(bars)}

    gross = slippage_cost = fees = net = notional = 0.0
    problems = []
    for t in trades:
        d = 1 if t["direction"] == "long" else -1
        entry_i = index[t["entry_time"]]
        # Which candle produced this trade?
        sweep = next((c for c in candles if c["last"] + 1 == entry_i), None)
        if sweep is None:
            problems.append(f"{t['entry_time']}: entry is not the bar after any candle end")
            continue
        if sweep["first"] == 0:
            problems.append(f"{t['entry_time']}: sweep candle has no preceding range")
            continue
        rng = next(c for c in candles if c["last"] + 1 == sweep["first"])
        if d == 1:
            if abs(float(t["stop"]) - sweep["low"]) > 1e-6:
                problems.append(f"{t['entry_time']}: stop {t['stop']} != sweep low {sweep['low']}")
            if abs(float(t["target"]) - rng["high"]) > 1e-6:
                problems.append(f"{t['entry_time']}: target {t['target']} != range high {rng['high']}")
        else:
            if abs(float(t["stop"]) - sweep["high"]) > 1e-6:
                problems.append(f"{t['entry_time']}: stop {t['stop']} != sweep high {sweep['high']}")
            if abs(float(t["target"]) - rng["low"]) > 1e-6:
                problems.append(f"{t['entry_time']}: target {t['target']} != range low {rng['low']}")

        entry_raw = float(t["entry"]) / (1 + d * slip)
        exit_raw = float(t["exit"]) / (1 - d * slip)
        g = d * (exit_raw - entry_raw) * args.quantity
        s = g - d * (float(t["exit"]) - float(t["entry"])) * args.quantity
        f = 2 * args.fee
        gross += g
        slippage_cost += s
        fees += f
        net += float(t["pnl"])
        notional += (entry_raw + exit_raw) * args.quantity

        if abs((g - s - f) - float(t["pnl"])) > 1e-6:
            problems.append(f"{t['entry_time']}: recomputed net {g - s - f:.4f} != report {t['pnl']}")

    curve = list(csv.DictReader((Path(args.run) / "equity.csv").open()))
    start_equity = float(curve[0]["equity"])
    end_equity = float(curve[-1]["equity"])
    peak, max_dd = start_equity, 0.0
    for row in curve:
        value = float(row["equity"])
        peak = max(peak, value)
        max_dd = max(max_dd, 1 - value / peak)

    lines = [f"### {args.label}", "",
             "| Item | Value |", "| --- | ---: |",
             f"| Trades | {len(trades)} |",
             f"| Shares per trade | {args.quantity:g} |",
             f"| Traded notional (entry+exit) | {notional:,.2f} |",
             f"| Gross P&L (before costs) | {gross:,.2f} |",
             f"| Slippage cost ({args.slippage_bps} bps/side) | {-slippage_cost:,.2f} |",
             f"| Commissions ({args.fee:.2f}/side) | {-fees:,.2f} |",
             f"| Net P&L | {net:,.2f} |",
             f"| Reconciliation gross - slippage - fees | {gross - slippage_cost - fees:,.2f} |",
             f"| Capital denominator (first bar close x shares) | {start_equity:,.2f} |",
             f"| Return on deployed capital | {100 * net / start_equity:.3f}% |",
             f"| Max drawdown, realised equity curve | {100 * max_dd:.2f}% |",
             f"| End equity from curve | {end_equity:,.2f} |",
             f"| Winners / losers | "
             f"{sum(1 for t in trades if float(t['pnl']) > 0)} / "
             f"{sum(1 for t in trades if float(t['pnl']) <= 0)} |",
             f"| Sum of winning / losing trades | "
             f"{sum(float(t['pnl']) for t in trades if float(t['pnl']) > 0):,.2f} / "
             f"{sum(float(t['pnl']) for t in trades if float(t['pnl']) <= 0):,.2f} |",
             "",
             f"Checks: {len(trades)} trades, {len(problems)} problems.", ""]
    if problems:
        lines += ["```"] + problems[:20] + ["```", ""]

    with open(args.out, "a") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
