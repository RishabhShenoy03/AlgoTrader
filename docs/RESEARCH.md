# Your first strategy investigation

Start with [the development report](../results/aapl-trend-development/report.md).
It is an experiment on real historical data, not a recommendation to deploy.
The trading engine and strategies remain C++. `scripts/research.py` is an optional
Python-standard-library helper that runs the C++ binary and collects reports; it
has no broker connection or order-submission path.

## What is working now

- A manual paper order filled, and the account holds one AAPL share.
- `ibkr-quotes` returns explicitly delayed AAPL quotes without a paid-data change.
- `ibkr-history` returns historical daily bars after fixing advisory 2188 handling.
- Live five-second bars still require the appropriate account entitlement. Delayed
  watchlist quotes are not converted to live strategy bars.
- The first research experiment uses AAPL because its contract is already verified.
  A broad ETF should be a separate follow-up test, not chosen after comparing profits.

No new trades were placed during this research work. Do not run the current
`ibkr-paper --strategy sma --window 100` expecting this daily strategy: that mode
currently consumes five-second bars, so 100 bars means about 8 minutes, not 100 days.
A daily paper scheduler with completed-session checks and historical warmup is
still needed before deploying this specific research rule.

## The preliminary hypothesis

**Hypothesis:** price trends can persist, so holding only while the daily close is
above a slow moving average may reduce sustained downtrend exposure. The tradeoff
is repeated small losses when prices cross the average in a sideways market, and
missing some recoveries. This is a hypothesis to falsify, not an established edge.

Freeze the initial candidate at a 100-day lookback:

1. After a daily bar is complete, calculate the average of the **previous 100
   closes**, excluding today's close. This is exactly what the current C++ code does.
2. If flat and today's close is above that average, buy 10 shares at the **next
   available session's open**, including fees/slippage.
3. If long and today's close is below the average, sell the held shares at the
   next session's open. If equal, keep the current state.
4. Hold cash otherwise. No leverage, shorting, pyramiding or options in this first
   experiment. Sizing is fixed for a simple accounting exercise, not optimal sizing.
5. Use 50 and 200 days as coarse development sensitivity checks. Do not secretly
   promote 200 merely because it happened to look best in this sample.

See `MovingAverageStrategy::on_bar` in `src/MovingAverageStrategy.cpp` and
`Backtester::batch` in `src/Backtester.cpp`. The same day's closing signal must
never be filled at that day's opening price: that would use information from the future.

## Data already downloaded

`results/research-aapl-2011-2025/bars.csv` contains 3,770 daily records from
2011-01-05 through 2025-12-31. Source: your IBKR Gateway, contract 265598, TRADES,
regular trading hours, request end `20260101-00:00:00` UTC, duration `15 Y`.
Retrieved 2026-11-07 Singapore time. The exact start follows the returned dataset;
we do not fill in missing earlier sessions.

IBKR says TRADES history is adjusted for splits but not dividends.
[IBKR historical-bar documentation](https://www.interactivebrokers.com/docs/tws-api/doc/md/historical-bars).
Consequences for this project:

- Treat this as a price-only study in split-adjusted share units. Historical
  adjusted prices are not necessarily prices you could literally trade at then.
- The engine does not process dividend cash or stock-split share changes. This is
  especially relevant when comparing stock exposure with cash. It is not a total-return test.
- Cash earns no interest. Fees are a flat amount per fill, not a reconstructed
  historical broker schedule. Tick rounding and slippage on adjusted prices are
  approximations, particularly across old split events.
- AAPL was selected as a surviving, well-known company today. Results on this
  company alone cannot establish performance across a stock universe.
- Do not use these prices as option premiums. Options require historical data for
  each actual contract, quotes/spreads, expiry and exercise/assignment treatment.

Data-quality checks before interpreting results: chronological dates, no duplicate
sessions, positive finite prices, missing sessions vs exchange calendar, outlier
returns, and several prices checked against an independent provider. The runner
checks ordering/duplicates/positive prices but **does not validate an exchange
calendar or independently verify the provider's prices**. Those remain research tasks.

## Run the experiment yourself

From the project root, run:

```bash
python3 scripts/research.py \
  --data results/research-aapl-2011-2025/bars.csv \
  --stage develop \
  --source 'IBKR TRADES daily, retrieved 2026-11-07; split-adjusted, excludes dividends' \
  --output results/my-first-development
```

The existing completed report is `results/aapl-trend-development/report.md`.
The runner refuses to overwrite any output directory. Use a new descriptive path
for a deliberately changed experiment; retain the old evidence.

It uses $10,000 initial cash and 10 shares per entry for every strategy, with:

| Assumption | Base | Stress |
| --- | ---: | ---: |
| Fee per execution | $1.10 | $2.20 |
| Adverse slippage per side | 5 basis points (0.05%) | 20 basis points (0.20%) |

These are illustrative research assumptions, not an IBKR fee quote. The observed
one-share paper commission was about $1.09; costs vary with venue, size and time.
Buy-and-hold buys after the first close. Every SMA run starts cold and waits its
own window. All strategies use the same period; the warmup delay is deliberately
included in these initial results. For a more rigorous parameter comparison,
add a common pre-period warmup with signals disabled and a common scoring start.

Initial results, development period only:

| Strategy | Base net P&L | Stress net P&L | Base maximum account drawdown | Executions |
| --- | ---: | ---: | ---: | ---: |
| Buy and hold | $273.55 | $272.25 | 2.04% | 1 |
| SMA 50 | $14.40 | -$193.40 | 1.24% | 138 |
| SMA 100 (initial candidate) | $157.70 | $36.80 | 1.17% | 80 |
| SMA 200 | $310.90 | $280.00 | 0.96% | 20 |

This tells us turnover matters and the candidate is cost-sensitive. It does not
prove SMA 200 has an edge. Most account cash is idle; the small account returns
and drawdowns are **not** the returns and risk of a fully invested strategy.
Ending holdings are marked to the last close; no closing trade or terminal sale
commission is fabricated. Buy-and-hold receives no dividends in this model.

## Protect the unseen data

| Stage | Dates | What to do |
| --- | --- | --- |
| Development | 2011–2018 | Learn the data, audit trades and test the small declared parameter set |
| Validation | 2019–2021 | Run the frozen candidate; decide whether the hypothesis merits more work |
| Final holdout | 2022–2025 | Evaluate once after freezing the complete method |

Only development has been run. The helper does not automatically select a winner
or execute validation/holdout. For validation, after writing down your chosen
rules, run:

```bash
python3 scripts/research.py \
  --data results/research-aapl-2011-2025/bars.csv \
  --stage validate --window 100 \
  --source 'IBKR TRADES daily; same frozen dataset as development' \
  --output results/aapl-validation-w100
```

Run `--stage holdout --window 100` with a new output path only after the method is
frozen. Each period restarts flat and cold; these are independent experiments,
not a stitched continuous portfolio. Do not sum their returns.

Once you inspect a holdout and change rules in response, that interval is no longer
unseen. Record every attempted strategy, including failures. Do not randomly shuffle
bars: the order of market regimes and the information available at each moment matter.

Before validation, write a short research note answering:

- Why might the rule work, and which behavior would disprove that explanation?
- Are lower drawdowns worth giving up some buy-and-hold return for your objective?
- Which cost assumptions, sizing rule and instruments are fixed?
- How much drawdown is tolerable for the intended allocation, not just idle-cash returns?
- What results would make you abandon the idea rather than tune another parameter?

## How to read and audit a backtest

A backtest replays information in time order and applies an execution/accounting
model. It estimates what **that model** would have done. It does not reproduce
queue position, actual exchange fills or future profitability.

Open one run directory, for example `results/aapl-trend-development/sma-100-base`:

1. `trades.csv`: pick the first buy. Find the preceding bar in `bars.csv`. Average
   the 100 earlier closes and verify the buy signal. Check that the fill occurs at
   a strictly later session open. With 5 bps slippage, a buy starts from
   `open × 1.0005`, then rounds adversely to the instrument tick size.
2. Trace cash: a buy subtracts `shares × fill price + fee`; a sell adds
   `shares × fill price − fee`. Mark equity as cash plus shares times that day's close.
3. `equity.csv`: look for the largest peak-to-trough decline. Maximum drawdown is
   `(previous peak − current equity) / previous peak`. Read the dates, not just the number.
4. `decisions.csv`: explain any rejected intent. A strategy that cannot afford its
   orders is not the experiment you intended to run.
5. `summary.csv`: total return is final equity divided by initial equity minus one;
   it includes unrealized P&L and is not annualized. Executions are fills, not
   completed round trips. A high win rate alone says nothing about loss size.
6. Compare base and stress costs. If small assumption changes erase the effect,
   execution modeling is central to the hypothesis.

`manifest.json` records inputs and executable hashes, parameters and exact commands.
Each run preserves the selected input bars and instrument catalogue. Keep the
corresponding source revision as well; a hash identifies a binary but does not
reconstruct it.

## A practical learning schedule (30–60 minutes per session)

1. Run `ibkr-status` and `ibkr-quotes`; explain the difference between delayed prices,
   account positions, order status and executions.
2. Read 20 daily data rows and reproduce one SMA signal by hand or in a spreadsheet.
3. Trace one buy and sell through fees, cash and equity.
4. Read the eight development runs; explain the turnover/cost differences in your own words.
5. Review missing dividends/splits, sizing and data provenance before claiming a result.
6. Write the frozen candidate and failure criteria, then run validation.
7. Investigate the validation drawdowns without changing the rule after every loss.
8. Decide whether to abandon, explicitly redesign using development/validation only,
   or proceed to the final holdout.

Before daily automated paper trading: implement common warmup, session-calendar
scheduling, daily signal persistence, order reconciliation/recovery, a fresh-price
check or suitable bounded execution policy, and order sizing. The existing one-share
AAPL position must be handled explicitly; the current trading app intentionally
requires a flat account. Continue logging daily expected versus actual orders/fills.

After learning the workflow, extend one asset model at a time. ETF research needs
correct distributions; futures need explicit rolls, contract multipliers and
variation/maintenance margin; options need contract selection from point-in-time
chains, spreads, expiries and exercise/assignment. The current long-option support
is an accounting starting point, not a complete options-research platform.
