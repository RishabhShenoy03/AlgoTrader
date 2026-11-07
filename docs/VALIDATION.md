# Validation record

Validated on Linux ARM64 with GCC 14.2.0, CMake 3.31.6 and Ninja.

- Default debug build: all public headers compile independently; eight CTest cases pass (with Python available for the optional research-runner test).
- Core test executable: 132 explicit checks, including accounting, derivatives, risk, fills,
  malformed data, date ranges, deterministic replay, journal integrity and a simulated paper session.
- Earlier baseline: AddressSanitizer + UndefinedBehaviorSanitizer build passed seven CTest cases.
  The latest quote/research additions were checked with debug builds, not re-certified under sanitizers.
- Optional adapter: official TWS API 10.50.02, locally generated protobuf 3.21.12 sources and
  Intel decimal library ABI 000 compile/link successfully.
- Real-SDK loopback test: mock TWS handshake, explicit paper-account verification, account currency,
  snapshot end-marker gating, incomplete-snapshot timeout, read-only submission guard, disconnect
  readiness and decimal conversion roundtrip pass. Added regression cases confirm historical
  advisory 2188 preserves bars, real permission errors and missing end markers fail, delayed quotes
  retain their type and omit unavailable prices, and diagnostic quotes reject an order-enabled adapter.
- SDK-enabled debug build: nine CTest cases pass. The research-runner test uses synthetic data,
  invokes only offline backtests, checks staged experiments and prevents output overwrites.
- Original demo regression: equity values remain 1000, 994, 1006.
- CLI example: all four research strategy modes execute and emit CSV reports; unknown options fail.

Reproduce offline checks:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/engine_tests

cmake -S . -B build/sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug -DALGO_SANITIZERS=ON
cmake --build build/sanitize -j 2
ctest --test-dir build/sanitize --output-on-failure
```

With the local SDK preset, run `cmake --build --preset ibkr` and `ctest --preset ibkr` for the
SDK-enabled tests, including the mock TWS socket test. See `IBKR.md` for a fresh-machine SDK build.

A real paper Gateway on the Raspberry Pi has now verified account snapshots, AAPL contract
resolution, broker cancellation feedback, a one-share execution at $343.94 with commission,
delayed quote retrieval and 3,770 historical daily bars. Live five-second bars still return
permission error 420. Historical advisory 2188 was incorrectly treated as fatal and is now
handled explicitly without suppressing real failures. See `IBKR.md` for the diagnosis.

The development-only daily-price experiment is recorded in `docs/RESEARCH.md`. Later validation
and final holdout periods have not been evaluated. No further orders were placed while fixing
data access or building the research workflow. The mock and paper test do not certify strategy
profitability, option assignment, exchange liquidity or crash recovery of a live session.
