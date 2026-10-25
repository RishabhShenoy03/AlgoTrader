#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

class Portfolio {
private:
    struct Position {
        double shares{0.0};
        double avg_price{0.0};
    };
    
    double cash_;
    std::unordered_map<std::uint64_t, Position> holdings_;

public:
    explicit Portfolio(double initial_cash);

    bool buy(std::uint64_t ticker,
             double quantity,
             double price,
             double fee);

    bool sell(std::uint64_t ticker,
             double quantity,
             double price,
             double fee);  

    double cash() noexcept;

    // Returns zero if the ticker is not held.
    double shares(std::uint64_t ticker);

    // Requires a current price for every held position.
    double equity(const std::unordered_map<std::uint64_t, double>& prices);
};