#include "TradingApp.hpp"
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace algo {
TradingApp::TradingApp(Broker &b, Strategy &s, Portfolio &p, OrderManager &o, FillProcessor &f,
                       RiskManager &r, EventJournal &j, double max_loss)
    : broker_(b), strategy_(s), portfolio_(p), orders_(o), fills_(f), risk_(r), journal_(j),
      max_session_loss_(max_loss) {
    if (!std::isfinite(max_loss) || max_loss <= 0 || max_loss >= 1)
        throw std::invalid_argument("invalid session loss limit");
}
TradingApp::~TradingApp() {
    broker_.set_callbacks({});
    broker_.disconnect();
}
void TradingApp::start() {
    journal_.open();
    const auto records = journal_.replay();
    // A previous nonempty session cannot be blindly resumed with a new process-local order map.
    // Require explicit operator reconciliation/new journal; never auto-resubmit uncertain intents.
    if (!records.empty())
        throw std::runtime_error("existing paper session journal: reconcile in TWS and use a new session "
                                 "journal; automatic broker recovery is not supported");
    journal_.append({0, "startup", "session", "paper"});
    BrokerCallbacks c;
    c.on_order = [this](const auto &u) {
        std::ostringstream payload;
        payload << std::setprecision(17) << u.order_id << ' ' << static_cast<int>(u.status) << ' '
                << u.filled_quantity << ' ' << u.remaining_quantity << ' ' << std::quoted(u.reason);
        journal_.append({0, "event", "status", payload.str()});
        orders_.on_update(u);
    };
    c.on_fill = [this](const auto &f) {
        std::ostringstream p;
        p << std::setprecision(17) << f.execution_id << ' ' << f.order_id << ' ' << f.ticker << ' '
          << static_cast<int>(f.side) << ' ' << f.quantity << ' ' << f.price;
        journal_.append({0, f.executed_at, "execution", p.str()});
        fills_.on_fill(f);
        orders_.on_fill(f);
    };
    c.on_commission = [this](const auto &f) {
        std::ostringstream p;
        p << std::setprecision(17) << f.execution_id << ' ' << f.amount << ' ' << f.currency;
        journal_.append({0, "event", "commission", p.str()});
        fills_.on_commission(f);
    };
    c.on_snapshot = [this](const auto &s) { reconcile(s); };
    c.on_bar = [this](const auto &b) { on_bar(b); };
    c.on_error = [this](const auto &e) {
        risk_.halt(e);
        reconciled_ = false;
        running_ = false;
        journal_.append({0, "event", "error", e});
    };
    broker_.set_callbacks(std::move(c));
    broker_.connect();
    running_ = true;
    broker_.request_snapshot();
}
void TradingApp::reconcile(const BrokerSnapshot &s) {
    if (!s.complete || s.currency != portfolio_.currency())
        throw std::runtime_error("incomplete snapshot or currency mismatch");
    orders_.reconcile(s);
    // First release requires a flat, isolated paper account. External activity is never adopted silently.
    if (!s.positions.empty() || !s.open_orders.empty())
        throw std::runtime_error(
            "paper startup requires a flat account with no open orders; reconcile external activity first");
    portfolio_.restore(s.cash, {});
    initial_equity_ = s.cash;
    reconciled_ = true;
    risk_.resume();
    journal_.append({0, "startup", "reconciled", s.account});
}
void TradingApp::on_bar(const Bar &b) {
    if (!running_ || !reconciled_ || !broker_.ready())
        return;
    if (!last_bar_.empty() && b.date <= last_bar_)
        return;
    last_bar_ = b.date;
    const auto &instrument = portfolio_.instrument(b.ticker);
    if (!instrument.expiry.empty() && b.date.substr(0, 10) >= instrument.expiry)
        throw std::runtime_error("contract expiry reached; reconcile position in TWS");
    if (portfolio_.equity({{b.ticker, b.close}}) < initial_equity_ * (1 - max_session_loss_)) {
        risk_.halt("session loss limit");
        throw std::runtime_error(
            "session loss limit reached; inspect holdings and outstanding orders in TWS");
    }
    for (auto o : strategy_.on_bar(b, portfolio_)) {
        o.created_at = b.date;
        auto d = risk_.evaluate(o, portfolio_, orders_.pending(), b.close, true);
        std::ostringstream payload;
        payload << std::setprecision(17) << o.ticker << ' ' << static_cast<int>(o.side) << ' '
                << static_cast<int>(o.type) << ' ' << o.quantity << ' ' << o.limit_price.value_or(0) << ' '
                << std::quoted(o.client_key) << ' ' << std::quoted(d.reason);
        journal_.append({0, b.date, d.allowed ? "intent" : "risk_rejection", payload.str()});
        if (d.allowed) {
            const auto id = orders_.submit(o, d.reserve_cash, d.reserve_shares);
            journal_.append({0, b.date, "submitted", std::to_string(id) + " " + o.client_key});
        }
    }
}
void TradingApp::run() {
    try {
        while (running_) {
            broker_.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        stop();
    } catch (...) {
        risk_.halt("event loop failed");
        running_ = false;
        broker_.disconnect();
        throw;
    }
}
void TradingApp::request_stop() noexcept {
    running_ = false;
}
void TradingApp::stop() {
    running_ = false;
    risk_.halt("stopped");
    for (const auto &s : orders_.pending())
        orders_.cancel(s.order.id);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!orders_.pending().empty() && broker_.ready() && std::chrono::steady_clock::now() < end)
        broker_.poll();
    journal_.append({0, "shutdown", "unresolved_orders", std::to_string(orders_.pending().size())});
    broker_.disconnect();
    journal_.flush();
}
} // namespace algo
