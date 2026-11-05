#include "Portfolio.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace algo {
Portfolio::Portfolio(double cash, std::string currency) : cash_(cash), currency_(std::move(currency)) {
    if (!std::isfinite(cash) || cash < 0 || currency_.empty())
        throw std::invalid_argument("invalid initial cash/currency");
}
void Portfolio::add_instrument(const Instrument &i) {
    validate_instrument(i);
    if (i.currency != currency_)
        throw std::invalid_argument(
            "FX conversion is not supported; account and instruments must share currency");
    if (instruments_.contains(i.ticker))
        throw std::invalid_argument("duplicate instrument");
    instruments_.emplace(i.ticker, i);
}
const Instrument &Portfolio::instrument(std::uint64_t ticker) const {
    auto it = instruments_.find(ticker);
    if (it != instruments_.end())
        return it->second;
    if (!instruments_.empty())
        throw std::out_of_range("unknown instrument " + std::to_string(ticker));
    static const Instrument legacy = [] {
        Instrument i;
        i.ticker = 1;
        i.symbol = "LEGACY";
        i.lot_size = 0.00000001;
        return i;
    }();
    return legacy; // Compatibility for the original portfolio demo only.
}
const std::string &Portfolio::currency() const noexcept {
    return currency_;
}
double Portfolio::cash() const noexcept {
    return cash_;
}
double Portfolio::shares(std::uint64_t t) const {
    auto it = holdings_.find(t);
    return it == holdings_.end() ? 0 : it->second.shares;
}
std::vector<Portfolio::PositionSnapshot> Portfolio::positions() const {
    std::vector<PositionSnapshot> out;
    for (const auto &[id, p] : holdings_)
        out.push_back(p);
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.ticker < b.ticker; });
    return out;
}
double Portfolio::margin_used() const {
    double total = 0;
    for (const auto &[id, p] : holdings_)
        if (instrument(id).asset_class == AssetClass::Future)
            total += p.shares * instrument(id).initial_margin;
    return total;
}
void Portfolio::apply_execution(const Fill &f) {
    const auto &i = instrument(f.ticker);
    if (!valid_quantity(f.quantity, i.lot_size) || !std::isfinite(f.price) || f.price <= 0)
        throw std::invalid_argument("invalid execution quantity/price");
    auto old = holdings_.contains(f.ticker) ? holdings_.at(f.ticker) : PositionSnapshot{f.ticker, 0, 0};
    auto p = old;
    double next = cash_;
    if (f.side == Side::Buy) {
        p.shares += f.quantity;
        p.average_price = (old.shares * old.average_price + f.quantity * f.price) / p.shares;
        if (i.asset_class != AssetClass::Future)
            next -= f.quantity * f.price * i.multiplier;
    } else {
        if (f.quantity > old.shares + 1e-9)
            throw std::runtime_error("short positions are unsupported");
        p.shares = std::max(0.0, old.shares - f.quantity);
        next += f.quantity * i.multiplier *
                (i.asset_class == AssetClass::Future ? f.price - old.average_price : f.price);
    }
    if (!std::isfinite(next) || !std::isfinite(p.average_price) || !std::isfinite(p.shares))
        throw std::overflow_error("accounting overflow");
    if (p.shares > 1e-10)
        holdings_.insert_or_assign(f.ticker, p);
    else
        holdings_.erase(f.ticker);
    cash_ = next;
}
void Portfolio::apply_commission_delta(double fee) {
    if (!std::isfinite(fee) || !std::isfinite(cash_ - fee))
        throw std::invalid_argument("invalid commission");
    cash_ -= fee;
}
bool Portfolio::buy(std::uint64_t t, double q, double p, double fee) {
    if (!std::isfinite(fee) || fee < 0)
        return false;
    try {
        auto next = *this;
        next.apply_execution({"", 0, t, Side::Buy, q, p, ""});
        next.apply_commission_delta(fee);
        if (next.cash_ < next.margin_used() - 1e-9)
            return false;
        *this = std::move(next);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}
bool Portfolio::sell(std::uint64_t t, double q, double p, double fee) {
    if (!std::isfinite(fee) || fee < 0)
        return false;
    try {
        auto next = *this;
        next.apply_execution({"", 0, t, Side::Sell, q, p, ""});
        next.apply_commission_delta(fee);
        if (next.cash_ < 0)
            return false;
        *this = std::move(next);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}
double Portfolio::equity(const std::unordered_map<std::uint64_t, double> &prices) const {
    double result = cash_;
    for (const auto &[id, p] : holdings_) {
        const auto &i = instrument(id);
        double mark = prices.at(id);
        if (!std::isfinite(mark) || mark <= 0)
            throw std::invalid_argument("invalid mark");
        result +=
            p.shares * i.multiplier * (i.asset_class == AssetClass::Future ? mark - p.average_price : mark);
    }
    if (!std::isfinite(result))
        throw std::overflow_error("equity overflow");
    return result;
}
void Portfolio::restore(double cash, const std::vector<PositionSnapshot> &positions) {
    if (!std::isfinite(cash))
        throw std::invalid_argument("invalid restored cash");
    std::unordered_map<std::uint64_t, PositionSnapshot> state;
    for (const auto &p : positions) {
        if (!valid_quantity(p.shares, instrument(p.ticker).lot_size) || !std::isfinite(p.average_price) ||
            p.average_price <= 0 || !state.emplace(p.ticker, p).second)
            throw std::invalid_argument("invalid restored position");
    }
    holdings_ = std::move(state);
    cash_ = cash;
}
void Portfolio::settle_futures(const std::unordered_map<std::uint64_t, double> &prices) {
    auto next = *this;
    for (auto &[id, p] : next.holdings_)
        if (instrument(id).asset_class == AssetClass::Future) {
            double mark = prices.at(id);
            if (!std::isfinite(mark) || mark <= 0)
                throw std::invalid_argument("invalid settlement");
            next.cash_ += (mark - p.average_price) * p.shares * instrument(id).multiplier;
            p.average_price = mark;
        }
    if (!std::isfinite(next.cash_))
        throw std::overflow_error("settlement overflow");
    *this = std::move(next);
}
} // namespace algo
