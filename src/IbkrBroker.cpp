#include "IbkrBroker.hpp"

// Intentionally unimplemented. Uncomment one template when implementing its milestone.
// Until then, using these declarations requires implementations and will fail to link.

// IbkrBroker::IbkrBroker(IbkrConfig config) {
//     TODO: Store explicit paper connection/account configuration; no SDK dependency in public header.
// }

// IbkrBroker::~IbkrBroker() {
//     TODO: Disconnect and join reader thread before releasing SDK objects.
// }

// void IbkrBroker::resolve_instrument(std::uint64_t ticker, std::string symbol, std::string exchange, std::string currency) {
//     TODO: Request contract details and reject ambiguous matches; initial scope is one stock/ETF.
// }

// void IbkrBroker::subscribe(std::uint64_t ticker) {
//     TODO: Map request ID to resolved contract; retain timestamps and market-data type; detect stale data.
// }

// void IbkrBroker::unsubscribe(std::uint64_t ticker) {
//     TODO: Cancel subscription and retire request mapping safely.
// }

// void IbkrBroker::connect() {
//     TODO: Begin connection; readiness requires completed synchronization. Map SDK callbacks through the serialized event queue.
// }

// void IbkrBroker::disconnect() {
//     TODO: Stop callbacks and join owned threads before destruction. Map SDK callbacks through the serialized event queue.
// }

// bool IbkrBroker::ready() const {
//     TODO: False until account, open orders and executions have reconciled. Map SDK callbacks through the serialized event queue.
// }

// void IbkrBroker::submit(const Order& order) {
//     TODO: Send an order; success here does not mean a fill. Map SDK callbacks through the serialized event queue.
// }

// void IbkrBroker::cancel(OrderId id) {
//     TODO: Request cancellation; await broker acknowledgement. Map SDK callbacks through the serialized event queue.
// }

// void IbkrBroker::request_snapshot() {
//     TODO: Request cash, positions, open orders and recent executions. Map SDK callbacks through the serialized event queue.
// }

// void IbkrBroker::poll() {
//     TODO: Dispatch queued callbacks on the application thread. Map SDK callbacks through the serialized event queue.
// }

// void IbkrBroker::set_callbacks(BrokerCallbacks callbacks) {
//     TODO: Install callbacks before connecting; callbacks must not outlive owners. Map SDK callbacks through the serialized event queue.
// }
