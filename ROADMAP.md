# Implementation roadmap

The immediate objective is one understandable C++ strategy running automatically in a paper
account with observable orders, correct accounting and restart behavior. A running program is a
software milestone; profitability requires separate evidence and has no dependable deadline.
This file is a proposed engineering plan, not a claim that any skeleton method is operational.

## Scope and event contracts

Start with one stock/ETF, one currency, long-only positions, market orders and completed bars.
Reject unsupported order types explicitly until implemented. Internal integer ticker IDs are
not automatically IBKR contract IDs. Resolve and store that mapping before requesting data.
Options, leverage, short selling, multi-currency accounting and live operation are later work.

The intended flow is:

```text
bar -> strategy intent -> risk approval -> durable intent/reservation -> broker submission
broker execution -> deduplication -> portfolio accounting -> durable checkpoint
broker status -> order state/reservation update (never an accounting fill)
commission -> execution match -> commission delta -> portfolio accounting
snapshot -> reconciliation -> enable or halt strategy
```

Keep application state on one event-loop thread. SDK callbacks should enqueue owned events;
never mutate portfolio state from a reader thread. Define bounded queue overflow behavior and
callback lifetimes. Every uncertain submission remains reserved until reconciliation resolves it.
Persist execution IDs, applied commission totals, portfolio and order state together. The current
members are starting points; extend checkpoint schemas and APIs when implementing these contracts.

## 1. Establish the accounting baseline (2–4 focused hours)

- Run the existing build and demo; retain its exact-output regression test.
- Add meaningful unit tests for fractional buys/sells, full liquidation, fees, invalid/nonfinite
  inputs, insufficient cash, missing marks and no state mutation after rejection.
- Implement const portfolio views. Document average-cost policy and the distinction between trade
  cost basis and fee expense. Define price validation and rounding tolerances.
- Implement execution application separately from pre-trade validation: confirmed broker fills
  must be accounted for even when local cash or risk assumptions were wrong.
- Define commission corrections and account currency handling before applying them.

**Acceptance:** independently calculated cash, holdings and marked equity agree; rejected requests
leave state untouched. Tests cover duplicate execution handling once FillProcessor is implemented.

## 2. Connect read-only to paper TWS (2–5 hours)

- Obtain the current official IBKR TWS API C++ sample and follow its platform build instructions.
  Verify SDK requirements against current official documentation when doing this milestone.
- Configure the paper TWS session and API connection settings; use its configured host/port and
  an explicit client ID. Keep read-only enabled. Do not guess account identity from a port.
- Build/run the sample separately first. Log connection, handshake, errors and clean shutdown.
- Then implement IbkrBroker's PImpl, SDK build integration, reader ownership and event queue.
  Keep SDK dependencies optional so offline replay still builds independently.

**Acceptance:** repeated connection/disconnection succeeds; wrong port and stopped TWS produce
clear diagnostics; no order is submitted. A connected socket alone does not set `ready()` true.

## 3. Read positions and one instrument's prices (3–6 hours)

- Request account/position snapshots and wait for all completion markers.
- Resolve exactly one unambiguous stock/ETF contract. Store contract mapping and currency.
- Subscribe to market data; log timestamps, live/delayed mode and permission errors.
- Aggregate completed bars using an explicit session clock/timezone policy; never treat a quote
  callback as a completed bar. Decide how missing intervals and market closures behave.
- Compare displayed account/position information and prices to paper TWS.

**Acceptance:** account identity and snapshots match TWS; incomplete snapshots and stale or
unavailable data block decisions. Disconnects do not silently leave the application ready.

## 4. Implement deterministic historical replay (4–8 hours)

- Implement CsvDataFeed: `date,ticker,open,close`, ISO dates, strict numeric validation,
  useful line-number errors, chronological ordering and deterministic reset.
- Implement BrokerSim market orders with next-bar-open execution, adverse slippage and fees.
  Check available exposure through the same risk/order path used for paper trading.
- Implement Backtester ordering: previous orders at open, execution accounting, close marks,
  strategy evaluation, risk approval, queue orders for a later bar.
- Define final pending-order policy, missing bars and repeated runs. Start with one instrument;
  multiple instruments require timestamp batching and complete mark maps.

**Acceptance:** a hand-calculated fixture matches every fill and cash/equity value; changing future
bars cannot change earlier decisions. No same-close fill or fabricated final-bar liquidation.

## 5. Add one strategy and performance output (3–6 hours)

- Implement moving-average warmup, rolling state, crossover detection and reset.
- Size sell intents using actual holdings; avoid repeated outstanding buys through order/risk state.
- Record initial and subsequent marked equity. Implement total return, drawdown and CSV output;
  specify empty-series/zero-equity behavior. Add hand-calculated report tests.
- Include run configuration, data identity, fee/slippage assumptions and a simple benchmark.

**Acceptance:** identical inputs reproduce identical orders and reports. A no-trade run, a falling
market and a known drawdown example produce expected results. No claim of edge from one replay.

## 6. Implement order lifecycle and protective limits (5–10 hours)

- Implement stable client keys, broker ID mapping and order transitions. Preserve unknown and
  pending-cancel exposure. Cancellation requests do not release reservations immediately.
- Handle rejection, partial execution, cancellation after partial execution and late callbacks.
- Implement FillProcessor execution-ID deduplication and commission-before/after-fill buffering;
  apply corrected commission totals as deltas, including after restart.
- Risk-check finite sizes/prices, freshness, maximum order/position notional, cash buffer,
  fees/slippage and all outstanding reservations. Default halted until reconciliation succeeds.
- Extend limits to daily loss, order rate and concentration before unattended operation. Specify
  what halts new orders, what cancels existing orders and what requires manual review.

**Acceptance:** scripted duplicates and reordered events never double-count cash or release
exposure early. Unknown submission outcomes halt/reserve rather than blindly retrying.

## 7. Submit one manually triggered paper order (4–8 hours)

- Finish IBKR order translation only for the supported contract/order scope.
- Explicitly opt out of read-only mode for the verified paper account; inspect the resolved
  contract and intended size. Trigger one order manually before connecting automatic decisions.
- Observe statuses, executions, commission and cancellation in both logs and TWS.
- Exercise rejection and partial-fill paths with deterministic injected events where the paper
  environment does not reproduce them reliably.

**Acceptance:** local order state and accounting reconcile with broker truth; a submitted request
alone never changes cash/holdings. Every event can be traced to a durable intent and execution ID.

## 8. Make restarts and reconnects safe (6–12 hours)

- Implement a versioned journal and checkpoints with checksums/sequence validation and a defined
  OS durability policy. Journal intent durably before submit; fail closed on disk errors.
- Replay local state without resending orders. Query broker open orders, positions, cash and
  execution history, including external/manual activity. Handle limited history windows explicitly.
- Persist broker/client ID mappings and advance allocators safely. Match ambiguous submissions
  with broker evidence; require manual resolution when identity cannot be established.
- Detect disconnects/stale feeds, stop new decisions, reconnect with bounded backoff, resubscribe
  and complete reconciliation before resuming. Add shutdown signal handling and queue draining.

**Acceptance:** restart before send, after send but before acknowledgement, after partial fill,
and before commission delivery causes no duplicate order or accounting update. Corrupt journals,
missing history and unexplained external trades block automation with actionable diagnostics.

## 9. Run automatically in paper and observe (4–8 hours setup, then observation)

- Connect TradingApp to the single strategy through the risk, journal and order layers.
- Provide explicit configuration validation and modes: read-only, manual paper, automatic paper.
- Add structured logs, health/last-data timestamps, reconnect counts, outstanding exposure,
  a stop control and an end-of-session reconciliation report.
- Run supervised sessions first, then repeated sessions with daily reconciliation. Retain decision
  inputs and rejected-intent reasons. Record incidents and add regression fixtures for each.
- Document setup, startup/shutdown, recovery and what to do when the broker disagrees with local state.

**Acceptance:** daily positions/cash/orders agree with broker records within documented rounding
and timing rules; restarts are repeatable, halts are observable, and discrepancies are resolved
before another automated session. This milestone authorizes no live-money deployment.

## Later research and scope expansion

A reliable execution system does not establish a profitable strategy. Separately address data
quality, adjusted prices/corporate actions, survivorship bias, out-of-sample evaluation, parameter
selection bias, realistic costs/liquidity, turnover and benchmark comparison. Keep research
configuration separate from execution permissions. Paper results do not establish live fill quality.

Options require additional contract identity (expiry, strike, right), multipliers, tick/lot rules,
chain selection, liquidity checks, expiry/exercise/assignment accounting and appropriate scenario
limits. Do not reuse stock-share accounting without designing these explicitly. Multi-currency,
shorts and live deployment each need separate reconciliation and operational acceptance criteria.

## Pace at 30–60 minutes per day

Planning estimates above total roughly **33–67 focused hours**, excluding account setup delays and
observation sessions: approximately **5–10 weeks at an hour daily**, or **10–20 weeks at half an hour**.
They are estimates, not deadlines. A basic connection can arrive much earlier; the complete recovery
and replay scope takes longer than a minimal strategy demonstration.

For each session: spend 5 minutes choosing one observable outcome, 20–45 minutes implementing or
learning, and 5–10 minutes testing and noting the next step. Avoid implementing every header at once.

First week: build the demo and verify accounting; prepare paper TWS; build the official C++ sample;
connect read-only and print one response; repeat after a clean restart. Next, read positions and one
instrument before attempting any order. You can complete phases 2–3 before the full offline replay.
