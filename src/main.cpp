#include "Portfolio.hpp"
#include <iostream>

int main() {
    // 1. Start with $1,000.
    Portfolio portfolio{1000.0};

    // 2. Buy 2.5 shares of ticker 1 at $100 each, with a $1 fee.
    const bool bought = portfolio.buy(1, 2.5, 100.0, 1.0);

    std::cout << "Buy succeeded: " << bought << '\n';
    std::cout << "Cash: " << portfolio.cash() << '\n';
    std::cout << "Shares: " << portfolio.shares(1) << '\n';

    // 3. Suppose ticker 1 now trades at $110.
    std::unordered_map<std::uint64_t, double> prices{
        {1, 110.0}
    };

    std::cout << "Equity: " << portfolio.equity(prices) << '\n';
}