#pragma once
#include "Bar.hpp"
#include "Order.hpp"
#include "Portfolio.hpp"
#include <vector>

// Declaration-only scaffold; implementations are intentionally deferred.
class Strategy {
public:
    virtual ~Strategy() = default;
    // Clear all history before a run.
    virtual void reset() = 0;
    // Return intents from information available at bar close only; OrderManager assigns IDs.
    virtual std::vector<Order> on_bar(const Bar& bar, const Portfolio& portfolio) = 0;
};
