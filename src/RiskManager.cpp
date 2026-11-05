#include "RiskManager.hpp"
#include <cmath>
#include <stdexcept>

namespace algo {
RiskManager::RiskManager(RiskLimits l) : limits_(l) {
    for (double x :
         {l.max_order_notional, l.max_position_notional, l.cash_buffer, l.fee_per_fill, l.slippage_bps})
        if (!std::isfinite(x) || x < 0)
            throw std::invalid_argument("invalid risk limits");
    if (l.max_order_notional == 0 || l.max_position_notional == 0 || l.slippage_bps >= 10000)
        throw std::invalid_argument("invalid risk limits");
}
void RiskManager::halt(std::string reason) {
    halted_ = true;
    reason_ = std::move(reason);
}
void RiskManager::resume() {
    halted_ = false;
    reason_.clear();
}
RiskDecision RiskManager::evaluate(const Order &o, const Portfolio &p, const std::vector<OrderState> &pending,
                                   double price, bool fresh) const {
    auto no = [](std::string reason) { return RiskDecision{false, std::move(reason), 0, 0}; };
    if (halted_)
        return no(reason_);
    if (!fresh || !std::isfinite(price) || price <= 0)
        return no("stale or invalid price");
    const auto &i = p.instrument(o.ticker);
    if (!valid_quantity(o.quantity, i.lot_size))
        return no("quantity violates lot size");
    if (!i.expiry.empty() && o.created_at.substr(0, 10) >= i.expiry)
        return no("expiry reached; exercise/assignment unsupported");
    if (o.type == OrderType::Limit &&
        (!o.limit_price || !std::isfinite(*o.limit_price) || *o.limit_price <= 0))
        return no("invalid limit price");
    const double estimate =
        (o.type == OrderType::Limit ? *o.limit_price : price * (1 + limits_.slippage_bps / 10000.0));
    const double notional = o.quantity * estimate * i.multiplier;
    if (!std::isfinite(notional) || notional > limits_.max_order_notional)
        return no("order notional limit");
    double reserved = 0, same_buys = 0, same_sells = 0;
    for (const auto &s : pending) {
        reserved += s.reserved_cash;
        if (s.order.ticker == o.ticker) {
            if (s.order.side == Side::Buy)
                same_buys += s.order.quantity - s.filled_quantity;
            else
                same_sells += s.order.quantity - s.filled_quantity;
        }
    }
    // One outstanding order per instrument prevents duplicate strategy intents.
    if (same_buys > 0 || same_sells > 0)
        return no("order already pending for instrument");
    if (o.side == Side::Sell) {
        if (o.quantity > p.shares(o.ticker) - same_sells + 1e-9)
            return no("short selling unsupported");
        return {true, "approved", 0, o.quantity};
    }
    if ((p.shares(o.ticker) + same_buys + o.quantity) * estimate * i.multiplier >
        limits_.max_position_notional)
        return no("position notional limit");
    const double required = (i.asset_class == AssetClass::Future ? o.quantity * i.initial_margin : notional) +
                            limits_.fee_per_fill;
    if (required + reserved + limits_.cash_buffer > p.cash() - p.margin_used() + 1e-9)
        return no("insufficient available cash/margin");
    return {true, "approved", required, 0};
}
} // namespace algo
