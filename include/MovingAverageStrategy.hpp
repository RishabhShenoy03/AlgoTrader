#pragma once
#include "Strategy.hpp"
#include <deque>
#include <optional>

namespace algo {
enum class StrategyKind { MovingAverage, Breakout, MeanReversion, BuyAndHold };
class MovingAverageStrategy : public Strategy {
  public:
    MovingAverageStrategy(std::uint64_t ticker, std::size_t window, double quantity,
                          StrategyKind kind = StrategyKind::MovingAverage, double threshold = 0.02);
    void reset() override;
    std::vector<Order> on_bar(const Bar &, const Portfolio &) override;

    // Read-only introspection for research and reporting. on_bar behaviour is unchanged.
    std::size_t window_size() const noexcept { return window_; }
    std::size_t observed() const noexcept { return closes_.size(); }
    bool warmed_up() const noexcept { return closes_.size() == window_; }
    // Mean of the retained prior closes; empty until the window is full.
    std::optional<double> prior_average() const;

  private:
    double average() const;
    std::uint64_t ticker_;
    std::size_t window_;
    double quantity_;
    StrategyKind kind_;
    double threshold_;
    std::deque<double> closes_;
};
} // namespace algo
