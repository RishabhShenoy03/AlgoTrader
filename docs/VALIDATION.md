# Validation record

Validated on Linux ARM64 with GCC 14.2.0, CMake 3.31.6 and Ninja.

- Default debug build: all public headers compile independently; seven CTest cases pass.
- Core test executable: 132 explicit checks, including accounting, derivatives, risk, fills,
  malformed data, date ranges, deterministic replay, journal integrity and a simulated paper session.
- AddressSanitizer + UndefinedBehaviorSanitizer build: the same seven CTest cases pass.
- Optional adapter: official TWS API 10.50.02, locally generated protobuf 3.21.12 sources and
  Intel decimal library ABI 000 compile/link successfully.
- Real-SDK loopback test: mock TWS handshake, explicit paper-account verification, account currency,
  snapshot end-marker gating, incomplete-snapshot timeout, read-only submission guard, disconnect
  readiness and decimal conversion roundtrip pass.
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

No real IBKR account session was available during implementation. Actual account permissions,
contract resolution, historical downloads, real-time subscriptions and paper executions remain
external acceptance checks. The mock does not simulate the entire brokerage service or certify
strategy profitability, option assignment, exchange liquidity or crash recovery of a live session.
