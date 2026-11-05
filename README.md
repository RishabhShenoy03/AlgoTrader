# AlgoTrader

C++23 learning project for historical replay and an eventual IBKR paper-trading application.
The existing portfolio/demo works. The trading architecture is a **declaration-only skeleton**:
new `.hpp` files contain contracts and state, and `.cpp` files contain commented method templates.
No IBKR connection, strategy, order routing or recovery is implemented yet.

Build with a C++23 compiler, Ninja and CMake 3.21+ for the version-3 presets:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/mainmain
```

Without presets: `cmake -S . -B build/manual`, then `cmake --build build/manual`.
The demo prints equity 1000, 994 and 1006. Its one-second delays are intentional existing behavior.

`event_core` contains the existing implementation. `trading_skeleton` compiles the unfinished
sources and every public header independently but does not link them into the demo. Instantiating
unfinished classes will fail to link until their definitions are written. Passing compilation
checks does not mean trading functionality works. Move completed implementations into a linked
library as milestones are delivered; avoid compiling the same source into two linked targets.

| Layer | Headers | Responsibility |
| --- | --- | --- |
| Market data | Bar, CsvDataFeed | Validate chronological input and expose bars |
| Decisions | Strategy, MovingAverageStrategy | Produce order intents from completed bars |
| Execution | Order, Fill, Broker, BrokerSim, IbkrBroker | Separate requests, status, executions and commissions |
| Controls | OrderManager, RiskManager | Track outstanding exposure and approve intents |
| Accounting | Portfolio, FillProcessor | Apply executions once and reconcile commissions |
| Replay | Backtester, PerformanceReport | Enforce event order and report marked equity |
| Operations | EventJournal, TradingApp | Persist state, reconcile, recover and run the event loop |

Start with [ROADMAP.md](ROADMAP.md). The first integration milestone is a standalone read-only
paper connection, before integrating the complete architecture. The intended application is fully
C++; Python is not a project dependency. The SDK adapter is hidden behind `IbkrBroker::Impl`, so
building this skeleton does not require an IBKR SDK installation.
