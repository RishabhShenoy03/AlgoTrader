#include "Backtester.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace algo {
Backtester::Backtester(CsvDataFeed &f, BrokerSim &b, Strategy &s, Portfolio &p, OrderManager &o,
                       FillProcessor &fills, RiskManager &risk, PerformanceReport &r)
    : feed_(f), broker_(b), strategy_(s), portfolio_(p), orders_(o), fills_(fills), risk_(risk), report_(r) {}
Backtester::~Backtester() {
    broker_.set_callbacks({});
    broker_.disconnect();
}
const std::vector<std::string> &Backtester::decisions() const noexcept {
    return decisions_;
}
void Backtester::batch(const std::vector<Bar> &bars) {
    // Require a current mark for every open position; never silently carry stale prices.
    for (const auto &p : portfolio_.positions()) {
        if (std::none_of(bars.begin(), bars.end(), [&](const auto &b) { return b.ticker == p.ticker; }))
            throw std::runtime_error("missing bar for held instrument");
        const auto &i = portfolio_.instrument(p.ticker);
        if (!i.expiry.empty() && bars.front().date.substr(0, 10) >= i.expiry)
            throw std::runtime_error(
                "position reached expiry: close before expiry; exercise/assignment is not modeled");
    }
    for (const auto &b : bars) {
        portfolio_.instrument(b.ticker);
        broker_.on_bar_open(b);
        marks_[b.ticker] = b.close;
    }
    portfolio_.settle_futures(marks_);
    broker_.settle(marks_);
    if (portfolio_.cash() < portfolio_.margin_used())
        throw std::runtime_error("futures margin breach: run stopped (no invented forced liquidation)");
    report_.record(bars.front().date, portfolio_.equity(marks_));
    for (const auto &b : bars)
        for (auto intent : strategy_.on_bar(b, portfolio_)) {
            intent.created_at = b.date;
            const auto d = risk_.evaluate(intent, portfolio_, orders_.pending(), b.close, true);
            decisions_.push_back(b.date + "," + std::to_string(b.ticker) + "," +
                                 (d.allowed ? "approved" : "rejected") + "," + d.reason);
            if (d.allowed)
                orders_.submit(std::move(intent), d.reserve_cash, d.reserve_shares);
        }
}
void Backtester::run() {
    if (ran_)
        throw std::logic_error("create fresh backtest objects for each run");
    if (!portfolio_.positions().empty())
        throw std::invalid_argument("backtest requires a flat starting portfolio");
    ran_ = true;
    broker_.configure_account(portfolio_);
    BrokerCallbacks c;
    c.on_order = [this](const auto &u) { orders_.on_update(u); };
    c.on_fill = [this](const auto &f) {
        fills_.on_fill(f);
        orders_.on_fill(f);
    };
    c.on_commission = [this](const auto &fee) { fills_.on_commission(fee); };
    c.on_snapshot = [this](const auto &s) {
        orders_.reconcile(s);
        if (std::abs(s.cash - portfolio_.cash()) > 1e-7)
            throw std::runtime_error("simulator cash reconciliation failed");
        for (const auto &p : portfolio_.positions())
            if (!s.positions.contains(p.ticker) || std::abs(s.positions.at(p.ticker) - p.shares) > 1e-8)
                throw std::runtime_error("simulator position mismatch");
        if (s.positions.size() != portfolio_.positions().size())
            throw std::runtime_error("simulator position count mismatch");
    };
    broker_.set_callbacks(std::move(c));
    broker_.connect();
    risk_.resume();
    strategy_.reset();
    feed_.open();
    report_.reset();
    report_.record("initial", portfolio_.cash());
    std::vector<Bar> group;
    while (auto b = feed_.next()) {
        if (!group.empty() && group.front().date != b->date) {
            batch(group);
            group.clear();
        }
        group.push_back(*b);
    }
    if (group.empty())
        throw std::runtime_error("empty bar data");
    batch(group);
    // Cancel final unfilled orders, without fabricating fills or liquidation.
    for (const auto &s : orders_.pending())
        orders_.cancel(s.order.id);
    broker_.request_snapshot();
    broker_.disconnect();
}
} // namespace algo
