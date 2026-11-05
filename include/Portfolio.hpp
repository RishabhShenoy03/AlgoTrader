#pragma once
#include "Fill.hpp"
#include "Instrument.hpp"
#include <unordered_map>
#include <vector>

namespace algo {

class Portfolio {
  public:
    struct PositionSnapshot {
        std::uint64_t ticker;
        double shares;
        double average_price;
    };
    explicit Portfolio(double initial_cash, std::string currency = "USD");
    void add_instrument(const Instrument &instrument);
    const Instrument &instrument(std::uint64_t ticker) const;
    const std::string &currency() const noexcept;
    bool buy(std::uint64_t ticker, double quantity, double price, double fee);
    bool sell(std::uint64_t ticker, double quantity, double price, double fee);
    double cash() const noexcept;
    double shares(std::uint64_t ticker) const;
    std::vector<PositionSnapshot> positions() const;
    double equity(const std::unordered_map<std::uint64_t, double> &prices) const;
    double margin_used() const;
    void apply_execution(const Fill &fill);
    void apply_commission_delta(double amount);
    void restore(double cash, const std::vector<PositionSnapshot> &positions);
    void settle_futures(const std::unordered_map<std::uint64_t, double> &prices);

  private:
    double cash_;
    std::string currency_;
    std::unordered_map<std::uint64_t, PositionSnapshot> holdings_;
    std::unordered_map<std::uint64_t, Instrument> instruments_;
};
} // namespace algo
