# AlgoTrader Basic C++ Architecture

## Goals
- Keep trading logic, execution simulation, and risk/accounting separate.
- Make strategy development fast without changing core engine code.
- Support backtesting first, then extend to paper/live trading.

## High-Level Layers
1. **Data Layer**
   - Loads market data (CSV/API adapters).
   - Normalizes data into a shared `Bar` model.
2. **Engine Layer**
   - Drives the event loop over bars/ticks.
   - Coordinates strategy, broker simulator, and portfolio updates.
3. **Strategy Layer**
   - Generates signals/orders from market data and indicators.
   - Should be pluggable so multiple strategies can be tested.
4. **Execution Layer**
   - Simulates order handling, slippage, fees, partial fills.
   - Emits fills back to the engine.
5. **Portfolio & Risk Layer**
   - Tracks positions, cash, PnL, and exposure.
   - Applies risk limits before sending orders.
6. **Reporting Layer**
   - Produces performance stats (returns, drawdown, Sharpe) and logs.

## Core Domain Models
- `Bar`: OHLCV + timestamp.
- `Signal`: buy/sell/flat intent with symbol and strength.
- `Order`: side, quantity, type, price constraints.
- `Fill`: execution result including fee/slippage.
- `Position`: current holdings and average cost.
- `PortfolioSnapshot`: account state at each step.

## Suggested Folder Structure
```text
docs/
  ARCHITECTURE.md
  TODO.md
include/
  core/        # Bar, Signal, Order, Fill, Position
  data/        # IDataFeed, CsvDataFeed
  engine/      # BacktestEngine, Event types
  strategy/    # IStrategy + concrete strategies
  execution/   # IBroker, BrokerSim
  portfolio/   # Portfolio, RiskManager
  analytics/   # Metrics, report generation
src/
  main.cpp
  ...implementation files...
tests/
  unit/
  integration/
```

## Runtime Flow (Backtest)
1. Data feed emits next `Bar`.
2. Engine updates indicators/state.
3. Strategy receives context and emits `Signal`/`Order`.
4. Risk manager validates or modifies order size.
5. Broker simulator executes and returns `Fill`.
6. Portfolio updates cash/positions/PnL.
7. Metrics recorder stores equity curve and stats.

## Extension Path
- Multi-asset support.
- Additional order types (limit/stop).
- Transaction cost models.
- Walk-forward optimization.
- Live broker adapter reuse of same interfaces.
