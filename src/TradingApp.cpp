#include "TradingApp.hpp"

// Intentionally unimplemented. Uncomment one template when implementing its milestone.
// Until then, using these declarations requires implementations and will fail to link.

// TradingApp::TradingApp(Broker& broker, Strategy& strategy, Portfolio& portfolio, OrderManager& orders, FillProcessor& fills, RiskManager& risk, EventJournal& journal) {
//     TODO: Wire callbacks onto one event loop; dependencies must outlive app.
// }

// void TradingApp::start() {
//     TODO: Recover journal, connect read-only, reconcile account/positions/orders/executions before enabling decisions.
// }

// void TradingApp::run() {
//     TODO: Poll broker, detect stale data/disconnects, and serialize all state mutations; initial mode is paper only.
// }

// void TradingApp::on_bar(const Bar& bar) {
//     TODO: After synchronization and freshness gates, evaluate strategy, risk-check and journal approved intents before submission.
// }

// void TradingApp::reconcile(const BrokerSnapshot& snapshot) {
//     TODO: Compare broker truth with recovered state; require completed snapshot and resolve discrepancies before resuming.
// }

// void TradingApp::stop() {
//     TODO: Halt new intents; request cancellations per policy, drain acknowledgements, persist unresolved orders, disconnect and flush.
// }
