#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace algo {

enum class AssetClass { Stock, Crypto, Future, Option };
// Single account currency. Prices are quoted per unit; multiplier converts to cash.
struct Instrument {
    std::uint64_t ticker{};
    std::string symbol;
    AssetClass asset_class{AssetClass::Stock};
    std::string currency{"USD"};
    double multiplier{1};
    double lot_size{1};
    double tick_size{0.01};
    double initial_margin{}; // Fixed cash per futures contract; user supplied, not exchange feed.
    std::string expiry;      // YYYY-MM-DD. Orders/holdings on or after expiry are rejected.
    double strike{};
    std::string right; // C/P for options.
    std::string exchange{"SMART"};
    long con_id{}; // IBKR-resolved contract identity; required for IBKR mode.
};
void validate_instrument(const Instrument &instrument);
std::vector<Instrument> load_instruments(const std::string &path);
bool valid_timestamp(const std::string &value);
bool valid_quantity(double quantity, double lot);
} // namespace algo
