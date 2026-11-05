#include "MovingAverageStrategy.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace algo {
MovingAverageStrategy::MovingAverageStrategy(std::uint64_t ticker, std::size_t window, double quantity,
                                             StrategyKind kind, double threshold)
    : ticker_(ticker), window_(window), quantity_(quantity), kind_(kind), threshold_(threshold) {
    if (ticker == 0 || window < 2 || !std::isfinite(quantity) || quantity <= 0 || !std::isfinite(threshold) ||
        threshold <= 0 || threshold >= 1)
        throw std::invalid_argument("invalid strategy parameters");
}
void MovingAverageStrategy::reset() {
    closes_.clear();
}
std::vector<Order> MovingAverageStrategy::on_bar(const Bar &b, const Portfolio &p) {
    if (b.ticker != ticker_)
        return {};
    bool buy = false, sell = false;
    if (kind_ == StrategyKind::BuyAndHold)
        buy = true;
    else if (closes_.size() == window_) {
        const double mean =
            std::accumulate(closes_.begin(), closes_.end(), 0.0) / static_cast<double>(window_);
        if (kind_ == StrategyKind::MovingAverage) {
            buy = b.close > mean;
            sell = b.close < mean;
        }
        if (kind_ == StrategyKind::Breakout) {
            buy = b.close > *std::max_element(closes_.begin(), closes_.end());
            sell = b.close < mean;
        }
        if (kind_ == StrategyKind::MeanReversion) {
            buy = b.close < mean * (1 - threshold_);
            sell = b.close >= mean;
        }
    }
    closes_.push_back(b.close);
    if (closes_.size() > window_)
        closes_.pop_front();
    const double held = p.shares(ticker_);
    if ((buy && held < 1e-9) || (sell && held > 0)) {
        Order o;
        o.ticker = ticker_;
        o.side = buy ? Side::Buy : Side::Sell;
        o.quantity = buy ? quantity_ : held;
        o.created_at = b.date;
        o.client_key = b.date + "/" + std::to_string(ticker_) + "/" + (buy ? "buy" : "sell");
        return {o};
    }
    return {};
}
} // namespace algo
