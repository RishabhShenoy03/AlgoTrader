# IBKR paper setup

This adapter uses the official C++ TWS socket API, not Python or a reverse-engineered protocol.
It was compiled against **TWS API 10.50.02**. API callback signatures change across SDK versions;
use that version or verify compatibility before upgrading.

## 1. Prepare paper TWS

1. Install TWS/IB Gateway from IBKR and log into the **paper** session.
2. Enable socket API clients in API settings. Initially keep **Read-Only API** enabled.
3. Note the configured socket port; use that value rather than relying on a guessed default.
4. Use a positive, unused client ID (the CLI default is 17), localhost when both programs run on
   this machine, and the exact paper account identifier shown by TWS (`DU...`).
5. Confirm account currency and market-data permissions. Read-only access, delayed subscriptions
   and live real-time bars have different requirements; errors are surfaced rather than bypassed.

Follow [IBKR's API documentation](https://www.interactivebrokers.com/docs/tws-api/) and
[official download page](https://download2.interactivebrokers.com/installers/tws-api/index.html).
The SDK is not redistributed in this repository. Download/install it under its applicable terms.

## 2. Build the adapter

On the workspace prepared with this implementation, `CMakeUserPresets.json` already defines an
`ibkr` preset pointing to the SDK/dependencies under `~/.local/share/algotrader`. It is ignored by Git
because its paths are machine-specific. You can use it directly, including from VS Code:

```sh
cmake --preset ibkr
cmake --build --preset ibkr
ctest --preset ibkr
```

For a fresh machine, follow the dependency setup below.

For Debian/Ubuntu, install build dependencies (these commands require your system package privileges):

```sh
sudo apt-get install cmake ninja-build g++ libprotobuf-dev protobuf-compiler libintelrdfpmath-dev
```

If your distribution does not supply `libintelrdfpmath-dev`, build the Intel decimal library using
[IBKR's instructions](https://www.interactivebrokers.com/docs/tws-api/doc/download-the-tws-api/unique-configurations/implementing-the-intel-decimal-library-for-mac-os-and-linux).
The required ABI is CALL_BY_REF=0, GLOBAL_RND=0, GLOBAL_FLAGS=0 (`libbidgcc000.a` in Debian packages).

After installing/extracting the SDK:

```sh
cmake -S . -B build/ibkr -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DALGO_WITH_IBKR=ON \
  -DIBKR_SDK_ROOT=/absolute/path/to/tws-api
cmake --build build/ibkr -j 2
ctest --test-dir build/ibkr --output-on-failure
```

`IBKR_SDK_ROOT` must contain `source/cppclient/client/EClientSocket.h` and `source/proto`.
CMake regenerates protobuf files with your installed protoc/runtime and compiles the SDK in the
build directory, without editing SDK sources. Set `IBKR_DECIMAL_LIBRARY` explicitly if discovery
fails. On ARM64, dependencies must also be ARM64; the Linux SDK installer can bundle an x86 JRE,
so extracting/installing the SDK may need a platform-compatible Java runtime. SDK compilation itself
uses C++ and does not require Java at runtime.

## 3. Prepare one real instrument

Copy the header and **one** row from `examples/instruments.csv` to a private catalogue. Replace
symbol, exchange, contract ID, currency and all contract details with values shown in TWS contract
information. Example rows use conId 0 intentionally and cannot submit an order. Futures and options
must identify a specific expiry, and options must include the exact strike/right/multiplier.

Keep the first automatic session to a liquid instrument with a simple quantity and understandable
costs. Do not use option premium or contract IDs from the synthetic examples as real market data.

## 4. Read-only connection

Replace the placeholder port/account below with your configured values:

```sh
./build/ibkr/algotrader ibkr-readonly \
  --instruments my-instrument.csv --host 127.0.0.1 \
  --port YOUR_PORT --client-id 17 --account DU_YOUR_ACCOUNT --seconds 60
```

The adapter waits for handshake, account verification, contract resolution and all snapshot end
markers, prints cash/positions/open orders, then prints completed five-second TRADES bars. Compare
with TWS. Unknown held contracts need catalogue coverage; this first CLI version targets one
instrument and is intended for a dedicated paper account.

Outside regular hours or without suitable market data, real-time bars may not arrive. The default
30-second stale-data watchdog stops the session rather than trading from old prices. This is an
expected diagnostic, not evidence that the strategy works. Increase `--timeout` for slow historical
requests, and use `--stale-seconds` deliberately for your data environment.

## 5. Export historical bars

```sh
./build/ibkr/algotrader ibkr-history \
  --instruments my-instrument.csv --port YOUR_PORT --account DU_YOUR_ACCOUNT \
  --duration "1 M" --bar-size "1 day" --end-time "20260131-23:59:59" \
  --timeout 60 --output results/history

./build/dev/algotrader backtest \
  --instruments my-instrument.csv --data results/history/bars.csv \
  --strategy sma --window 10 --quantity 1 --output results/history-test
```

Duration/bar-size combinations, history availability, pacing and data permissions are controlled by
IBKR. This command makes one bounded request; it does not bypass limits, automatically paginate or
supply missing expired option data. `--end-time` uses IBKR's UTC `YYYYMMDD-HH:MM:SS` syntax; omitting
it requests up to the present and may include a still-forming bar. Prefer a completed historical
endpoint and inspect the export before research. Daily futures bars may use settlement prices.
[IBKR historical bars documentation](https://interactivebrokers.github.io/tws-api/historical_bars.html).

## 6. Automated paper session

First verify the read-only path. Start with a **flat paper account and no open orders**; avoid other
manual/API trading in that account. Disable Read-Only API in the paper TWS settings when ready.

```sh
./build/ibkr/algotrader ibkr-paper \
  --instruments my-instrument.csv --port YOUR_PORT --client-id 17 \
  --account DU_YOUR_ACCOUNT --enable-orders yes \
  --strategy sma --window 20 --quantity 1 \
  --max-order 1000 --max-position 1000 --cash-buffer 100 \
  --max-session-loss 0.02 --seconds 300 --journal results/paper-session-001.journal
```

Choose limits suitable for the actual instrument's notional; the values above are illustrative.
Twenty five-second bars mean roughly 100 seconds of warmup, not twenty days. `--seconds` includes
startup time. The same strategy logic is used in replay and paper mode, but paper fills come only
from broker execution callbacks. Press Ctrl+C to stop new decisions and request cancellation.

Inspect orders, fills, commissions and holdings in TWS and the journal. Stop does not close held
positions. Reconcile and flatten manually before starting a new session with a new journal path.
A disconnect, stale feed, external event or unresolved state requires manual review; automatic
broker recovery is not implemented. Do not reuse the same journal to force a restart.

## What is verified here

The offline core, simulator, journal, parser and strategy paths have automated tests. The adapter
compiles and links against the official SDK. A local mock TWS exercises handshake, paper-account
and currency verification, snapshot completion, timeout, read-only rejection, disconnect handling and
SDK decimal conversions. A configured paper Gateway on port 4002 has now passed handshake, account/cash/position/open-order
snapshots, AAPL contract resolution, and a read-only manual-order preview. The live-bar request
returned error 420 (no market data permissions). A subsequent manual paper test confirmed order submission, a broker price-protection cancellation,
and a successful one-share AAPL execution with commission reporting. Historical daily download has also been verified after fixing advisory 2188 handling;
automated strategy trading remains externally unverified.

## First manual paper buy without a streaming-data subscription

`ibkr-buy` previews by default and does not request market data. It is a small
connectivity/order-lifecycle test: one USD stock share, limit price at most $1000,
DAY validity and regular trading hours. AAPL is an example contract, not a strategy
recommendation. The adapter verifies its contract metadata with IBKR at startup.

From the repository root, preview using your paper account and chosen maximum
price per share (do not enter the angle brackets):

```sh
./build/ibkr/algotrader ibkr-buy --instruments examples/ibkr-aapl.csv   --port 4002 --account <YOUR_PAPER_ACCOUNT> --limit-price <YOUR_MAX_PRICE>
```

Add `--enable-orders yes` to send the order. This command really submits a paper
order. It requires an initially flat account with no open orders and cash covering
the limit plus a $100 buffer. It records intent and callbacks in
`results/manual-buy.journal` and refuses to reuse a nonempty journal. Reconcile any
previous attempt before selecting a new `--journal` path; a unique path is not a
substitute for reconciliation after a disconnect.

The default monitoring window is 60 seconds (`--seconds 1..300`). On timeout or
Ctrl+C it requests cancellation and waits up to 10 seconds for acknowledgement.
A cancellation request can race with a fill. If disconnected, cancellation may
remain unconfirmed: inspect the paper account before retrying. Filled shares are
not sold on exit. Late commissions may arrive after the command exits; the broker
account remains the authoritative record. An accepted order is not a confirmed
fill. Outside regular market hours, or if the limit is too low, it may remain
unfilled and be cancelled. IBKR may reject orders for permissions or precautionary
settings even though the API connection works.

The automatic `ibkr-paper` strategy still needs the live five-second bar feed;
error 420 (no market data permissions) blocks that path. This manual test does not
resolve data entitlements or prove that an automated strategy works.

## Recorded paper test and cancellation diagnosis (2026-11-07 Singapore)

The first AAPL BUY 1 LIMIT 400 order reached IBKR but was cancelled with zero fills.
Error 202 explained that the limit was too aggressive: IBKR quoted a market price
of 343.83 and rejected limits at or above 353.938602. The manual test was then
retried once with a 344 limit after confirming no positions or open orders. IBKR
reported a one-share fill at 343.94 and commission of 1.090003 USD. These are
historical test values, not current quotes or prices to hard-code for future orders.

Root cause: a buy limit chosen far above the market triggered IBKR's price
protection. A successful API connection and `Submitted` status do not guarantee
execution. Missing streaming-data entitlement was a separate issue; it did not
prevent this manual paper fill. Preserve IBKR's protections and use a limit near a
fresh quote when testing again, within the user's maximum. Do not automatically
retry cancellation messages or infer a current quote from an old error message.

The adapter now forwards error 202 explanations through an informational callback
instead of discarding them. The CLI prints/journals these notices, distinguishes
PendingCancel from PendingSubmit, and reports the cumulative filled quantity.
An unfilled cancellation exits with code 2; errors/rejections exit with code 1;
a reported fill exits with code 0. Reconcile the broker account after an uncertain
outcome regardless of the exit code. A successful buy leaves the position open,
so the first-buy command intentionally refuses subsequent buys in that account.

Check holdings without streaming data or order submission:

```sh
./build/ibkr/algotrader ibkr-status --instruments examples/ibkr-aapl.csv --port 4002 --account <YOUR_PAPER_ACCOUNT>
```

The local audit records are `results/manual-buy-agent-1.journal` (cancelled) and
`results/manual-buy-agent-2.journal` (filled). They are deliberately ignored by Git.
The Gateway API-client indicator turns red when a short-lived CLI disconnects;
this alone is not an IBKR server connection failure.

## Data access diagnosis and fix (2026-11-07)

There were two distinct problems:

1. `reqRealTimeBars` returned error 420: the login lacks permission for live bars.
   This is an account entitlement, not a socket or order-permission failure.
2. Historical requests returned advisory **2188**, which our adapter incorrectly
   treated as fatal. It means up-to-the-second history requires a subscription;
   older bars can still follow. We now log this notice only for an active historical
   request, then require actual bars AND the end marker. Real errors (e.g. 162),
   empty data and timeouts still fail. The regression tests cover these paths.

Verified against Gateway: delayed AAPL bid/ask/last values arrived, followed by a
successful download of 3,770 daily AAPL bars spanning 2011-01-05 to 2025-12-31.
No additional subscription was purchased and no new orders were placed.

```bash
./build/ibkr/algotrader ibkr-quotes \
  --instruments examples/ibkr-aapl.csv --port 4002 --account <YOUR_PAPER_ACCOUNT> \
  --market-data-type 3 --timeout 10

./build/ibkr/algotrader ibkr-history \
  --instruments examples/ibkr-aapl.csv --port 4002 --account <YOUR_PAPER_ACCOUNT> \
  --duration '15 Y' --bar-size '1 day' --end-time 20260101-00:00:00 \
  --timeout 45 --output results/research-aapl-2011-2025
```

`ibkr-quotes` is read-only. Type 3 requests delayed quotes; 4 requests delayed-frozen
quotes, useful when closed. The output reports the actual type received, because
IBKR can supply live data if entitled. Unavailable/nonpositive prices are omitted.
No timestamp guarantee is made for bid/ask/last; they are diagnostics, not actionable
fresh-price checks. They never enter `on_bar`, and this mode cannot submit orders.
Do not relax the stale-data guard or label delayed ticks as current five-second bars.

For actual live bars, sign into the live-account Client Portal and check
**Settings → Trading Platform → Market Data Subscriptions**, including API access
for the exchange/product. Check the paper-account data-sharing settings for the
appropriate live username. An existing subscription might be shareable; otherwise
an eligible funded account and a paid subscription may be needed. Availability and
fees depend on the account; no subscription changes were made here. Keep the live
session/data-sharing restrictions in mind if live and paper run on different devices.

Sources: [IBKR delayed data](https://www.interactivebrokers.com/docs/tws-api/doc/md/delayed-data),
[subscription settings](https://www.ibkrguides.com/orgportal/usersettings/marketdatasubscriptions.htm),
[paper data sharing](https://www.interactivebrokers.com/lib/cstools/faq/content/1136176599),
and [the upstream firsthand 2188 bug report](https://github.com/nautechsystems/nautilus_trader/issues/5088).
The fix was independently confirmed with this project's real Gateway and loopback tests.

Start your offline investigation with [the worked research guide](RESEARCH.md).
