#pragma once
#include "Order.hpp"
#include <string>

struct Fill {
    std::string execution_id; // Deduplicate before changing cash or holdings.
    OrderId order_id{};
    std::uint64_t ticker{};
    Side side{Side::Buy};
    double quantity{};
    double price{};
    std::string executed_at;
};

// Commissions may arrive after the execution or be corrected later.
struct Commission {
    std::string execution_id;
    double amount{}; // Total commission for this execution, not a delta.
    std::string currency;
};
