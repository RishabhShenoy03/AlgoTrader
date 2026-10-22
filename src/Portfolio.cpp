#include <cstdint>
#include <string>
#include <unordered_map>
#include <cmath>

#include "Portfolio.hpp"

explicit Portfolio(double initial_cash)
    : cash_{initial_cash}, holdings_{} 

bool Portfolio::buy(std::uint64_t ticker,
                    double quantity,
                    double price,
                    double fee) {
    // Validate inputs before changing anything.
    if (!std::isfinite(quantity) || quantity <= 0.0 ||
        !std::isfinite(price)    || price <= 0.0 ||
        !std::isfinite(fee)      || fee < 0.0) {
        return false;
    }

    const double cost = quantity * price + fee;

    if (!std::isfinite(cost) || cost > cash_) {
        return false;
    }

    // Look up the position without inserting one.
    const auto it = holdings_.find(ticker);

    const double old_shares =
        it != holdings_.end() ? it->second.shares : 0.0;

    const double old_avg_price =
        it != holdings_.end() ? it->second.avg_price : 0.0;

    const double new_shares = old_shares + quantity;

    const double new_avg_price =
        (old_shares * old_avg_price + quantity * price)
        / new_shares;

    if (!std::isfinite(new_shares) ||
        !std::isfinite(new_avg_price)) {
        return false;
    }

    // All checks passed: update the position and cash.
    holdings_.insert_or_assign(
        ticker, Position{new_shares, new_avg_price}
    );

    cash_ -= cost;
    return true;
}

bool Portfolio::sell(std::uint64_t ticker,
                     std::int64_t quantity,
                     double price,
                     double fee) {
   
    if (!std::isfinite(quantity) || quantity <= 0.0 ||
        !std::isfinite(price)    || price <= 0.0 ||
        !std::isfinite(fee)      || fee < 0.0) {
        return false;
    }

    // Look up the position without inserting one.
    const auto it = holdings_.find(ticker);
    if (it == holdings_.end() || quantity > it->second.shares) {
        return false;
    };

    const double proceeds = quantity * price - fee;
    const double new_cash = cash_ + proceeds;
    
    if (quantity == it->second.shares) {
        holdings_.erase(it);
    } else {
        it->second.shares -= quantity;
        // Average purchase price stays unchanged.
    }

    cash_ = new_cash;
    return true;
}

double Portfolio::cash() const noexcept {
    return cash_;
}

// Returns zero if the ticker is not held.
std::int64_t Portfolio::shares(std::uint64_t ticker) const {
    if (!holdings_.contains(ticker)) {
        return 0;
    }
    return holdings_[ticker].shares;
}

// Requires a current price for every held position.
double Portfolio::equity(const std::unordered_map<std::uint64_t, double>& prices) const {
    double result{};
    for (const auto& holding : holdings_) {
        result += holding.second.shares * holding.second.avg_price;
    }
    return result;
};

