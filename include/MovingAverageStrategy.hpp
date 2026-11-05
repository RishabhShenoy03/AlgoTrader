#pragma once
#include "Strategy.hpp"
#include <cstddef>
#include <deque>

// Declaration-only scaffold; implementations are intentionally deferred.
class MovingAverageStrategy : public Strategy {
public:
    // Validate window and quantity; configure one long-only instrument.
    MovingAverageStrategy(std::uint64_t ticker, std::size_t window, double quantity);
    // Clear rolling prices and crossover state.
    void reset() override;
    // Warm up; compute mean; emit crossover intents; size sells against holdings; risk layer accounts for pending orders.
    std::vector<Order> on_bar(const Bar& bar, const Portfolio& portfolio) override;

private:
    std::uint64_t ticker_;
    std::size_t window_;
    double quantity_;
    std::deque<double> closes_;
    double rolling_sum_{};
    bool previously_above_{};
};
