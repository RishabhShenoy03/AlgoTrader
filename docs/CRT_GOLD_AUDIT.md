# Independent review of the gold CRT experiment (corrected)

Review of `c88080a` on `feat/crt-gold`. Corrections live on `feat/crt-gold-audit` in new
output directories (`results/crt-gold-v2/`); the original report and its results are
untouched apart from a correction banner pointing here.

Verdicts in one line: **four of the five claimed defects are real and are fixed; the fifth
(cutoff) is worse than claimed — the original cutoff sat after the contract's first notice
day.** All results remain negative, but the corrected engine is stricter and the losses are
larger, because slippage is now actually charged.

## 1. Claim-by-claim

| # | Claim | Verdict | Evidence |
| --- | --- | --- | --- |
| 1 | 2026-12-20 cutoff may be too late | **Confirmed, and worse** | First notice day for GC Dec-2026 is 2026-11-30; the old cutoff was 20 days *after* it |
| 2 | Sweep must close inside both boundaries | **Confirmed bug** | Code only tested the swept side; a close above the range high passed as bullish |
| 3 | Candle construction and timezone | **Confirmed bugs** | Bar counting only; negative local hours dropped; DST transition hour wrong |
| 4 | Entry and lifecycle timing | **Confirmed bug** | Entered on the next available bar, not the expected slot |
| 5 | P&L separation | **Confirmed bug** | "Gross" already included slippage, and slippage was never deducted |
| 7 | Extended-hours data missing | **Claim holds**; cause still unknown | Raw callback is byte-identical to the export, so nothing was filtered locally |

### Claim 1 — cutoff

Exchange and broker facts, checked separately:

- **First notice day (exchange):** the last business day of the month preceding the delivery
  month → **2026-11-30** (Monday) for GCZ6. Cross-checked against a third-party expiry
  calendar that lists 2026-11-30 as first notice and 2026-12-29 as last trading day.
- **Last trading day:** 2026-12-29, 12:30 ET (COMEX floor close for gold).
- **IBKR long close-out:** IBKR prohibits delivery on physically settled futures and may
  liquidate positions that are not closed before its published close-out deadline. That
  deadline is published per contract on IBKR's Futures Close-Out Policy page, which is
  JavaScript-rendered and could not be read headlessly. **I could not verify your
  account-specific deadline**, and I did not guess one.
- **Timestamps:** exchange dates are ET; the old report never stated a timezone.

The corrected run therefore uses a documented conservative convention: **no entry on or
after 2026-11-20 and forced exit before it** — seven business days before first notice day,
which is earlier than any plausible broker deadline. It is a research convention, not
IBKR's rule. If you trade this for real, read the exact date and time from IBKR's page.

Effect: one eval setup is now skipped by the cutoff; dev is unaffected because it ends
2026-10-27.

### Claim 2 — signal rules

Before:

```cpp
if (swept_low && sweep.close > range.low) direction = 1;          // only the swept side
else if (swept_high && sweep.close < range.high) direction = -1;
```

A candle that traded below the low and then closed **above the range high** — a clean
breakout, not a reclaim — was accepted as a bullish CRT entry. After:

```cpp
if (swept_low && sweep.close > range.low && sweep.close < range.high) direction = 1;
else if (swept_high && sweep.close < range.high && sweep.close > range.low) direction = -1;
```

Equality and double-sided cases are now explicit tests (`test_equality_is_not_inside`,
`test_double_sided_sweep_skipped`, `test_close_above_range_high_is_not_bullish`,
`test_close_below_range_low_is_not_bearish`).

### Claim 3 — candles and timezone

Two separate defects, both confirmed:

1. **Only a bar count was checked.** A duplicate bar plus a missing bar produced
   `bars_seen == expected` and a candle that looked complete. Now every expected 15-minute
   slot must arrive exactly once and on the grid; duplicates and off-grid timestamps are
   counted and rejected, and incomplete candles carry the reason.
2. **Hand-coded timezone arithmetic.** The old code added −4 or −5 hours to the UTC hour and
   then normalised only *positive* overflow, so any bar between 00:00 and 04:59 UTC produced
   a negative local hour and was silently dropped instead of landing on the previous day.
   The offset was also chosen from the **UTC** date, so the one-hour window around each DST
   change was wrong. Both are now handled by the system tz database
   (`TZ=America/New_York`, glibc `localtime_r`), and tested at the exact instants:

   | UTC | Expected ET | Why |
   | --- | --- | --- |
   | 2026-03-08T06:59:59Z | 01:59 EST | last minute before the jump |
   | 2026-03-08T07:00:00Z | 03:00 EDT | clocks skip 02:00 |
   | 2026-11-01T05:59:59Z | 01:59 EDT | last minute before the fall back |
   | 2026-11-01T06:00:00Z | 01:00 EST | the hour repeats |
   | 2026-01-06T03:00:00Z | 2026-01-05 22:00 | crosses midnight backwards |

   `std::chrono` tzdb segfaults on this toolchain (probed), so glibc's tz files are used
   instead — still a real database, not a hand-coded rule.

Session calendar: the runner has no exchange calendar. It pairs only consecutive declared
buckets, and an absent session breaks the chain, so no pair spans a missing day. Holidays
and early closes are handled the same way — an incomplete bucket is dropped, never spliced.

### Claim 4 — entry and lifecycle

Before: `bar_index.upper_bound(sweep.end_utc)` — the next bar of any timestamp, so a signal
whose expected entry bar was missing entered hours later. After: the expected timestamp is
computed (`sweep.end + 15m`) and must exist exactly; otherwise the setup is counted in
`skipped_missing_entry_bar` and dropped. Skips caused by an already-open position are now
counted too (`skipped_open_position`) instead of vanishing.

### Claim 5 — P&L and reports

Before: `gross` was computed from the **already-slipped** fills, `slippage` was calculated
but never subtracted, and `net = gross − fees`. So the report's "Gross P&L" was really
post-slippage, pre-fee, and "net" quietly ignored slippage entirely.

After, per trade: `gross_pre_slippage` (reference prices), `slippage` (pre-slip minus
as-filled), `fees`, and `net = gross_pre_slippage − slippage − fees`. Asserted in
`test_slippage_decomposition` and reconciled against the equity curve in
`test_reconciliation`.

Execution prices now round **adversely** to the tick (buy up, sell down) instead of to
nearest, and the report says so. The equity file is labelled `realised_equity` and the
report states explicitly that it is realised trade P&L on a fixed notional — not an account
balance, with no margin and no mark-to-market.

## 2. Data diagnosis

Re-issued the exact original request on the same contract:

```
Raw callback: 1274 bars, first 2026-09-29T13:30:00Z, last 2026-11-27T17:45:00Z
Saved 1274 bars
sha256 1c22fa96…d3dde5   (both the re-fetch and the original export)
```

The raw callback count equals the exported row count and the hashes match byte for byte, so
**nothing is being filtered or rewritten locally**. The contract's own trading hours from
IBKR are `1800-1700` ET (23 hours), while every returned bar sits between 09:30 and 16:45 ET.
What remains unknown is *why*: the request sets `useRTH=0` and IBKR returns no permission
error, so the two candidates are an inactive CME entitlement or a paper-account limitation.
Distinguishing them needs IBKR's market-data page, which requires your login. No IBKR chart
comparison was possible headlessly; that check is still outstanding.

## 3. Old versus corrected results

Same data, same 2-hour definition, same costs. Only engine corrections differ.

| Run | Old (c88080a) | Corrected | Difference |
| --- | --- | --- | --- |
| Dev, 17 trades | net −$4,875 | **net −$5,215** | slippage now charged; adverse rounding |
| Dev, gross | −$4,790 ("gross") | **−$2,070 pre-slippage** | the old figure was post-slippage |
| Dev, stress | net −$16,390 | **net −$16,530** | same |
| Eval, trades | 13 | **12** (1 skipped by cutoff) | cutoff correction |
| Eval, net | −$9,185 | **−$12,990** | cutoff + costs |
| Eval, stress | net −$17,390 | **−$20,540** | same |
| 4H frozen definition | 0 pairs, 0 trades | **0 pairs, 0 trades** | unchanged: data still blocks it |

Dev setups are 17 in both versions, so the stricter signal rule did not change the count on
this sample — its effect appears in the price accounting, not the trade list. The corrected
numbers are uniformly worse, which is what a properly costed backtest should do to a
losing strategy.

Independent check of trade 0 in `dev-base`: range 09:30 ET, 8/8 bars, high 4,207.2 low
4,180.0; sweep 11:30 ET, 8/8 bars, low 4,176.3, close 4,180.7 (inside both bounds); entry at
the 4,180.4 open → 4,181.3 filled; stop 4,176.3, exit 4,175.4. Recomputing from raw bars
reproduces pre-slippage −$410, slippage $180, fees $5, net −$595 exactly.

## 4. Tests

`crt_clock_tests`, 15 scenarios: DST and midnight instants, duplicate/off-grid/missing bars,
a missing full session, closes outside the opposite boundary, equality, double-sided sweeps,
missing entry bar, overlapping positions, cutoff for long and short, adverse tick rounding,
slippage decomposition, gap-through-stop, reconciliation.

- `ctest --preset dev`: **11/11 passed**
- `ctest --preset ibkr`: **12/12 passed**
- Existing `crt_tests` (26 checks) and `daily_signal_tests` (121 checks) unchanged and passing.
- **Not validated by these tests:** IBKR order handling, fills, margin, rolls, delivery.
  Everything here is offline simulation.

## 5. Unresolved risks

- IBKR's per-contract close-out deadline for GCZ6 is unverified; the cutoff is a
  conservative convention, not a broker rule.
- First notice day is rule-derived plus a third-party cross-check, not read from a CME
  circular.
- The reason for the truncated session is unknown, so the 4H experiment stays blocked.
- Sample size is 21 + 22 sessions, one contract, one market: even a positive result here
  would be weak evidence.
- Realised-only P&L overstates smoothness: no mark-to-market, so intra-trade drawdowns are
  invisible.

## 6. Reproduce

```sh
export LD_LIBRARY_PATH=/home/rish/.local/share/algotrader/build-deps/usr/lib/aarch64-linux-gnu

# data (read-only, unchanged, hash-verified)
./build/ibkr/algotrader ibkr-history --instruments results/crt-gold/gc-2026-12.inst.csv \
  --port 4002 --account <paper-id> --bar-size "15 mins" --duration "2 M" --extended-hours \
  --output results/crt-gold-v2/refetch

# corrected 2-hour runs
for r in "dev-base dev 2.50 2" "dev-stress dev 5.00 10" "eval-base eval 2.50 2" "eval-stress eval 5.00 10"; do
  set -- $r
  ./build/dev/algotrader crt-clock --instruments results/crt-gold/gc-2026-12.inst.csv \
    --data results/crt-gold/gc-2026-12-15m-$2.csv --quantity 1 --fee $3 --slippage-bps $4 \
    --buckets 570:120,690:120,810:120,930:120 --max-hold-hours 4 --cutoff 2026-11-20 \
    --output results/crt-gold-v2/$1
done

# frozen 4H definition (still blocked by data coverage)
./build/dev/algotrader crt-clock --instruments results/crt-gold/gc-2026-12.inst.csv \
  --data results/crt-gold/gc-2026-12-15m/bars.csv --quantity 1 --fee 2.50 --slippage-bps 2 \
  --buckets 570:240,810:240 --cutoff 2026-11-20 --output results/crt-gold-v2/4h-check
```

Data hashes: full export `1c22fa966970324dc6cd69355ff3fa46695cad2c5292e27efef57ad510d3dde5`,
dev split `54f95131321117fb8ebd09182dcbbe748c3aea5829c22296e36849369af96444`, eval split
`ec3cb206a9633e2eeceac04db531cf41c46b716248ad6ccdca9d4b619ef29aad`.
