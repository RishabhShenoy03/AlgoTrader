# AlgoTrader

A C++23 framework for testing your own long-only strategies against CSV bars and running them in
an IBKR **paper** account. The original portfolio demo remains available as `mainmain`.

Implemented: stocks/ETFs, crypto spot, futures and long options; multipliers/lot sizes; chronological
multi-instrument replay; configurable costs; market and open-crossing limit orders; partial-fill
simulation; order reservations; execution/commission accounting; reports; and an optional official
TWS API adapter with account verification, contract checks, historical export and paper submission.

Example strategies are research baselines, **not demonstrated profitable strategies**. The sample
prices are synthetic and unsuitable for assessing investment performance.

## Build and run

Requires Linux/POSIX, CMake 3.21+, Ninja and a C++23 compiler. No Python runtime or broker SDK is
needed for backtests.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev

./build/dev/algotrader backtest \
  --instruments examples/instruments.csv --data examples/bars.csv \
  --strategy sma --window 3 --quantity 1 --output results/demo
```

Open `results/demo/summary.csv`, `equity.csv`, `trades.csv`, `decisions.csv` and `run.txt`.
Returns include configured commissions and adverse slippage; open positions are marked at the last
close, not artificially liquidated. Final pending orders are cancelled. Repeat runs with a different
output directory to compare parameters. `algotrader --help` lists all arguments.

For VS Code, select the `dev` configure/build/test presets, build `all`, and select `algotrader` as
the launch target. The Testing view discovers the CTest cases. Use terminal commands for CLI arguments
or add them to your debugger configuration.

## Use your own data and strategies

- [Data format and asset models](docs/DATA.md): instrument catalogue, bar schema, futures/options boundaries.
- [Strategy guide](docs/STRATEGIES.md): built-in parameters, a custom strategy example, evaluation workflow.
- [IBKR setup](docs/IBKR.md): SDK build, account setup, read-only check, historical export and paper run.
- [Architecture and limitations](docs/ARCHITECTURE.md): event order, risk controls, journals and recovery.
- [Next milestones](ROADMAP.md): remaining work beyond this implementation.

```sh
# Hold out a later period. Strategy warmup starts at --from; earlier bars are not used.
./build/dev/algotrader backtest \
  --instruments examples/instruments.csv --data examples/bars.csv \
  --from 2026-01-10 --to 2026-01-20 \
  --strategy breakout --window 3 --quantity 1 --output results/holdout

# Compare a passive long-only baseline using the same costs.
./build/dev/algotrader backtest \
  --instruments examples/instruments.csv --data examples/bars.csv \
  --strategy buy-hold --quantity 1 --output results/baseline
```

## Scope you should know

All assets must share the account currency. Futures use user-supplied fixed initial margin and
mark-to-market at each bar close; this is a research approximation, not an exchange settlement engine.
Options use actual option premium bars and contract multipliers. Short positions, option exercise/
assignment, expiry settlement, contract rolls, corporate actions and FX conversion are not modeled.
Holding a derivative into its expiry date stops the run instead of inventing accounting.

IBKR paper mode supports one instrument in a flat, isolated paper account, using five-second TRADES
bars in regular hours. A connection loss, stale data or reconciliation problem halts the session;
there is no automatic resubmission or unattended restart. Existing session journals require manual
broker reconciliation and a new journal. See the architecture document before running paper orders.

The adapter is built and tested separately from the offline engine. Real TWS account, market-data
permissions and paper order validation require a running user session; offline tests cannot certify
those external services.
