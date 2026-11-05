#include "OrderManager.hpp"

// Intentionally unimplemented. Uncomment one template when implementing its milestone.
// Until then, using these declarations requires implementations and will fail to link.

// OrderManager::OrderManager(Broker& broker) {
//     TODO: Keep broker reference; restore next ID from durable/broker state before trading.
// }

// OrderId OrderManager::submit(Order intent) {
//     TODO: Called only after risk approval; reserve exposure and persist intent before broker submission; retain ambiguous failures for reconciliation.
// }

// void OrderManager::cancel(OrderId id) {
//     TODO: Mark pending cancellation; keep reservation until terminal acknowledgement.
// }

// void OrderManager::on_update(const OrderUpdate& update) {
//     TODO: Handle duplicates and out-of-order events without regressing terminal states; status is not a fill.
// }

// void OrderManager::on_fill(const Fill& fill) {
//     TODO: Deduplicate executions and update reservations, never double count cumulative status quantities.
// }

// std::vector<OrderState> OrderManager::pending() const {
//     TODO: Return all outstanding exposure including unknown submissions and pending cancellations.
// }

// void OrderManager::reconcile(const BrokerSnapshot& snapshot) {
//     TODO: Match durable keys/broker IDs, detect external orders and block trading on unresolved differences.
// }
