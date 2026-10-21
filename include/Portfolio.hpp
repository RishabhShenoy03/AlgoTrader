#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

class Portfolio {
public:
    struct Position {
        std::int64_t shares{0};
        double avg_price{0.0};
    };

    explicit Portfolio(double initial_cash);

    bool buy(const std::string& ticker,
             std::int64_t quantity,
             double price,
             double fee);

    bool sell(const std::string& ticker,
              std::int64_t quantity,
              double price,
              double fee);

    double cash() const noexcept;

    // Returns zero if the ticker is not held.
    std::int64_t shares(const std::string& ticker) const;

    // Requires a current price for every held position.
    double equity(
        const std::unordered_map<std::string, double>& prices
    ) const;

private:
    double cash_;
    std::unordered_map<std::string, Position> holdings_;
};