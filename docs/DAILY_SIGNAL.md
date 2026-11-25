> **Execution update:** The daily mode can submit a supervised paper order when explicitly enabled. It rechecks position and cash immediately before submission; any change requires reconciliation. This snapshot cannot eliminate changes after the check.

# Daily signal report (read-only)

The first step from research to paper trading: a report that states what the frozen
SMA rule wants for one instrument, next to what the paper account actually holds.
It is read-only by default. Supervised paper execution requires both `--execute`
and `--enable-orders yes`.

`docs/RESEARCH.md` describes the strategy and the data. This page covers the
operational command.

## Prerequisites

- A completed build. `build/dev` is enough for the offline path; `build/ibkr` is
  needed to talk to TWS/IB Gateway (see [IBKR.md](IBKR.md)).
- For the live path: IB Gateway or TWS running and logged into the **paper**
  session, socket API enabled, and the port it listens on. The report refuses any
  account identifier that does not start with `DU`.
- An instrument catalogue row for the symbol (for AAPL, `examples/ibkr-aapl.csv`).

## Offline replay (no broker, no network)

```sh
./build/dev/algotrader daily-signal \
  --instruments examples/ibkr-aapl.csv \
  --bars results/research-aapl-2011-2025/bars.csv \
  --window 100 --quantity 1 \
  --as-of 2025-12-31 \
  --output results/daily-signal
```

Use this to reproduce a past decision exactly. `--as-of` waives the freshness
check only; the session must still be a completed one. With `--bars` there is no
broker session, so the position is reported as unknown and nothing is proposed —
that is the intended behaviour, not a failure.

## Live read-only report

```sh
./build/ibkr/algotrader daily-signal \
  --instruments examples/ibkr-aapl.csv \
  --host 127.0.0.1 --port 4002 --account DU_YOUR_ACCOUNT \
  --window 100 --quantity 1 \
  --output results/daily-signal
```

This opens a read-only session, requests the account snapshot and then `2 Y` of
daily TRADES bars with the same request shape the historical export uses. It writes
the report and disconnects. Nothing is subscribed to and no market-data
entitlement is changed.

If the snapshot cannot be completed (unknown external contract, timeout,
disconnect), the run reports the broker state as **unknown** and proposes nothing.
If a report is still wanted in that situation, pass `--bars FILE` as well: the
signal comes from the file while the broker state is recorded as unconfirmed.

## What the report contains

`results/daily-signal/<session>_<utc-stamp>/` with:

| File | Contents |
| --- | --- |
| `signal.md` | Human-readable report: decision, data quality, policy, limitations |
| `signal.json` | The same fields, machine-readable, including `"submitted": false` |
| `bars-used.csv` | The decision session plus the 100 sessions it averaged, verbatim |
| `command.txt` | The exact command that produced the report |

A new directory is created for every run and the writer refuses to reuse an
existing one, so earlier evidence is never overwritten.

Sample (offline replay above):

```
# Daily signal report — AAPL

- Run (UTC): 2026-11-25T03:12:23Z
- Mode: read-only signal report. No order was submitted, modified or cancelled.
- Status: **BLOCKED - broker state unknown**

## Decision

| Item | Value |
| --- | --- |
| Decision session (completed) | 2025-12-31 |
| Close | 271.86 USD |
| Prior 100-session average | 256.7701 USD |
| Averaged window | 2025-08-08 to 2025-12-30 |
| Signal | above-average |
| Strategy target | 1 share |
| Broker position | unknown — offline mode ... |
| Proposed action | none |
```

## Rules the report enforces

- **Prior window.** The average uses the 100 completed closes *before* the decision
  session; the decision close is excluded. It is computed by `MovingAverageStrategy`
  itself, so the report cannot drift from the backtester.
- **Signal.** Close above the average targets `--quantity` shares; below targets 0;
  **equal retains the current position** and asserts no target. When the position is
  also unknown, nothing can be proposed at all.
- **Warmup.** Fewer than `window + 1` completed sessions fails before any decision.
- **Data quality.** Daily `YYYY-MM-DD` sessions only, one instrument, strictly
  increasing dates (duplicates and unsorted rows fail), finite positive prices.
- **Session completion.** A bar dated today is accepted only after 22:00 UTC (US
  regular close plus buffer). A still-forming or future bar is never the decision
  bar, and the run fails rather than silently falling back.
- **Freshness.** The newest completed session must be within `--max-stale-days`
  (default 5) calendar days.
- **Reconciliation.** Any open order, or a position that is neither flat nor exactly
  the strategy quantity, blocks the proposal with the reason stated.

## Decision ledger

Every run appends one row to `results/daily-signal/ledger.csv` (override with
`--ledger`):

```
decided_at,session,symbol,state,target,broker_shares,open_orders,status,side,quantity,limit_price,report_dir
```

`session` is the decision session, so the same day can never be traded twice by
accident. A row with `status=INTENT` is written **before** an order is sent and the
outcome row after it, so a crash mid-submission still blocks a repeat. The ledger is
never rewritten; a malformed ledger fails closed (the run refuses rather than
guessing).

## Scheduling

`packaging/algotrader-daily-signal.{service,timer}` run the report once per weekday at
22:10 UTC. They are **templates and are not installed**. To use them:

```sh
mkdir -p ~/.config/algotrader
printf 'ALGO_ACCOUNT=DU1234567\n' > ~/.config/algotrader/daily.env   # your paper account
chmod 600 ~/.config/algotrader/daily.env
mkdir -p ~/.config/systemd/user
cp packaging/algotrader-daily-signal.* ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now algotrader-daily-signal.timer
```

The run itself handles the timing instead of a sleep loop: `--wait-until-close`
blocks until 22:00 UTC in 30-second steps (Ctrl+C cancels), then evaluates. It exits
without a report when today is not a US equity trading day (weekends and NYSE full
closures, computed from holiday rules rather than a hand-maintained table; early
closes are treated as normal sessions).

## Supervised paper execution (opt-in, off by default)

Nothing submits unless **both** `--execute` and `--enable-orders yes` are given, the
session is live (not `--bars`), and every check below passes:

1. The proposal is `ACTIONABLE` (signal ok, broker confirmed, no open orders, position
   flat or exactly the strategy quantity).
2. A limit price exists: close ± `--limit-band-bps` (default 25), rounded adversely to
   the instrument tick. Buys only round up, sells only down — it never chases a gap.
3. The session is not already in the ledger.
4. `RiskManager` approves the order against the broker cash, position and pending
   orders (`--max-order`, `--max-position`, `--cash-buffer`).

```sh
export LD_LIBRARY_PATH=/home/rish/.local/share/algotrader/build-deps/usr/lib/aarch64-linux-gnu
./build/ibkr/algotrader daily-signal \
  --instruments examples/ibkr-aapl.csv --port 4002 --account DU_YOUR_ACCOUNT \
  --window 100 --quantity 1 \
  --enable-orders yes --execute --seconds 120 \
  --output results/daily-signal
```

An `INTENT` ledger row is appended before submission, the order lifecycle is journalled
to `<report>/execution.journal`, and the outcome (`filled`, `cancelled`, `rejected`,
`uncertain`) is appended afterwards. An `uncertain` outcome requires manual
reconciliation before another run; it is never retried automatically.

This path requires the **paper** account's Read-Only API setting to be off. The
reporting session always connects read-only.

## This versus automated paper trading

| | This report | `ibkr-paper` |
| --- | --- | --- |
| Orders | opt-in guarded paper execution | sends after `--enable-orders yes` |
| Input | completed daily bars | five-second live bars |
| Warmup | 100 sessions | about eight minutes |
| Decision | stated for a human | applied automatically |
| Journal | report directory | `EventJournal` with recovery |
| Risk controls | `RiskManager` on opt-in execution | `RiskManager`, session loss limit |

`ibkr-paper --strategy sma --window 100` is **not** this strategy: its 100 bars are
100 five-second bars. Running the daily rule automatically needs a scheduler that
waits for a completed session, a persistent decision record, order recovery and a
fresh-price execution policy. Those are the next milestones; this report exists so
each of them can be checked against a human-verifiable signal first.

## Current limitations

- US equity regular hours only. Futures (corn) need a session calendar, contract
  rolls and multiplier/margin handling before this rule transfers.
- Prices are split-adjusted but not dividend-adjusted: a price-only study.
- The report states intent, not execution economics: no slippage, commission or
  fill assumption is applied.
- Broker state is only as good as the snapshot that run received.
- The execution path has been unit-tested against the recorded state machine but has
  not been exercised against a live Gateway in this iteration. Run it supervised once,
  with `--quantity 1`, before trusting it unattended.
