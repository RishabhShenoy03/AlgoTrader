# Roadmap after the working engine

## Implemented

- C++23 CLI, CMake presets, standalone header checks and automated tests.
- Strict chronological CSV input with inclusive date filters and multi-instrument timestamp batches.
- Stock/ETF, crypto spot, futures and long-option metadata/accounting.
- Market and opening-price limit simulation with fees, adverse slippage, tick rounding and optional partial fills.
- Pending-order reservations, execution deduplication, delayed/corrected commissions and risk limits.
- SMA, breakout, mean-reversion and buy/hold strategy baselines behind a reusable Strategy interface.
- Equity, drawdown, executions, decision logs and run reports.
- Optional official C++ IBKR adapter: paper-account verification, contract validation, read-only snapshots,
  real-time bars, historical export, order/cancel translation and execution/commission callbacks.
- Supervised paper application with durable event journal, stale-data stop, session loss stop and
  explicit refusal to blindly restart an existing session.

## Your next steps

1. Run the example backtest and inspect individual trades; change costs and verify their effect.
2. Import real data with correct contract metadata. Keep different market calendars separate initially.
3. Evaluate one strategy on development and holdout date ranges and compare it with a passive baseline.
4. Set up paper TWS and complete the read-only checks in `docs/IBKR.md`.
5. Export a small historical dataset, compare several bars to TWS and replay it.
6. Run a small supervised paper session; reconcile every order, execution, fee and position.
7. Retain incident records and add regression tests before longer sessions.

## Work required for broader research

- Real OHLCV, bid/ask spreads, liquidity/volume participation and realistic exchange calendars.
- Corporate actions, dividends, adjusted-price policies and survivorship-safe universes.
- Multi-currency accounting and explicit FX conversion.
- Derivative rolls, expiry settlement, option exercise/assignment, Greeks and underlying event handling.
- Short selling, borrow costs, exchange/broker margin rules and maintenance-margin liquidation policy.
- Portfolio allocation, per-instrument strategy configuration, benchmark normalization, trade-level PnL,
  walk-forward experiments and frequency-aware statistical metrics.
- Historical pagination/provider adapters, dataset versioning and richer import formats.

## Work required before unattended operation

- Recoverable account checkpoints including executions and commission totals.
- Durable local/broker ID mappings and execution-history reconciliation across crash windows.
- Safe handling of broker corrections, account-wide external activity and restart with existing positions.
- Reconnect/resubscribe logic that enables trading only after complete reconciliation.
- Realistic order-rate controls, daily limits, alerting, monitoring and operator recovery procedures.
- Contract market-rule tick tables, trading-session handling and overnight futures settlement reconciliation.
- Extend the real-SDK mock-TWS suite to contract resolution, data and order/execution replay, plus supervised validation against the actual paper account.

This release intentionally halts on unsupported states. It is a useful research and supervised paper
foundation; these remaining items must not be inferred from a successful compile or simulated return.
No backtest or example strategy establishes a profitable edge, and paper fills do not prove live liquidity.
