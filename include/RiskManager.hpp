#pragma once
#include "Order.hpp"
#include "Portfolio.hpp"
#include <string>
#include <vector>

struct RiskLimits {
    double max_order_notional{};
    double max_position_notional{};
    double cash_buffer{};
};
struct RiskDecision {
    bool allowed{};
    std::string reason;
};

// Declaration-only scaffold; implementations are intentionally deferred.
class RiskManager {
public:
    // Validate all limits; remain halted until application reconciliation completes.
    explicit RiskManager(RiskLimits limits);
    // Reject invalid/stale values; include fees, slippage and pending exposure; enforce cash, long-only holdings, order and position limits.
    RiskDecision evaluate(const Order& intent, const Portfolio& portfolio, const std::vector<OrderState>& pending, double price, bool price_fresh) const;
    // Latch halt and record reason; cancellation remains available.
    void halt(std::string reason);
    // Only resume after an explicit application reconciliation gate.
    void resume();

private:
    RiskLimits limits_;
    bool halted_{true};
};
