#pragma once
#include <cstdint>
#include <optional>
#include <string>

using OrderId = std::uint64_t;
enum class Side { Buy, Sell };
enum class OrderType { Market, Limit };
enum class OrderStatus { PendingSubmit, Submitted, PartiallyFilled, Filled, PendingCancel, Cancelled, Rejected, Unknown };

// Internal ticker IDs must be explicitly resolved to broker contracts.
struct Order {
    OrderId id{};
    std::string client_key; // Durable idempotency key, retained across reconnects.
    std::uint64_t ticker{};
    Side side{Side::Buy};
    OrderType type{OrderType::Market};
    double quantity{};
    std::optional<double> limit_price;
    std::string created_at; // UTC timestamp; replay uses a documented session clock.
};

struct OrderUpdate {
    OrderId order_id{};
    OrderStatus status{OrderStatus::Unknown};
    double filled_quantity{}; // Cumulative; never apply this as an execution.
    double remaining_quantity{};
    std::string reason;
};

struct OrderState {
    Order order;
    OrderStatus status{OrderStatus::PendingSubmit};
    double filled_quantity{};
    double reserved_cash{};
    double reserved_shares{};
};
