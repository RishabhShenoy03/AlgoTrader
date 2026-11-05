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
SDK decimal conversions. No successful real-account handshake, market-data subscription, historical download or paper fill has been
observed without your configured TWS session. Those are the next external validation steps.
