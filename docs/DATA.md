# Data and asset models

## Instrument catalogue

Use this exact header:

```csv
ticker,symbol,asset,currency,multiplier,lot_size,tick_size,initial_margin,expiry,strike,right,exchange,con_id
1,DEMO,stock,USD,1,1,0.01,0,,0,,SMART,0
2,BTC,crypto,USD,1,0.0001,0.01,0,,0,,PAXOS,0
3,MES,future,USD,5,1,0.25,1500,2026-12-18,0,,CME,0
4,DEMO,option,USD,100,1,0.01,0,2026-12-18,100,C,SMART,0
```

These are examples, not current contract/margin specifications. Supply correct metadata from your
broker/data vendor. `ticker` is your positive integer identifier; `con_id` is a different IBKR
contract identifier. Use 0 for offline-only instruments. IBKR mode requires a real positive conId
and verifies symbol, type, currency, multiplier, expiry, option strike/right and minimum tick.

- `asset`: `stock` (also ETFs), `crypto` (spot only), `future`, or `option`.
- `currency`: must match `--currency`; no implicit FX conversion.
- `multiplier`: price-to-cash factor per unit/contract. Never assume all options use 100.
- `lot_size`: quantity increment; futures/options require integer contracts.
- `tick_size`: adverse execution prices are rounded to this increment. Exchange tick tables can
  vary with price; the simulator uses one fixed increment, so split or adapt the model if needed.
- `initial_margin`: fixed cash reserved per futures contract; not the contract notional or a fee.
- `expiry`: mandatory for futures/options, `YYYY-MM-DD`. Close before this date.
- `strike`, `right`: option strike and `C`/`P`; use 0 and an empty right for other assets.
- `exchange`: contract routing exchange, used by IBKR. `con_id` must never be guessed.

One `--quantity` currently applies to every instrument's strategy. For different sizes, create
separate runs or instantiate strategies with per-instrument quantities in C++.

## Bar CSV

```csv
date,ticker,open,close
2026-01-05,1,100,102
2026-01-05,2,60000,61000
2026-01-06,1,103,101
2026-01-06,2,61100,60500
```

Dates are either `YYYY-MM-DD` for a consistent daily session calendar, or UTC timestamps
`YYYY-MM-DDTHH:MM:SSZ` for intraday bars. Do not mix formats. Rows must be chronological, and a
(ticker, timestamp) pair must be unique. Equal-timestamp rows form one portfolio batch. The original
row order within the batch determines order priority when instruments compete for available cash.

Every held instrument must have a bar in each batch. This deliberately rejects stale valuation;
for mixed-session assets, align the dataset to a shared calendar or run separate portfolios. Never
forward-fill an open price and interpret it as a tradable quote.

Prices must be finite and positive; negative-price commodity futures and zero-premium option bars
are not supported in this release. Parser errors include file and row. Headers are strict, fields
are unquoted, and blanks/malformed rows are errors rather than silently skipped observations.

`--from` and `--to` select inclusive calendar dates. The strategy starts cold at `--from`; allow
warmup within the selected interval. Preserve a chronological holdout period rather than randomly
shuffling trading bars.

## Accounting and execution assumptions

| Asset | Buy cash movement | Marked value / equity |
| --- | --- | --- |
| Stock / ETF / crypto spot | quantity × price × multiplier, plus fee | cash + market value |
| Long option | contracts × quoted premium × multiplier, plus fee | cash + option premium market value |
| Long future | fee only; fixed initial margin reserved | cash + change from settlement basis × contracts × multiplier |

Futures P&L is transferred to cash and the basis reset at each simulated bar close. Daily bars
approximate daily mark-to-market only if the close is an appropriate settlement mark. Intraday
settlement is a simplifying convention. Falling below configured initial margin stops the run;
maintenance margin, variation-margin timing and liquidation are not modeled. See
[CME's mark-to-market explanation](https://www.cmegroup.com/education/courses/introduction-to-futures/mark-to-market).

A close-derived order becomes eligible at a strictly later bar's open. Market fills use adverse
basis-point slippage and tick rounding. Limit orders fill only if the opening execution price
satisfies the limit; there is no invented intrabar high/low path. `--max-fill` caps quantity filled
per order per bar, with a fee for each partial fill. It is a test model, not a volume/liquidity model.
A gap that makes an order unaffordable results in rejection at execution.

There are no automatic expiry settlements, option pricing/Greeks, early exercise, assignment,
stock splits, dividends or futures rolls. Option tests must use each specific contract's actual
premium history, not its underlying's prices. Flatten/roll in your strategy before expiry or test
a pre-expiry interval. Continuous futures data needs an explicit roll/accounting model before
it can represent realizable returns.
