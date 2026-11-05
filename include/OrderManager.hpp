#pragma once
#include "Broker.hpp"
#include <unordered_map>
#include <vector>

// Declaration-only scaffold; implementations are intentionally deferred.
class OrderManager {
public:
    // Keep broker reference; restore next ID from durable/broker state before trading.
    explicit OrderManager(Broker& broker);
    // Called only after risk approval; reserve exposure and persist intent before broker submission; retain ambiguous failures for reconciliation.
    OrderId submit(Order intent);
    // Mark pending cancellation; keep reservation until terminal acknowledgement.
    void cancel(OrderId id);
    // Handle duplicates and out-of-order events without regressing terminal states; status is not a fill.
    void on_update(const OrderUpdate& update);
    // Deduplicate executions and update reservations, never double count cumulative status quantities.
    void on_fill(const Fill& fill);
    // Return all outstanding exposure including unknown submissions and pending cancellations.
    std::vector<OrderState> pending() const;
    // Match durable keys/broker IDs, detect external orders and block trading on unresolved differences.
    void reconcile(const BrokerSnapshot& snapshot);

private:
    Broker& broker_;
    OrderId next_id_{1};
    std::unordered_map<OrderId, OrderState> orders_;
};
