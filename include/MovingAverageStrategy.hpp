#pragma once
#include "Strategy.hpp"
#include <deque>

namespace algo {
enum class StrategyKind { MovingAverage, Breakout, MeanReversion, BuyAndHold };
class MovingAverageStrategy : public Strategy {
  public:
    MovingAverageStrategy(std::uint64_t ticker, std::size_t window, double quantity,
                          StrategyKind kind = StrategyKind::MovingAverage, double threshold = 0.02);
    void reset() override;
    std::vector<Order> on_bar(const Bar &, const Portfolio &) override;

  private:
    std::uint64_t ticker_;
    std::size_t window_;
    double quantity_;
    StrategyKind kind_;
    double threshold_;
    std::deque<double> closes_;
};
} // namespace algo
