# Execution architecture and operational boundaries

All application types live in namespace `algo`; the SDK keeps its own global types. CMake links
implemented components into `event_core`; headers are independently compile-checked on every build.

## Backtest event order

For each timestamp batch:

1. Require a current bar for every existing holding and reject expiry-date holdings.
2. Execute earlier orders at the matching instrument's open.
3. Apply uniquely identified executions; apply commissions separately.
4. Mark all closes and settle futures P&L under the documented simulation convention.
5. Record portfolio equity, evaluate each strategy, risk-check intents and reserve exposure.
6. Submit approved intents for later bars. Cancel final pending orders at end of data.

BrokerSim has a separate account ledger. End-of-run reconciliation compares its cash/positions and
open orders with application state. Order status events never book a trade. Cumulative filled
status arriving before its executions retains a pending reservation until accounting catches up.
Duplicate executions are ignored; conflicting corrections fail for explicit reconciliation.
Commissions can precede fills, be duplicated or have their total corrected later.

Risk limits cover order/position notional, available cash, configured futures margin, cash buffer,
quantity increments and one pending order per instrument. Limits are pre-trade estimates; gaps can
cause fills to differ from those estimates. The simulator rejects unaffordable fills. Broker truth
is accounted even when it violates a local cash estimate. Paper session loss checks happen on bars;
they halt further decisions and request cancellation, not guaranteed liquidation or a loss cap.

## IBKR paper session

The CLI requires an explicit `DU...` paper account and, for submission, both `ibkr-paper` mode and
`--enable-orders yes`. The adapter verifies that exact account through `managedAccounts`, waits for
`nextValidId`, validates resolved contracts and collects account/position/open-order end markers.
A socket connection alone is insufficient to make the broker ready.

The reader thread decodes socket input into the SDK queue. Application callbacks run when the main
thread calls `poll()`. Broker order IDs are allocated independently from local IDs; stable client
keys are sent in `orderRef`. Completed five-second TRADES bars drive strategy decisions. Data
permission errors, stale/future timestamps, malformed events and disconnected sessions fail closed.

Initial automatic paper sessions require **one instrument, a flat account and no open orders**.
External/unrecognized order or execution events cause a halt. Do not share the account with another
strategy or manual trading during a session. The adapter requests all open orders for startup but
is not a comprehensive multi-client account supervisor. Read-only mode can inspect positions without
adopting them, provided their contracts are known to the catalogue.

On normal shutdown, request cancellation and wait up to two seconds for acknowledgements. Open
positions remain open. Unresolved orders are recorded; inspect TWS. If the connection or disk fails,
cancellation may be impossible. There are no automatic reconnections or repeated submission attempts.

## Journal and restart

The POSIX journal takes an exclusive process lock, uses ordered versioned records with corruption
checksums, and calls `fdatasync` after every append. Approved intent is written before submission;
execution and commission events are written before application accounting. Record fields cannot
contain newlines. A corrupt/truncated log is rejected, not silently discarded.

This is an audit log and fail-closed restart guard, **not a complete recoverable account checkpoint**.
The first record creates a session. A process refuses to trade using an existing nonempty journal,
including after a clean stop. Reconcile/flatten the account in TWS and use a new journal path for a
new session. Do not erase a journal to bypass an uncertain order. Fully automatic recovery requires
broker ID mapping, account checkpoints, execution-history reconciliation and commission-state replay;
these are explicitly deferred in ROADMAP.md.

## Testing boundary

Offline tests exercise accounting, orders, risk, parser errors, deterministic replay, look-ahead
prevention, option/futures rules, reporting, journal integrity/locking and a simulated TradingApp
session. The optional adapter is compiled against the official SDK. A local mock TWS exercises the real SDK handshake, account/currency checks, snapshot end markers,
timeouts, read-only submission guard, disconnect behavior and decimal conversion ABI. These tests
do not verify actual IBKR subscriptions, account permissions or paper executions. Those require a running, configured user account.
