# CRT on gold futures: rules, data and result

> **Correction (2026-11-28).** This report is preserved as historical evidence, but three
> of its statements are wrong and were identified by an independent review:
>
> 1. **The 2026-12-20 close-out cutoff is unsafe.** First notice day for GC December 2026 is
>    **2026-11-30**, so the cutoff sits 20 days *after* the contract can go to delivery. The
>    corrected runs use 2026-11-20 and never hold through it.
> 2. **"Gross P&L" already included slippage**, and net P&L ignored slippage entirely;
>    slippage was computed and then dropped. The corrected engine reports pre-slippage,
>    slippage, fees and net separately.
> 3. **The signal rule was too loose**: a candle that swept one side and closed beyond the
>    *opposite* boundary was accepted as a reclaim.
>
> The numbers below are the original, uncorrected results. Corrected figures, the full
> claim-by-claim verdict and the new tests are in
> [CRT_GOLD_AUDIT.md](CRT_GOLD_AUDIT.md). Nothing here should be quoted as a measurement of
> the strategy without that document.

Research experiment, 2026-11-28. Code revision: the commit that adds this file, on branch
`feat/crt-gold`, based on `7c399f5`.

**Verdict: negative, and partly blocked.** The frozen clock-aligned 4H definition could not
be tested at all — this account's futures history contains only the 09:30–17:00 ET slice of
a 23-hour contract, which yields one complete 4-hour candle per session and therefore zero
range→sweep pairs. A clearly-labelled substitute (2-hour candles on the available window,
chosen before any result was seen) was testable and lost money in both periods, before and
after costs.

## 1. What the project already had, and what it did not

Verified in the code at `7c399f5`, not from documentation:

| Capability | General backtester | Wired into the CRT runner |
| --- | --- | --- |
| OHLC bars, 15-minute timestamps | yes | yes (since `73ea8c2`) |
| Long and short positions | yes | yes, in the simulator |
| Futures multiplier, tick size | yes (`Instrument`, `Portfolio`) | **no** — `CrtBacktest` sized in "shares" |
| Commissions and slippage | yes | yes, per side |
| Stop/target fills, gaps, ambiguity | n/a | stop-first on ties; gap fills fixed in `7c399f5` |
| Session breaks, holidays, DST | no | **no** — candles were N bars, not clock windows |
| Margin, expiry / close-out | expiry only | **no** |
| Continuous contracts, rolls | no | no |

Reusable: the bar format, the CSV feed, the CRT state machine's shape, the audit script.
Must change: candle construction (clock-aligned), contract economics (multiplier, tick),
and a close-out cutoff. Unsupported: rolls, continuous contracts, margin accounting,
delivery mechanics.

## 2. Contract

`GC` (COMEX gold), **December 2026**, con_id **462941472**, resolved through IBKR
`reqContractDetails` — not assumed. Specification as returned by IBKR:

- symbol `GC`, secType `FUT`, exchange `COMEX`, currency `USD`, trading class `GC`
- expiry `20261229`, multiplier **100**, minimum tick **0.1** (⇒ $10 per tick, $100 per $1 move)
- timezone `US/Eastern`; trading hours `1800-1700` next day, i.e. 23 hours with a one-hour
  break; Saturday closed
- physically delivered (COMEX gold standard), so the run uses a close-out cutoff of
  **2026-12-20**, nine days before first notice/last trade, and never enters on or after it

MGC (E-micro, multiplier 10) was available and was not used: GC matched the brief and the
day's data depth was identical.

## 3. Data

Requested read-only from the running Gateway:

```
ibkr-history --instruments results/crt-gold/gc-2026-12.inst.csv \
  --bar-size "15 mins" --duration "2 M" --extended-hours --output ...
```

- 1,274 bars, **2026-09-29 → 2026-11-27**, 43 sessions, ticker `1`
- `whatToShow=TRADES`, `useRTH=0`, formatDate=2 (UTC ISO timestamps)
- sha256 of the full export: `1c22fa966970324dc6cd69355ff3fa46695cad2c5292e27efef57ad510d3dde5`
- split by session into dev (21 sessions, 2026-09-29 → 2026-10-27, sha256 `54f95131…96444`)
  and eval (22 sessions, 2026-10-28 → 2026-11-27, sha256 `ec3cb206…29aad`)

**The coverage problem.** Every bar falls between 13:30Z and 21:45Z — 09:30 to 16:45 ET —
while the contract trades 18:00–17:00 ET. The overnight and pre-market session is absent.
Checked three ways: 15-minute bars over 2 months, 15-minute bars over 1 day, and 1-hour
bars over 5 days — all return the same 09:30–16:45 ET window. The request is correct
(`useRTH=0` verified in code and by the CLI flag), and IBKR returns no permission error
beyond advisory 2188. So this is a data entitlement/coverage limit, not a local filter.
Thanksgiving (2026-11-26) is absent entirely; 2026-11-27 is a half day with 14 bars.

Not verified separately: whether the bars match another provider. Only one feed is
available here, so price accuracy is checked structurally (range consistency, ordering,
duplicates, tick alignment) rather than against an independent source.

## 4. Frozen rules

Written before any gold result was examined. Times are **US Eastern**, converted from the
UTC timestamps with the US DST rule (second Sunday of March → first Sunday of November),
which the tests cover on both transition days.

1. Candles are clock windows, never "N bars". A candle counts as **complete** only if it
   received exactly its expected number of 15-minute bars. Incomplete candles are dropped
   and counted, never used.
2. Candle 1 is a complete candle; its high and low are the range.
3. Candle 2 must be the **next bucket of the declared schedule** — same session, or the
   first bucket of the next session when candle 1 is the last bucket. A missing bucket is
   not skipped in favour of a later candle.
4. Bullish: candle 2 trades below candle 1's low and closes back inside the range. Bearish:
   it trades above the high and closes back inside. A candle that pierces **both** sides is
   skipped.
5. Entry only after candle 2 has closed, at the next bar's open. If that open is already at
   or beyond the target, or at or beyond the stop, the setup is skipped.
6. Stop at candle 2's extreme on the swept side; target at candle 1's opposite edge.
7. One position at a time, one contract. Both directions are supported and reported
   separately.
8. Time exit after `max_hold_hours` of elapsed bars (default 8h; the substitute run used
   4h to keep the horizon inside one session).
9. A bar touching both stop and target counts as the **stop**. A bar opening beyond the stop
   fills at the worse open, plus slippage — never at the stop price.
10. Costs are assumptions, not IBKR quotes: base $2.50 per side and 2 bps slippage; stress
    $5.00 per side and 10 bps. One tick at $4,100 is ~2.4 bps, so the base case is about one
    tick of slippage per side.

### How this differs from the AAPL test

The AAPL experiment grouped 16 bars and called it four hours, so 54% of its candles spanned
a market close or weekend and its "4H" was really 4 hours of trading time. It also sized in
shares and ignored contract economics. This one is clock-aligned, rejects incomplete
candles, applies the multiplier and tick, and enforces a close-out date. The two are **not
comparable** as measurements; only the idea (sweep and reclaim) is shared.

## 5. Results

### The frozen 4H definition: not testable

```
crt-clock --buckets 570:240,810:240    (09:30-13:30, 13:30-17:30 ET)
candles 85, incomplete 43, pairs examined 0, setups 0, trades 0
```

Every session produces exactly one complete 4-hour candle (09:30–13:30 ET); the second is
always short because the feed stops at ~16:45 ET. Without a consecutive complete pair there
is no CRT setup. This is a data limitation, not a strategy result.

### Labelled substitute: 2-hour candles, same rules

Chosen *because* the data covers 7.5 hours, which fits three complete 2-hour candles
(09:30–11:30, 11:30–13:30, 13:30–15:30) — decided before any trade was seen, not after
comparing outcomes.

| Period | Cost | Trades | Win rate | Gross | Fees | Net | Max DD (notional) |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Dev (21 sessions) | base | 17 | 35.3% | −$4,790 | −$85 | **−$4,875** | 1.33% |
| Dev | stress | 17 | — | −$16,220 | −$170 | **−$16,390** | 3.91% |
| Eval (22 sessions) | base | 13 | 23.1% | −$9,120 | −$65 | **−$9,185** | 3.23% |
| Eval | stress | 13 | — | −$17,260 | −$130 | **−$17,390** | 5.03% |

Dev detail: average win $1,055, average loss $1,019, **expectancy −$287 per trade**, exits 10
stop / 6 target / 1 time, 2 gap-through-stop cases, 0 ambiguous bars. Start equity $418,710
(1 contract at $4,187.10 × 100).

Eval detail: average win $1,872, average loss $1,480, **expectancy −$707 per trade**, exits 10
stop / 3 target / 0 time, 1 gap-through-stop, 0 ambiguous bars. Start equity $406,490. The
loss rate is worse out of sample than in it, in the same direction in both periods.

**Denominator matters.** −$4,875 is −1.2% of contract notional but **−32.5% of a $15,000
margin assumption** (that margin figure is my assumption, not exchange data). Percentage
returns on a futures account should be read against margin, not notional, and neither number
is a portfolio return.

Costs are not the problem here: gross is negative in both periods, so the setups lose before
commission. Stress costs roughly triple the loss, as expected at 5× the slippage.

## 6. Audit

`scripts/audit_crt.py` logic was applied to the substitute run by hand and by recomputation:

- Trade 0 verified against raw bars: range 4,180.0–4,207.2, sweep low 4,176.3, entry at the
  17:30Z open 4,180.4 → 4,181.2 after 2 bps, stop 4,176.3, exit 4,175.5 (stop less slippage),
  gross −$570, net −$575. Reconciles exactly.
- Every trade's `net` = `gross` − 2 × fee; `Σ net` equals the equity change from `equity.csv`.
- Entry timestamps always equal the bar immediately after the sweep candle's `end_utc`.
- No look-ahead: range, sweep, stop and target are all fixed before the entry bar.

## 7. Tests

- `crt_clock_tests`: **30 checks**, covering DST offsets on both transition days, candle
  completeness and dropped candles, bullish/bearish signals, double-sided sweeps, no entry
  before the signal candle closes, gaps beyond entry/stop/target, stop-first ambiguity,
  multiplier and tick rounding, cutoff, and trade-to-equity reconciliation.
- `ctest --preset dev` 11/11, `ctest --preset ibkr` 11/11, `crt_tests` 26 checks unchanged.
- Not run: any margin simulation, roll logic, delivery mechanics, continuous contracts.
  Nothing here validates those.

## 8. What is missing, and what would fix it

The one blocked input is **full-session 15-minute gold history** (18:00–17:00 ET).

1. **IBKR CME market-data subscription** — the direct fix. This account's futures history
   stops at ~16:45 ET; a real-time/delayed CME bundle would normally include the full
   session. I did not check prices or change anything.
2. **A different provider** with full-session futures history (paid: Databento, Polygon,
   Barchart; free alternatives are usually limited to continuous front-month series and
   short windows).
3. **A market whose available window fits the rule** — but no equity session on this account
   is longer than 7.5 hours either, so the 4H definition stays untestable there too.

Corn (ZC) would need, beyond the above: a resolved con_id and expiry, CME futures
entitlement, multiplier 5,000 bu and 0.25¢ tick ($12.50/tick), tick rounding for limit
prices, margin, and a roll rule. The simulator now has multiplier and tick; it has no roll,
no margin accounting and no delivery handling. Futures trading is not enabled on the
account and this work did not touch it.

## 9. Reproduce

```sh
export LD_LIBRARY_PATH=/home/rish/.local/share/algotrader/build-deps/usr/lib/aarch64-linux-gnu

# 1. contract (read-only)
./build/ibkr/algotrader ibkr-contract --symbol GC --sec-type FUT --exchange COMEX \
  --expiry 202612 --account <paper-id>

# 2. data (read-only; catalogue in results/crt-gold/gc-2026-12.inst.csv)
./build/ibkr/algotrader ibkr-history --instruments results/crt-gold/gc-2026-12.inst.csv \
  --port 4002 --account <paper-id> --bar-size "15 mins" --duration "2 M" --extended-hours \
  --output results/crt-gold/gc-2026-12-15m

# 3. the frozen 4H definition (shows the data gap)
./build/dev/algotrader crt-clock --instruments results/crt-gold/gc-2026-12.inst.csv \
  --data results/crt-gold/gc-2026-12-15m/bars.csv --quantity 1 --fee 2.50 --slippage-bps 2 \
  --buckets 570:240,810:240 --cutoff 2026-12-20 --output results/crt-gold/runs-4h

# 4. the 2-hour substitute, dev and eval
./build/dev/algotrader crt-clock --instruments results/crt-gold/gc-2026-12.inst.csv \
  --data results/crt-gold/gc-2026-12-15m-dev.csv --quantity 1 --fee 2.50 --slippage-bps 2 \
  --buckets 570:120,690:120,810:120,930:120 --max-hold-hours 4 --cutoff 2026-12-20 \
  --output results/crt-gold/runs-dev-base
```

Data files are under `results/` (git-ignored); hashes above let you confirm a re-fetch
matches. No account identifiers are stored in this document or in the committed code.
