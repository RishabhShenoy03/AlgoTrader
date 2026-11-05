#include "OrderManager.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace algo {
namespace {
bool terminal(OrderStatus s) {
    return s == OrderStatus::Filled || s == OrderStatus::Cancelled || s == OrderStatus::Rejected;
}
} // namespace
OrderManager::OrderManager(Broker &b) : broker_(b) {}
OrderId OrderManager::submit(Order o, double cash, double shares) {
    if (!broker_.ready())
        throw std::runtime_error("broker not ready");
    if (!std::isfinite(o.quantity) || o.quantity <= 0 || !std::isfinite(cash) || cash < 0 ||
        !std::isfinite(shares) || shares < 0)
        throw std::invalid_argument("invalid order reservation");
    if (o.client_key.empty())
        o.client_key = "order-" + std::to_string(next_id_);
    if (!keys_.insert(o.client_key).second)
        throw std::invalid_argument("duplicate client order key");
    o.id = next_id_++;
    orders_.emplace(o.id, OrderState{o, OrderStatus::PendingSubmit, 0, cash, shares});
    try {
        broker_.submit(o);
    } catch (...) {
        orders_.at(o.id).status = OrderStatus::Unknown;
        throw;
    }
    return o.id;
}
void OrderManager::cancel(OrderId id) {
    auto &state = orders_.at(id);
    if (terminal(state.status))
        return;
    state.status = OrderStatus::PendingCancel;
    broker_.cancel(id);
}
void OrderManager::on_update(const OrderUpdate &u) {
    auto &s = orders_.at(u.order_id);
    if (!std::isfinite(u.filled_quantity) || u.filled_quantity < 0 ||
        u.filled_quantity > s.order.quantity + 1e-9)
        throw std::runtime_error("invalid cumulative fill");
    reported_fills_[u.order_id] = std::max(reported_fills_[u.order_id], u.filled_quantity);
    if (terminal(s.status))
        return;
    if (s.status == OrderStatus::PendingCancel && !terminal(u.status))
        return;
    if (s.status == OrderStatus::PartiallyFilled && u.status == OrderStatus::Submitted)
        return;
    s.status = u.status;
}
void OrderManager::on_fill(const Fill &f) {
    if (executions_.contains(f.execution_id))
        return;
    auto &s = orders_.at(f.order_id);
    if (f.execution_id.empty() || f.ticker != s.order.ticker || f.side != s.order.side ||
        !std::isfinite(f.quantity) || f.quantity <= 0 ||
        s.filled_quantity + f.quantity > s.order.quantity + 1e-9)
        throw std::runtime_error("execution does not match order");
    const double before = s.order.quantity - s.filled_quantity;
    s.filled_quantity += f.quantity;
    const double ratio = std::max(0.0, s.order.quantity - s.filled_quantity) / before;
    s.reserved_cash *= ratio;
    s.reserved_shares *= ratio;
    if (s.filled_quantity >= s.order.quantity - 1e-9)
        s.status = OrderStatus::Filled;
    else if (!terminal(s.status) && s.status != OrderStatus::PendingCancel)
        s.status = OrderStatus::PartiallyFilled;
    executions_.insert(f.execution_id);
}
std::vector<OrderState> OrderManager::all() const {
    std::vector<OrderState> out;
    for (const auto &[id, s] : orders_)
        out.push_back(s);
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.order.id < b.order.id; });
    return out;
}
std::vector<OrderState> OrderManager::pending() const {
    auto out = all();
    std::erase_if(out, [this](const auto &s) {
        double reported = reported_fills_.contains(s.order.id) ? reported_fills_.at(s.order.id) : 0;
        return terminal(s.status) && reported <= s.filled_quantity + 1e-9;
    });
    return out;
}
void OrderManager::reconcile(const BrokerSnapshot &snapshot) {
    if (!snapshot.complete)
        throw std::runtime_error("incomplete broker snapshot");
    const auto local = pending();
    if (local.size() != snapshot.open_orders.size())
        throw std::runtime_error("open order reconciliation mismatch");
    for (const auto &b : snapshot.open_orders) {
        auto it = orders_.find(b.order.id);
        if (it == orders_.end() || it->second.order.client_key != b.order.client_key ||
            it->second.order.ticker != b.order.ticker ||
            std::abs(it->second.filled_quantity - b.filled_quantity) > 1e-8)
            throw std::runtime_error("unrecognized or mismatched broker order");
    }
}
} // namespace algo
