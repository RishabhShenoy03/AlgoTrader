#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

explicit Portfolio(double initial_cash)
    : cash_{initial_cash}, holdings_{} 

bool Portfolio::buy(const std::string& ticker, std::int64_t quantity, double price, double fee) {

}

bool Portfolio::sell(const std::string& ticker, std::int64_t quantity, double price, double fee) {
    double new_avg_price =
    (old_shares * old_avg_price + bought_shares * buy_price)
    / (old_shares + bought_shares);
}

double Portfolio::cash() const noexcept {
    return cash_;
}

// Returns zero if the ticker is not held.
std::int64_t Portfolio::shares(const std::string& ticker) const {
    if (!holdings_.contains(ticker)) {
        return 0;
    }
    return holdings_[ticker].shares;
}

// Requires a current price for every held position.
double Portfolio::equity(const std::unordered_map<std::string, double>& prices) const {
    double result{};
    for (const auto& holding : holdings_) {
        result += holding.second.shares * holding.second.avg_price;
    }
    return result;
};

