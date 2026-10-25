#include <cstdint>
#include <string>
#include <unordered_map>
#include <cmath>
#include <stdexcept>

#include "Portfolio.hpp"

Portfolio::Portfolio(double initial_cash)
    : cash_{initial_cash}, holdings_{} {
    if (!std::isfinite(initial_cash) || initial_cash < 0.0) {
        throw std::invalid_argument("Invalid initial cash");
    }
}

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
                     double quantity,
                     double price,
                     double fee) {
   
    if (!std::isfinite(quantity) || quantity <= 0.0 ||
        !std::isfinite(price)    || price <= 0.0 ||
        !std::isfinite(fee)      || fee < 0.0) {
        return false;
    }

    // Look up the position, false if does not exist / quantity > owned
    const auto it = holdings_.find(ticker);
    if (it == holdings_.end() || quantity > it->second.shares) {
        return false;
    };

    const double proceeds = quantity * price - fee;
    const double new_cash = cash_ + proceeds;

    if (new_cash < 0 || !std::isfinite(new_cash)) {
        return false;
    }
    
    if (quantity == it->second.shares) {
        holdings_.erase(it);
    } else {
        it->second.shares -= quantity;
        // Average purchase price stays unchanged.
    }

    cash_ = new_cash;
    return true;
}

double Portfolio::cash() noexcept {
    return cash_;
}

// Returns zero if the ticker is not held.
double Portfolio::shares(std::uint64_t ticker) {
    if (!holdings_.contains(ticker)) {
        return 0.0;
    }
    return holdings_[ticker].shares;
}

// Requires a current price for every held position.
double Portfolio::equity(const std::unordered_map<std::uint64_t, double>& prices) {
    double result{cash_};
    for (const auto& [ticker, position] : holdings_) {
        const double& curr_price = prices.at(ticker);
        result += curr_price * position.shares;
    }
    return result;
}

