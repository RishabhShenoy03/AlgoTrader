#include "BrokerSim.hpp"

// Intentionally unimplemented. Uncomment one template when implementing its milestone.
// Until then, using these declarations requires implementations and will fail to link.

// BrokerSim::BrokerSim(double fee_per_fill, double slippage_bps) {
//     TODO: Validate nonnegative fees/slippage; initially support market orders only.
// }

// void BrokerSim::on_bar_open(const Bar& bar) {
//     TODO: Fill only matching orders queued on earlier bars, at open plus adverse slippage; emit execution then commission; never fill new close signals here.
// }

// void BrokerSim::connect() {
//     TODO: Begin connection; readiness requires completed synchronization. Implement simulator events.
// }

// void BrokerSim::disconnect() {
//     TODO: Stop callbacks and join owned threads before destruction. Implement simulator events.
// }

// bool BrokerSim::ready() const {
//     TODO: False until account, open orders and executions have reconciled. Implement simulator events.
// }

// void BrokerSim::submit(const Order& order) {
//     TODO: Send an order; success here does not mean a fill. Implement simulator events.
// }

// void BrokerSim::cancel(OrderId id) {
//     TODO: Request cancellation; await broker acknowledgement. Implement simulator events.
// }

// void BrokerSim::request_snapshot() {
//     TODO: Request cash, positions, open orders and recent executions. Implement simulator events.
// }

// void BrokerSim::poll() {
//     TODO: Dispatch queued callbacks on the application thread. Implement simulator events.
// }

// void BrokerSim::set_callbacks(BrokerCallbacks callbacks) {
//     TODO: Install callbacks before connecting; callbacks must not outlive owners. Implement simulator events.
// }
