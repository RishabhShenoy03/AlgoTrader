#pragma once
#include "Order.hpp"
#include "Portfolio.hpp"
#include <vector>

namespace algo {
struct RiskLimits {
    double max_order_notional{100000};
    double max_position_notional{200000};
    double cash_buffer{};
    double fee_per_fill{1};
    double slippage_bps{5};
};
struct RiskDecision {
    bool allowed{};
    std::string reason;
    double reserve_cash{};
    double reserve_shares{};
};
class RiskManager {
  public:
    explicit RiskManager(RiskLimits limits);
    RiskDecision evaluate(const Order &, const Portfolio &, const std::vector<OrderState> &, double price,
                          bool price_fresh) const;
    void halt(std::string reason);
    void resume();

  private:
    RiskLimits limits_;
    bool halted_{true};
    std::string reason_{"not reconciled"};
};
} // namespace algo
