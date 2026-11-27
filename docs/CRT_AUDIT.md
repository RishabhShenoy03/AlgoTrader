# CRT backtest audit

Audit of the 4H→15M candle-range backtest in `CrtBacktest.cpp`. Everything below is
recomputable from the committed scripts and the CSVs under `results/crt/`.

Reproduce:

```sh
export LD_LIBRARY_PATH=/home/rish/.local/share/algotrader/build-deps/usr/lib/aarch64-linux-gnu

# tuning year (2025-11-28 .. 2026-11-27)
./build/dev/algotrader crt-backtest --instruments examples/ibkr-aapl.csv \
  --data results/crt/aapl-15m/bars.csv --quantity 100 --fee 1.10 --slippage-bps 5 \
  --output results/crt/runs-audit/base
./build/dev/algotrader crt-backtest --instruments examples/ibkr-aapl.csv \
  --data results/crt/aapl-15m/bars.csv --quantity 100 --fee 2.20 --slippage-bps 20 \
  --output results/crt/runs-audit/stress
./build/dev/algotrader crt-backtest --instruments examples/ibkr-aapl.csv \
  --data results/crt/aapl-15m/bars.csv --quantity 100 --long-only \
  --output results/crt/runs-audit/long-only

# untouched prior year (2024-11-27 .. 2025-11-26)
./build/dev/algotrader crt-backtest --instruments examples/ibkr-aapl.csv \
  --data results/crt/aapl-15m-prior/bars.csv --quantity 100 --long-only \
  --output results/crt/runs-prior/long-only

python3 scripts/audit_crt.py --bars results/crt/aapl-15m/bars.csv \
  --run results/crt/runs-audit/base/<stamp> --fee 1.10 --slippage-bps 5 \
  --label base --out /tmp/audit.md
```

`scripts/audit_crt.py` rebuilds the candles from the raw bars independently of the engine,
re-derives every trade's range/sweep/stop/target, and reconciles gross, slippage, fees and
net from the executed prices. All five runs below reconcile with **0 problems**.

## 1. Gross, costs and net per trade

| Case | Trades | Notional (entry+exit) | Gross P&L | Slippage | Fees | Net | Return | Max DD |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Tuning year, both sides, base | 81 | 4,808,685 | +1,461.00 | −2,404.34 | −178.20 | **−1,121.54** | −4.06% | 7.73% |
| Tuning year, both sides, stress | 81 | 4,808,685 | +1,461.00 | −9,617.37 | −356.40 | **−8,512.77** | −30.80% | 31.47% |
| Tuning year, long-only | 44 | 2,574,228 | +4,516.00 | −1,287.11 | −96.80 | **+3,132.09** | +11.33% | 3.25% |
| **Prior year, long-only (frozen)** | 49 | 2,233,088 | +300.00 | −1,116.54 | −107.80 | **−924.34** | −3.95% | 7.54% |
| Prior year, both sides | 79 | 3,626,550 | −3,008.00 | −1,813.27 | −173.80 | **−4,995.07** | −21.32% | 22.45% |

Slippage is charged on both sides at the stated bps of price, so it scales with notional:
5 bps on $4.81M of two-way turnover is ~$2.4k, which is 1.6× the entire gross edge.

### Correction to the earlier summary

The first write-up said "gross win $10,081 vs gross loss $10,736 — a genuinely thin edge
on the right side of the ledger". That statement was wrong in two separate ways:

1. **The figures were not gross.** They were the sums of per-trade `pnl`, which the engine
   already had reduced by both commissions and slippage. The label was mine and incorrect;
   the report now says "sum of winning/losing trades (net of costs)".
2. **The edge was not positive.** $10,081 < $10,736, so even on those already-netted
   figures the losing trades outweighed the winners. There was no edge on any side.

The true gross edge on the tuning year is **+$1,461 before costs**, and costs are $2,583 —
which is the whole reason the net is negative. The near-equality of "gross wins" and net
result was an artefact of both being net-of-cost numbers, not a coincidence worth reading
into.

## 2. Baseline vs stress reconciliation

Both cases trade the same 81 setups: identical entries, exits and notional, because costs
do not change the rule. Only the price adjustments differ.

- **Commissions:** base $1.10/side × 162 sides = $178.20; stress $2.20/side = $356.40.
- **Slippage:** base 5 bps, stress 20 bps, applied to entry and exit notional; $2,404.34 vs
  $9,617.37, a 4× ratio as expected from 4× the bps.
- **Capital denominator:** `quantity × first bar close` — 100 × 276.42 = **$27,642** for the
  tuning year, 100 × 234.30 = **$23,430** for the prior year. Both runs of a year share the
  same denominator, so returns are comparable within a year and across cases.

### Equity curve consistency

Return and drawdown are computed from the **same** realised equity series
(`equity.csv`, one row per closed trade). Checked: `end_equity − start_equity` equals net
P&L exactly, and max drawdown recomputed from that file matches the reported figure.

Two limits, stated plainly:

- The curve is **realised only**. It contains no mark-to-market while a position is open, so
  an intra-trade excursion worse than any closed-trade drawdown would not appear. Drawdowns
  here are therefore lower bounds.
- Drawdown is measured on a 100-share position against a fixed notional, not on account
  equity with margin. It is a trade-sequence drawdown, not a portfolio one.

## 3. Candle boundaries, overnight and daylight saving

The data is regular-hours 15-minute bars only, so a "4H candle" is **16 consecutive bars**,
not a clock window. Measured on the tuning year: **219 of 405 candles (54%) span a calendar
day**, and the longest span a full weekend.

Examples (all timestamps UTC, straight from `bars.csv`):

| Trade | Range candle | Sweep candle | Entry | Exit |
| --- | --- | --- | --- | --- |
| Spring DST 2026-03-08 | 2026-02-27 16:00 → 19:45, high 269.64 low 265.26 | 2026-02-27 20:00 → **2026-03-02 17:15** (spans the weekend and the DST change), high 266.90 low 260.20 | 2026-03-02 17:30 long | 2026-03-03 17:30 stop |
| Overnight | 2026-02-13 14:30 → 18:15, high 262.23 low 258.37 | 2026-02-13 18:30 → **2026-02-17 15:45** (Presidents' Day holiday inside), high 261.39 low 255.45 | 2026-02-17 16:00 long | 2026-02-17 17:00 target |

Daylight saving is handled correctly at the data layer: session open is 14:30Z before
2026-03-09 and 13:30Z after (EST→EDT), then 13:30Z until 2026-10-30 and 14:30Z from
2026-11-02 (EDT→EST). IBKR returned UTC timestamps; nothing in the engine does timezone
arithmetic.

Consequences that are **not** handled, because the candle is defined by bar count:

- A half-session (three days in the sample have 5–14 bars instead of 26) shifts every
  subsequent candle boundary for the rest of the series.
- A candle that spans the weekend includes Friday's close and Monday's open in one range.
- The "4H" label is four hours of *trading time*, i.e. about 6.5 calendar hours plus any
  overnight gap. It is not what a charting platform draws.

### Time exit: 15-minute or 4-hour candles?

`--max-hold-bars` counts **15-minute bars**, default 32, which is 8 hours of trading time —
two 4H candles. Verified from the runs: all 13 time exits held exactly 33 bars
(entry bar plus 32), consistently. The report wording "two candles" therefore means
2 × 16 fifteen-minute bars, **not** two 4-hour *clock* candles.

## 4. Look-ahead, gaps and same-bar ambiguity

Checked per trade by `audit_crt.py` against an independent rebuild of the candles:

- **No look-ahead on entry.** Every entry timestamp is exactly the bar after the sweep
  candle's last bar, so the sweep candle is fully closed before the order is sent.
- **No look-ahead on levels.** Every stop equals that sweep candle's extreme on the swept
  side and every target equals the *range* candle's opposite edge — both fully determined
  before entry.
- **Same-bar stop and target.** Resolved as the **stop**, always. 39 of 81 tuning-year exits
  are stops and 29 are targets, so the pessimism rule is doing real work; the opposite
  assumption would raise returns.
- **Gap handling — a real bug, found in this audit and fixed.** A bar that opened beyond the
  stop was filled *at the stop price*, inventing money a stop order could not have captured.
  It now fills at the open when the open is worse than the stop. Fixing it moved the tuning
  year from −2.37% to **−4.06%** and stress from −29.1% to −30.8%, i.e. the earlier figures
  were flattered by the bug.
- **Remaining gap optimism:** target exits are always filled at the target price even when a
  bar opens through it. That is conservative in the wrong direction only for the strategy
  (it forgoes favourable gaps), so it cannot inflate results.
- **Entry gap guard:** if the entry bar opens at or beyond the target, the setup is skipped
  rather than entered; if the entry bar opens beyond the stop, the trade is skipped because
  risk is not positive. On the prior year that happened 3 times (82 setups recorded, 79
  trades) and 2 times on the long-only run (51 setups, 49 trades).

## 5. IBKR historical-data request and permissions

Inspected before concluding anything about subscriptions:

- The request is `reqHistoricalData(..., whatToShow="TRADES", useRTH=regular_hours ? 1 : 0,
  formatDate=2)`. `--extended-hours` sets `useRTH=0`; verified in `IbkrBroker.cpp` and by
  the CLI flag being accepted.
- **With `useRTH=0`, durations of 1 year, 3 days and 1 day, at both 15-minute and 1-minute
  granularity, every returned bar falls inside 13:30–21:00Z.** No pre-market or after-hours
  timestamps appear in any response. Bar counts are ~26 per session.
- No IBKR error for these requests beyond advisory 2188 ("up-to-the-second historical data
  requires additional subscription"), which concerns real-time bars, not this data.

So: the request is correct, the response is regular-hours-only, and IBKR returns no
permission error. The two candidate explanations are (a) the account has no US market-data
subscription covering extended hours, or (b) paper accounts are served regular-hours
history regardless. Distinguishing them needs the portal's Settings → Market Data
Subscriptions page, which requires your login. **I did not purchase or change anything.**
My earlier statement that this "requires a new subscription" overstated what the evidence
shows; this is the corrected version.

## 6. Frozen long-only rules on the untouched prior year

The long-only variant was chosen *after* seeing the tuning year, so it is not evidence. It
was frozen (16-bar candles, stop at the sweep extreme, target at the opposite edge, 32-bar
time exit, 100 shares, $1.10/side, 5 bps, no shorts) and run once on a non-overlapping
earlier year pulled with `--end-time 20251128-00:00:00`: **2024-11-27 → 2025-11-26**,
6,464 bars, no overlap with the tuning sample.

| | Tuning year | Prior year (untouched) |
| --- | ---: | ---: |
| Trades | 44 | 49 |
| Net P&L | +3,132.09 | **−924.34** |
| Return on deployed capital | +11.33% | **−3.95%** |
| Max drawdown | 3.25% | 7.54% |
| Gross before costs | +4,516.00 | +300.00 |
| Costs | −1,383.91 | −1,224.34 |

The variant does not survive. Its gross edge on the prior year is $300 against $1,224 of
costs — statistically and economically indistinguishable from zero. The +11% was sample
specific.

The both-directions rule is negative in both years (−4.06%, −21.32%).

## 7. Corn futures: what would be needed

Not enabled, not tested; assessment only.

- **Data.** No corn contract is configured. `IbkrBroker` requires a positive `con_id` and
  there is no symbol→contract resolution, so even the front-month ZC future cannot be
  requested today. Access also needs a CME futures market-data entitlement, which is a
  separate subscription from equities and is not evidenced on this account.
- **Simulator features.** `CrtConfig` sizes in *shares at a price*. A future needs the
  contract multiplier (5,000 bushels for ZC, $12.50 per 0.25¢ tick), tick-size rounding,
  initial and maintenance margin, and a roll rule — plus the CME session with its daily
  break, which the bar-count candle definition cannot represent.
- **Permissions.** Futures trading on the paper account would need to be enabled at IBKR;
  it is not, and I have not changed it.

## 8. Verdict

The accounting reconciles exactly and the mechanics are sound once the gap bug is fixed.
The strategy is not: it has a positive gross edge too small to cover turnover, it loses on
both the tuning year and an untouched prior year, and its long-only variant collapses out
of sample. Every number that looked attractive came from either a cost mislabel, an
uncosted gap assumption, or in-sample selection.

## 9. The next decision

Whether to (a) accept the CRT result as a completed negative experiment and move the
research to a different hypothesis, or (b) fund nothing further and stop — that is a
judgement about how much you want to learn from a documented failure.

If (a), the highest-value next experiment is not another CRT variant on the same data with
the same cost structure. It is a market whose range is large relative to its tick and
commission cost, with the contract-resolution work done first so the data is not silently
regular-hours-only.
