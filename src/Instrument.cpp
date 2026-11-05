#include "Instrument.hpp"
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace algo {
namespace {
double number(const std::string &s) {
    std::size_t n;
    double v = std::stod(s, &n);
    if (n != s.size() || !std::isfinite(v))
        throw std::invalid_argument("invalid number: " + s);
    return v;
}
} // namespace
bool valid_timestamp(const std::string &s) {
    if (s.size() != 10 && s.size() != 20)
        return false;
    if (s[4] != '-' || s[7] != '-')
        return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == 4 || i == 7 || (s.size() == 20 && (i == 10 || i == 13 || i == 16 || i == 19)))
            continue;
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    const auto d = std::chrono::year{std::stoi(s.substr(0, 4))} /
                   std::chrono::month{static_cast<unsigned>(std::stoi(s.substr(5, 2)))} /
                   std::chrono::day{static_cast<unsigned>(std::stoi(s.substr(8, 2)))};
    if (!d.ok())
        return false;
    return s.size() == 10 ||
           (s[10] == 'T' && s[13] == ':' && s[16] == ':' && s[19] == 'Z' && std::stoi(s.substr(11, 2)) < 24 &&
            std::stoi(s.substr(14, 2)) < 60 && std::stoi(s.substr(17, 2)) < 60);
}
bool valid_quantity(double q, double lot) {
    return std::isfinite(q) && q > 0 && std::isfinite(lot) && lot > 0 &&
           std::abs(q / lot - std::round(q / lot)) < 1e-7;
}
void validate_instrument(const Instrument &i) {
    if (i.ticker == 0 || i.symbol.empty() || i.currency.empty() || !std::isfinite(i.multiplier) ||
        i.multiplier <= 0 || !std::isfinite(i.lot_size) || i.lot_size <= 0 || !std::isfinite(i.tick_size) ||
        i.tick_size <= 0 || !std::isfinite(i.initial_margin) || i.initial_margin < 0)
        throw std::invalid_argument("invalid instrument metadata");
    if (i.asset_class == AssetClass::Future && (i.initial_margin <= 0 || i.expiry.empty() || i.lot_size != 1))
        throw std::invalid_argument("futures require margin, expiry, integer contracts");
    if (i.asset_class == AssetClass::Option &&
        (i.expiry.empty() || !std::isfinite(i.strike) || i.strike <= 0 ||
         (i.right != "C" && i.right != "P") || i.lot_size != 1))
        throw std::invalid_argument("options require expiry, strike, C/P, integer contracts");
    if (!i.expiry.empty() && (i.expiry.size() != 10 || !valid_timestamp(i.expiry)))
        throw std::invalid_argument("invalid expiry");
}
std::vector<Instrument> load_instruments(const std::string &path) {
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line))
        throw std::runtime_error("cannot read instruments: " + path);
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line != "ticker,symbol,asset,currency,multiplier,lot_size,tick_size,initial_margin,expiry,strike,"
                "right,exchange,con_id")
        throw std::runtime_error("invalid instrument CSV header");
    std::vector<Instrument> out;
    std::unordered_set<std::uint64_t> ids;
    std::size_t row = 1;
    while (std::getline(in, line)) {
        ++row;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        try {
            std::vector<std::string> f;
            std::istringstream stream(line);
            std::string x;
            while (std::getline(stream, x, ','))
                f.push_back(x);
            if (f.size() != 13 || line.back() == ',')
                throw std::runtime_error("expected 13 columns (use 0 for con_id)");
            Instrument i;
            const double id = number(f[0]);
            if (id < 1 || id > 2147483647 || std::floor(id) != id)
                throw std::runtime_error("invalid ticker");
            i.ticker = static_cast<std::uint64_t>(id);
            i.symbol = f[1];
            if (f[2] == "stock")
                i.asset_class = AssetClass::Stock;
            else if (f[2] == "crypto")
                i.asset_class = AssetClass::Crypto;
            else if (f[2] == "future")
                i.asset_class = AssetClass::Future;
            else if (f[2] == "option")
                i.asset_class = AssetClass::Option;
            else
                throw std::runtime_error("unknown asset class");
            i.currency = f[3];
            i.multiplier = number(f[4]);
            i.lot_size = number(f[5]);
            i.tick_size = number(f[6]);
            i.initial_margin = number(f[7]);
            i.expiry = f[8];
            i.strike = number(f[9]);
            i.right = f[10];
            i.exchange = f[11];
            const double con = number(f[12]);
            if (con < 0 || con > 2147483647 || std::floor(con) != con)
                throw std::runtime_error("invalid con_id");
            i.con_id = static_cast<long>(con);
            validate_instrument(i);
            if (!ids.insert(i.ticker).second)
                throw std::runtime_error("duplicate ticker");
            out.push_back(i);
        } catch (const std::exception &e) {
            throw std::runtime_error(path + ":" + std::to_string(row) + ": " + e.what());
        }
    }
    if (out.empty())
        throw std::runtime_error("empty instrument catalogue");
    return out;
}
} // namespace algo
