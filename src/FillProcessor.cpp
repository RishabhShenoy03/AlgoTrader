#include "FillProcessor.hpp"
#include <cmath>
#include <stdexcept>

namespace algo {
FillProcessor::FillProcessor(Portfolio &p) : portfolio_(p) {}
void FillProcessor::on_fill(const Fill &f) {
    if (f.execution_id.empty())
        throw std::invalid_argument("missing execution ID");
    auto it = applied_.find(f.execution_id);
    if (it != applied_.end()) {
        const auto &old = it->second;
        if (old.order_id != f.order_id || old.ticker != f.ticker || old.side != f.side ||
            old.price != f.price || old.quantity != f.quantity)
            throw std::runtime_error("execution correction needs reconciliation");
        return;
    }
    auto next = portfolio_;
    next.apply_execution(f);
    double fee = 0;
    if (commissions_.contains(f.execution_id)) {
        fee = commissions_.at(f.execution_id).amount;
        next.apply_commission_delta(fee);
    }
    portfolio_ = std::move(next);
    applied_.emplace(f.execution_id, f);
    fills_.push_back(f);
    fees_ += fee;
}
void FillProcessor::on_commission(const Commission &c) {
    if (c.execution_id.empty() || !std::isfinite(c.amount) || c.amount < 0 ||
        c.currency != portfolio_.currency())
        throw std::invalid_argument("invalid commission/currency");
    const double previous =
        commissions_.contains(c.execution_id) ? commissions_.at(c.execution_id).amount : 0;
    if (applied_.contains(c.execution_id)) {
        portfolio_.apply_commission_delta(c.amount - previous);
        fees_ += c.amount - previous;
    }
    commissions_.insert_or_assign(c.execution_id, c);
}
const std::vector<Fill> &FillProcessor::executions() const noexcept {
    return fills_;
}
double FillProcessor::fees() const noexcept {
    return fees_;
}
} // namespace algo
