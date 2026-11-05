#include "BrokerSim.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace algo {
BrokerSim::BrokerSim(double fee, double slip, double cap)
    : fee_per_fill_(fee), slippage_bps_(slip), max_fill_quantity_(cap) {
    if (!std::isfinite(fee) || fee < 0 || !std::isfinite(slip) || slip < 0 || slip >= 10000 ||
        !std::isfinite(cap) || cap < 0)
        throw std::invalid_argument("invalid execution costs");
}
void BrokerSim::configure_account(const Portfolio &p) {
    if (connected_ || !executions_.empty())
        throw std::logic_error("account already active");
    account_ = p;
}
void BrokerSim::connect() {
    connected_ = true;
}
void BrokerSim::disconnect() {
    connected_ = false;
}
bool BrokerSim::ready() const {
    return connected_;
}
void BrokerSim::set_callbacks(BrokerCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
}
void BrokerSim::poll() {}
void BrokerSim::submit(const Order &o) {
    if (!ready())
        throw std::runtime_error("simulator disconnected");
    if (!ids_.insert(o.id).second)
        throw std::runtime_error("duplicate order ID");
    const auto &i = account_.instrument(o.ticker);
    if (!valid_quantity(o.quantity, i.lot_size) ||
        (o.type == OrderType::Limit &&
         (!o.limit_price || !std::isfinite(*o.limit_price) || *o.limit_price <= 0))) {
        if (callbacks_.on_order)
            callbacks_.on_order({o.id, OrderStatus::Rejected, 0, o.quantity, "invalid order"});
        return;
    }
    pending_.push_back({o, OrderStatus::Submitted, 0, 0, 0});
    if (callbacks_.on_order)
        callbacks_.on_order({o.id, OrderStatus::Submitted, 0, o.quantity, ""});
}
void BrokerSim::cancel(OrderId id) {
    auto it =
        std::find_if(pending_.begin(), pending_.end(), [id](const auto &s) { return s.order.id == id; });
    if (it == pending_.end())
        return;
    auto s = *it;
    pending_.erase(it);
    if (callbacks_.on_order)
        callbacks_.on_order({id, OrderStatus::Cancelled, s.filled_quantity,
                             s.order.quantity - s.filled_quantity, "cancelled"});
}
void BrokerSim::on_bar_open(const Bar &b) {
    if (!ready())
        throw std::runtime_error("simulator disconnected");
    // Callbacks may cancel/submit; process a stable ID snapshot.
    std::vector<OrderId> ids;
    for (const auto &s : pending_)
        ids.push_back(s.order.id);
    for (auto id : ids) {
        auto it =
            std::find_if(pending_.begin(), pending_.end(), [id](const auto &s) { return s.order.id == id; });
        if (it == pending_.end())
            continue;
        auto s = *it;
        const auto &o = s.order;
        if (o.ticker != b.ticker || o.created_at >= b.date)
            continue;
        const auto &i = account_.instrument(o.ticker);
        if (!i.expiry.empty() && b.date.substr(0, 10) >= i.expiry) {
            cancel(id);
            continue;
        }
        double price = b.open * (1 + (o.side == Side::Buy ? 1 : -1) * slippage_bps_ / 10000);
        price = (o.side == Side::Buy ? std::ceil(price / i.tick_size - 1e-10)
                                     : std::floor(price / i.tick_size + 1e-10)) *
                i.tick_size;
        if (o.type == OrderType::Limit && ((o.side == Side::Buy && price > *o.limit_price) ||
                                           (o.side == Side::Sell && price < *o.limit_price)))
            continue;
        double quantity = o.quantity - s.filled_quantity;
        if (max_fill_quantity_ > 0)
            quantity = std::min(quantity, std::floor(max_fill_quantity_ / i.lot_size) * i.lot_size);
        if (quantity <= 0)
            throw std::runtime_error("partial fill cap is below instrument lot size");
        const bool accepted = o.side == Side::Buy ? account_.buy(o.ticker, quantity, price, fee_per_fill_)
                                                  : account_.sell(o.ticker, quantity, price, fee_per_fill_);
        if (!accepted) {
            pending_.erase(it);
            if (callbacks_.on_order)
                callbacks_.on_order({id, OrderStatus::Rejected, s.filled_quantity,
                                     o.quantity - s.filled_quantity,
                                     "insufficient cash/margin at execution price"});
            continue;
        }
        Fill f{"sim-" + std::to_string(next_execution_++), id, o.ticker, o.side, quantity, price, b.date};
        executions_.push_back(f);
        s.filled_quantity += quantity;
        s.status =
            s.filled_quantity >= o.quantity - 1e-9 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
        if (s.status == OrderStatus::Filled)
            pending_.erase(it);
        else
            *it = s;
        if (callbacks_.on_fill)
            callbacks_.on_fill(f);
        if (callbacks_.on_commission)
            callbacks_.on_commission({f.execution_id, fee_per_fill_, account_.currency()});
        if (callbacks_.on_order)
            callbacks_.on_order({id, s.status, s.filled_quantity, o.quantity - s.filled_quantity, ""});
    }
}
void BrokerSim::settle(const std::unordered_map<std::uint64_t, double> &prices) {
    account_.settle_futures(prices);
}
void BrokerSim::request_snapshot() {
    BrokerSnapshot s;
    s.account = "SIM";
    s.currency = account_.currency();
    s.cash = account_.cash();
    s.open_orders = pending_;
    s.executions = executions_;
    s.complete = true;
    for (const auto &p : account_.positions())
        s.positions.emplace(p.ticker, p.shares);
    if (callbacks_.on_snapshot)
        callbacks_.on_snapshot(s);
}
} // namespace algo
