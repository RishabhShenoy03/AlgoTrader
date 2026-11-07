#!/usr/bin/env python3
"""Run reproducible offline experiments through the C++ engine; no broker/network calls."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess

PERIODS = {"develop": ("2011-01-01", "2018-12-31"),
           "validate": ("2019-01-01", "2021-12-31"),
           "holdout": ("2022-01-01", "2025-12-31")}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(args):
    start, end = PERIODS[args.stage]
    binary = Path(args.binary).resolve()
    instruments = list(csv.DictReader(Path(args.instruments).open()))
    if len(instruments) != 1 or instruments[0]["asset"] != "stock":
        raise ValueError("This first research protocol requires exactly one stock/ETF")
    if not all(math.isfinite(x) and x > 0 for x in (args.cash, args.quantity)):
        raise ValueError("cash and quantity must be finite and positive")
    if args.stage != "develop" and args.window is None:
        raise ValueError("Freeze a --window before running validation or holdout")
    windows = [50, 100, 200] if args.stage == "develop" else [args.window]
    if any(w < 2 or w > 250 for w in windows):
        raise ValueError("window must be 2..250")
    rows = list(csv.DictReader(Path(args.data).open()))
    if not rows or list(rows[0]) != ["date", "ticker", "open", "close"]:
        raise ValueError("Expected engine CSV: date,ticker,open,close")
    previous = ""
    for row in rows:
        from datetime import date
        if len(row["date"]) != 10:
            raise ValueError("Daily YYYY-MM-DD data required")
        date.fromisoformat(row["date"])
        if row["date"] <= previous or row["ticker"] != instruments[0]["ticker"]:
            raise ValueError("Data must be sorted, unique and match the single instrument")
        previous = row["date"]
        if any(not math.isfinite(float(row[k])) or float(row[k]) <= 0 for k in ("open", "close")):
            raise ValueError("Invalid price")
    selected = [r for r in rows if start <= r["date"] <= end]
    if len(selected) < max(windows) + 252:
        raise ValueError("Need at least window + 252 bars within the selected period")
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=False)  # Never overwrite research evidence.
    shutil.copyfile(args.instruments, output / "instruments.csv")
    with (output / "bars.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["date", "ticker", "open", "close"])
        writer.writeheader()
        writer.writerows(selected)
    manifest = {"stage": args.stage, "from": start, "to": end, "actual_first": selected[0]["date"],
                "actual_last": selected[-1]["date"], "bars": len(selected), "windows": windows,
                "cash": args.cash, "quantity": args.quantity, "source": args.source,
                "full_input_sha256": digest(args.data), "period_input_sha256": digest(output / "bars.csv"),
                "instrument_sha256": digest(args.instruments), "engine_sha256": digest(binary),
                "runner_sha256": digest(__file__), "cold_start": True, "commands": []}
    results = []
    cases = [("buy-hold", 100)] + [("sma", w) for w in windows]
    for cost, fee, slippage in [("base", 1.10, 5), ("stress", 2.20, 20)]:
        for strategy, window in cases:
            name = f"{strategy}-{window}-{cost}"
            dest = output / name
            cmd = [str(binary), "backtest", "--instruments", str(output / "instruments.csv"),
                   "--data", str(output / "bars.csv"), "--strategy", strategy, "--window", str(window),
                   "--cash", str(args.cash), "--quantity", str(args.quantity), "--fee", str(fee),
                   "--slippage-bps", str(slippage), "--output", str(dest)]
            manifest["commands"].append(cmd)
            (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            completed = subprocess.run(cmd, text=True, capture_output=True)
            (output / f"{name}.log").write_text(completed.stdout + completed.stderr)
            if completed.returncode:
                raise RuntimeError(f"Engine failed: inspect {output / (name + '.log')}")
            summary = next(csv.DictReader((dest / "summary.csv").open()))
            trades = list(csv.DictReader((dest / "trades.csv").open()))
            decisions = list(csv.DictReader((dest / "decisions.csv").open()))
            holding = sum(float(t["quantity"]) * (1 if t["side"] == "buy" else -1) for t in trades)
            results.append({"run": name, "net_pnl": float(summary["final_equity"]) - args.cash,
                            "return_pct": 100 * float(summary["total_return"]),
                            "drawdown_pct": 100 * float(summary["max_drawdown"]),
                            "executions": int(summary["executions"]), "fees": float(summary["fees"]),
                            "rejected_intents": sum(d["decision"] == "rejected" for d in decisions),
                            "ending_shares": holding})
    with (output / "comparison.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(results[0]))
        writer.writeheader()
        writer.writerows(results)
    text = [f"# {args.stage.title()} experiment", "",
            f"{selected[0]['date']} to {selected[-1]['date']}; {len(selected)} daily bars.",
            f"Fixed {args.quantity:g} shares per entry; initial cash ${args.cash:g}.", "",
            "| Run | Net P&L | Return % | Drawdown % | Executions | Rejected intents |",
            "| --- | ---: | ---: | ---: | ---: | ---: |"]
    for r in results:
        text.append(f"| {r['run']} | {r['net_pnl']:.2f} | {r['return_pct']:.2f} | "
                    f"{r['drawdown_pct']:.2f} | {r['executions']} | {r['rejected_intents']} |")
    text += ["", "## Interpretation limits", "",
             "All strategies start flat and cold on the same date. SMA waits its window before trading; "
             "buy-and-hold enters after the first close. Warmup is included in reported returns. "
             "Fixed-share sizing and idle cash dilute account returns and are not constant-risk allocation.", "",
             "P&L includes open-position marks, but no terminal liquidation costs. "
             "No dividends, interest on idle cash, taxes or corporate-action share changes are modeled. "
             "Split-adjusted history is a research approximation, not a literal historical share ledger. "
             "Inspect rejected intents before trusting comparisons.", "",
             "Do not choose the best row mechanically. Look for parameter stability, cost sensitivity "
             "and drawdown tradeoffs. Validation and final holdout require separate explicit runs. "
             "This report is not evidence of a deployable edge."]
    (output / "report.md").write_text("\n".join(text) + "\n")
    print("\n".join(text))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/ibkr/algotrader")
    parser.add_argument("--instruments", default="examples/ibkr-aapl.csv")
    parser.add_argument("--data", required=True)
    parser.add_argument("--stage", choices=PERIODS, default="develop")
    parser.add_argument("--window", type=int)
    parser.add_argument("--cash", type=float, default=10000)
    parser.add_argument("--quantity", type=float, default=10)
    parser.add_argument("--source", required=True, help="Provider, retrieval time, adjustment policy/unknowns")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    try:
        run(args)
    except (ValueError, RuntimeError, OSError) as e:
        parser.exit(1, f"Research failed: {e}\n")


if __name__ == "__main__":
    main()
