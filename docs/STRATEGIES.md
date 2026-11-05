# Creating and evaluating strategies

The core interface is `algo::Strategy::on_bar(const Bar&, const Portfolio&)`. It sees a completed
bar and read-only account state and returns zero or more order intents. It does not change cash,
submit directly to the broker or see future bars. `reset()` clears state between runs.

## Built-in research baselines

| CLI name | Entry when flat | Exit when long |
| --- | --- | --- |
| `sma` | Close above mean of the previous `window` closes | Close below that mean |
| `breakout` | Close above maximum of the previous `window` closes | Close below their mean |
| `mean-reversion` | Close below previous mean × (1 − `threshold`) | Close at or above mean |
| `buy-hold` | First eligible completed bar | No automatic exit |

The current bar is excluded from the reference window. `window` must be at least 2. There are
`window` warmup bars before SMA/breakout/mean-reversion decisions. Entry quantity is fixed by
`--quantity`; exits close the current long position. Risk controls suppress duplicate outstanding
orders for the same instrument. Rejected intents appear in `decisions.csv`.

These rules are examples, not evidence of a profitable edge. The same rule is unlikely to have the
same behavior across stock prices, option premiums and futures contracts.

## A custom strategy

Create a header and implementation and add its `.cpp` to `event_core` in `CMakeLists.txt`.
A minimal example buys only after a configured threshold:

```cpp
#include "Strategy.hpp"

class ThresholdStrategy final : public algo::Strategy {
public:
    void reset() override {}

    std::vector<algo::Order> on_bar(const algo::Bar& bar,
                                  const algo::Portfolio& portfolio) override {
        if (bar.ticker != 1 || bar.close <= 100 || portfolio.shares(1) != 0)
            return {};
        algo::Order order;
        order.ticker = 1;
        order.side = algo::Side::Buy;
        order.quantity = 1;
        order.created_at = bar.date;
        order.client_key = "threshold/" + bar.date;
        return {order};
    }
};
```

Instantiate it in `src/cli.cpp` in place of the built-in strategy for your instrument, or add a
factory branch for a new CLI name. The shared backtester/TradingApp continues to handle risk,
orders, execution and accounting. Add an exit rule before using a derivative with expiry.
For a limit order, set `type = OrderType::Limit` and a valid `limit_price`.

## Research workflow

1. Inspect raw data, timestamps, gaps, adjusted-price policy and contract metadata.
2. Establish a baseline with `buy-hold`, including the same fees and sizing assumptions.
3. Develop the strategy on an earlier date range; record parameters before examining the holdout.
4. Compare neighboring parameter values. Avoid selecting only the highest-return run.
5. Run the untouched later period and stress costs, delayed fills and rejected orders.
6. Inspect individual trades and drawdowns, then collect supervised paper observations.

Example parameter comparison:

```sh
for window in 5 10 20; do
  ./build/dev/algotrader backtest \
    --instruments my-instruments.csv --data my-bars.csv \
    --strategy sma --window "$window" --quantity 1 \
    --fee 1 --slippage-bps 10 --output "results/sma-$window"
done
```

Outputs: `summary.csv` contains initial/final equity, total return, maximum peak-to-trough drawdown,
execution count and fees. `trades.csv` contains executions (partial fills count separately), and
`equity.csv` includes initial capital. `decisions.csv` exposes approved/rejected strategy intents.
`run.txt` records the invocation and `instruments.csv` snapshots the catalogue. Retain the exact
input bar file with your research records. Reusing an output directory overwrites report files.

Returns are unannualized because data frequency/calendar are not inferred. Win rate, trade-level
PnL matching, Sharpe ratio, funding, dividends, borrow costs and tax are not implemented. Compare
like-for-like data periods and position sizing rather than assuming equal contract counts imply
equal risk.
