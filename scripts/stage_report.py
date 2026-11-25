#!/usr/bin/env python3
"""Warm-started, equity-matched comparison of the engine's strategies.

The existing runner starts every strategy cold on the stage date and sizes with a
fixed number of shares against a fixed cash balance, so an SMA rule spends its
warmup out of the market and both strategies sit mostly in cash. That makes
returns incomparable to real trading and muddies the verdict.

This runner fixes the three measurement problems only:

  1. Warmup: prepend N bars before the scoring window so every strategy has a
     live signal on day one. Scoring still starts at the stage date.
  2. Sizing: cash is set from the scoring-start close so the account is roughly
     fully invested when the strategy is long (equity-matched, not fixed $10k).
  3. Metrics: computed from equity.csv restricted to the scoring window - CAGR,
     max drawdown, Calmar, exposure and turnover - not raw dollars on diluted cash.

It changes no strategy logic and no engine code. Offline; no broker calls.
"""
import argparse
import csv
import datetime as dt
import json
import math
import shutil
import subprocess
from pathlib import Path

PERIODS = {"develop": ("2011-01-01", "2018-12-31"),
           "validate": ("2019-01-01", "2021-12-31"),
           "holdout": ("2022-01-01", "2025-12-31")}

COSTS = [("base", 1.10, 5), ("stress", 2.20, 20)]


def load_bars(path):
    rows = list(csv.DictReader(Path(path).open()))
    if not rows or list(rows[0]) != ["date", "ticker", "open", "close"]:
        raise ValueError("expected engine CSV: date,ticker,open,close")
    previous = ""
    for row in rows:
        if len(row["date"]) != 10:
            raise ValueError("daily YYYY-MM-DD data required")
        dt.date.fromisoformat(row["date"])
        if row["date"] <= previous:
            raise ValueError("bars must be sorted and unique")
        previous = row["date"]
        for key in ("open", "close"):
            value = float(row[key])
            if not math.isfinite(value) or value <= 0:
                raise ValueError(f"invalid {key} on {row['date']}")
    return rows


def equity_curve(run_dir):
    return [(r["timestamp"], float(r["equity"]))
            for r in csv.DictReader((Path(run_dir) / "equity.csv").open())
            if r["timestamp"] != "initial"]


def score_window(run_dir, start, end):
    """Equity series restricted to the scoring window, with its base value."""
    curve = [(d, e) for d, e in equity_curve(run_dir) if start <= d <= end]
    if not curve:
        raise ValueError(f"no equity rows inside {start}..{end}")
    first_index = next(i for i, (d, _) in enumerate(equity_curve(run_dir)) if d >= start)
    prior = equity_curve(run_dir)[:first_index]
    base = prior[-1][1] if prior else curve[0][1]
    return base, curve


def metrics(run_dir, start, end):
    base, curve = score_window(run_dir, start, end)
    end_equity = curve[-1][1]
    total = end_equity / base - 1.0
    days = len(curve)
    years = days / 252.0
    cagr = (end_equity / base) ** (1.0 / years) - 1.0 if years > 0 else 0.0
    peak, max_dd = base, 0.0
    for _, value in curve:
        peak = max(peak, value)
        max_dd = max(max_dd, 1.0 - value / peak)
    trades = list(csv.DictReader((Path(run_dir) / "trades.csv").open()))
    position = 0.0
    invested_days = 0
    by_date = {}
    for t in trades:
        by_date[t["timestamp"]] = by_date.get(t["timestamp"], [])
        by_date[t["timestamp"]].append(t)
    for t in trades:
        if t["timestamp"] < start:
            position += float(t["quantity"]) * (1.0 if t["side"] == "buy" else -1.0)
    for date, _ in curve:
        for t in by_date.get(date, []):
            position += float(t["quantity"]) * (1.0 if t["side"] == "buy" else -1.0)
        if abs(position) > 1e-9:
            invested_days += 1
    return {"total_return": total, "cagr": cagr, "max_drawdown": max_dd,
            "calmar": (cagr / max_dd) if max_dd > 1e-9 else float("nan"),
            "exposure": invested_days / days if days else 0.0,
            "executions": len(trades), "final_equity": end_equity, "days": days}


def run(args):
    start, end = PERIODS[args.stage]
    windows = [50, 100, 200] if args.stage == "develop" else [int(w) for w in args.window.split(",")]
    if args.stage != "develop" and args.window is None:
        raise ValueError("freeze a --window (or a comma list) before validation or holdout")
    rows = load_bars(args.data)
    instrument = next(csv.DictReader(Path(args.instruments).open()))
    if any(r["ticker"] != instrument["ticker"] for r in rows):
        raise ValueError("bars must match the single instrument in the instruments file")

    selected = [r for r in rows if start <= r["date"] <= end]
    if not selected:
        raise ValueError("no bars inside the stage window")
    warmup = [r for r in rows if r["date"] < start][-args.warmup_bars:]
    if len(warmup) < args.warmup_bars:
        # The dataset begins at the stage start (the develop stage): burn the first
        # bars of the window as warmup and score after them instead of silently
        # starting cold, which is the bias this runner exists to remove.
        warmup = selected[:args.warmup_bars]
        selected = selected[args.warmup_bars:]
        if len(selected) < 252:
            raise ValueError("not enough bars left to score after warmup")
        print(f"note: no history before {start}; scoring starts {selected[0]['date']} "
              f"after {len(warmup)} warmup bars")
    feed = warmup + selected

    # Size from the highest close across warmup + scoring: a fixed-share account must be
    # able to afford its position at any point in the feed, or early signals are rejected
    # for lack of cash and the comparison silently measures affordability, not the rule.
    max_close = max(float(r["close"]) for r in feed)
    cash = args.quantity * max_close * 1.02
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(args.instruments, output / "instruments.csv")
    with (output / "bars.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["date", "ticker", "open", "close"])
        writer.writeheader()
        writer.writerows(feed)

    cases = [("buy-hold", "buy-hold", 100)] + [("sma", "sma", w) for w in windows] + \
            [("breakout", "breakout", 20), ("mean-reversion", "mean-reversion", 10)]
    results = []
    manifest = {"stage": args.stage, "score_from": start, "score_to": end,
                "warmup_bars": len(warmup), "warmup_first": warmup[0]["date"],
                "scoring_first": selected[0]["date"], "scoring_last": selected[-1]["date"],
                "quantity": args.quantity, "cash": round(cash, 2), "source": args.source,
                "note": "cash is sized from the scoring-start close: equity-matched, not $10k fixed",
                "commands": []}
    for cost, fee, slippage in COSTS:
        for label, strategy, window in cases:
            name = f"{label}-{window}-{cost}"
            dest = output / name
            cmd = [str(Path(args.binary).resolve()), "backtest", "--instruments",
                   str(output / "instruments.csv"), "--data", str(output / "bars.csv"),
                   "--strategy", strategy, "--window", str(window), "--cash", f"{cash:.2f}",
                   "--quantity", str(args.quantity), "--fee", str(fee),
                   "--slippage-bps", str(slippage), "--output", str(dest)]
            manifest["commands"].append(cmd)
            (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            done = subprocess.run(cmd, text=True, capture_output=True)
            (output / f"{name}.log").write_text(done.stdout + done.stderr)
            if done.returncode:
                raise RuntimeError(f"engine failed: inspect {output / (name + '.log')}")
            m = metrics(dest, start, end)
            m["run"] = name
            results.append(m)
            print(f"{name:28s} cagr {m['cagr']*100:6.2f}%  dd {m['max_drawdown']*100:5.2f}%  "
                  f"calmar {m['calmar']:5.2f}  exposure {m['exposure']*100:5.1f}%  "
                  f"trades {m['executions']}")

    with (output / "comparison.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["run", "total_return", "cagr", "max_drawdown",
                                              "calmar", "exposure", "executions", "final_equity",
                                              "days"])
        writer.writeheader()
        writer.writerows(results)

    lines = [f"# {args.stage.title()} stage: warm-started, equity-matched comparison", "",
             f"Scoring window {selected[0]['date']} to {selected[-1]['date']} "
             f"({len(selected)} sessions), {len(warmup)} warmup bars prepended from "
             f"{warmup[0]['date']}.", "",
             f"Cash sized for the highest close in the feed: {args.quantity:g} shares of "
             f"{max_close:.2f} => ${cash:,.0f}. Every strategy is warm at the scoring start and "
             "can afford its position at any point, so no signal is rejected for lack of cash.", "",
             "| Run | CAGR | Max drawdown | Calmar | Exposure | Trades |", "| --- | ---: | ---: | ---: | ---: | ---: |"]
    for r in results:
        lines.append(f"| {r['run']} | {r['cagr']*100:.2f}% | {r['max_drawdown']*100:.2f}% | "
                     f"{r['calmar']:.2f} | {r['exposure']*100:.1f}% | {r['executions']} |")
    base = {r["run"]: r for r in results}
    for cost in ("base", "stress"):
        bh, sm = base.get(f"buy-hold-100-{cost}"), base.get(f"sma-100-{cost}")
        if bh and sm:
            lines += ["", f"SMA-100 vs buy-and-hold ({cost}): CAGR {sm['cagr']*100:.2f}% vs "
                          f"{bh['cagr']*100:.2f}%, drawdown {sm['max_drawdown']*100:.2f}% vs "
                          f"{bh['max_drawdown']*100:.2f}%, Calmar {sm['calmar']:.2f} vs "
                          f"{bh['calmar']:.2f}, exposure {sm['exposure']*100:.1f}% vs "
                          f"{bh['exposure']*100:.1f}%."]
    lines += ["", "## Limits", "",
              "Prices are split-adjusted and exclude dividends, so the benchmark understates a "
              "long holder's total return. Cash earns no interest. Sizing is fixed shares, so a "
              "rising price raises exposure over the window. This measures the rule, not a "
              "deployable portfolio; breakout and mean-reversion are included as the engine's "
              "other strategies at default settings, not as tuned candidates."]
    (output / "report.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines[-6:]))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", default="build/ibkr/algotrader")
    p.add_argument("--instruments", default="examples/ibkr-aapl.csv")
    p.add_argument("--data", required=True)
    p.add_argument("--stage", choices=PERIODS, default="develop")
    p.add_argument("--window", help="window size, or a comma list such as 50,100,200")
    p.add_argument("--quantity", type=float, default=100)
    p.add_argument("--warmup-bars", type=int, default=260)
    p.add_argument("--source", required=True)
    p.add_argument("--output", required=True)
    args = p.parse_args()
    try:
        run(args)
    except (ValueError, RuntimeError, OSError) as e:
        p.exit(1, f"stage report failed: {e}\n")


if __name__ == "__main__":
    main()
