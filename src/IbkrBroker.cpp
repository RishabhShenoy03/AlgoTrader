#include "IbkrBroker.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#ifdef ALGO_WITH_IBKR
#include "CommissionAndFeesReport.h"
#include "Contract.h"
#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "Execution.h"
#include "Order.h"
#include "OrderCancel.h"
#include "OrderState.h"
#include "bar.h"
#endif
namespace algo {
namespace {
void validate_config(const IbkrConfig &c) {
    if (c.host.empty() || c.port < 1 || c.port > 65535 || c.client_id <= 0 ||
        !c.paper_account.starts_with("DU") || c.currency.empty() || c.timeout_seconds < 1 ||
        c.stale_seconds < 10)
        throw std::invalid_argument(
            "IBKR requires host, valid port, positive client ID and explicit DU paper account");
}
} // namespace
#ifdef ALGO_WITH_IBKR
struct IbkrBroker::Impl : DefaultEWrapper {
    IbkrConfig config;
    BrokerCallbacks callbacks;
    EReaderOSSignal signal{100};
    EClientSocket client{this, &signal};
    std::unique_ptr<EReader> reader;
    std::unordered_map<std::uint64_t, Instrument> instruments;
    std::unordered_map<std::uint64_t, Contract> contracts;
    std::unordered_map<int, std::uint64_t> requests;
    std::unordered_map<int, algo::Order> orders;
    std::unordered_map<OrderId, int> order_ids;
    std::unordered_map<std::uint64_t, int> subscriptions;
    std::unordered_map<int, int> contract_matches;
    std::unordered_set<std::uint64_t> verified_contracts;
    int quote_request{-1};
    IbkrQuote watchlist_quote;
    int history_request{-1};
    bool history_done{};
    std::vector<algo::Bar> history;
    BrokerSnapshot snapshot;
    int next_order{-1}, next_request{100};
    bool verified_account{}, position_end{}, account_end{}, order_end{}, cash_seen{}, synchronized{};
    std::string fatal;
    std::chrono::steady_clock::time_point last_data = std::chrono::steady_clock::now();
    explicit Impl(IbkrConfig c) : config(std::move(c)) {}
    void fail(const std::string &message) {
        synchronized = false;
        fatal = message;
    }
    std::uint64_t ticker(long con_id) const {
        for (const auto &[id, i] : instruments)
            if (i.con_id == con_id)
                return id;
        throw std::runtime_error("broker returned unknown contract " + std::to_string(con_id));
    }
    void completed() {
        if (synchronized || !position_end || !account_end || !order_end || !cash_seen || !fatal.empty() ||
            verified_contracts.size() != instruments.size())
            return;
        snapshot.complete = true;
        synchronized = true;
        if (callbacks.on_snapshot)
            callbacks.on_snapshot(snapshot);
    }
    void nextValidId(int id) override {
        next_order = std::max(next_order, id);
    }
    void managedAccounts(const std::string &accounts) override {
        std::istringstream in(accounts);
        std::string a;
        while (std::getline(in, a, ','))
            if (a == config.paper_account)
                verified_account = true;
        if (!verified_account)
            fail("configured paper account not present in managedAccounts");
    }
    void connectionClosed() override {
        fail("TWS disconnected; reconcile before restarting");
    }
    void error(int id, long long, int code, const std::string &message, const std::string &) override {
        if (code == 2104 || code == 2106 || code == 2158 || code == 2107 || code == 2108)
            return;
        if (code == 202 && orders.contains(id)) {
            if (callbacks.on_notice)
                callbacks.on_notice("IBKR 202 order " + std::to_string(orders.at(id).id) + ": " + message);
            return; // Preserve explanation; cancellation is confirmed by orderStatus.
        }
        if (code == 201 && orders.contains(id)) {
            const auto &o = orders.at(id);
            if (callbacks.on_order)
                callbacks.on_order({o.id, OrderStatus::Rejected, 0, o.quantity, message});
            return;
        }
        if (quote_request >= 0 && id == quote_request && (code == 10167 || code == 10090)) {
            if (callbacks.on_notice)
                callbacks.on_notice("IBKR " + std::to_string(code) + ": " + message);
            return; // Partial/delayed data notice; actual prices must still arrive.
        }
        if (code == 2188 && history_request >= 0 && (id == history_request || id == -1)) {
            if (callbacks.on_notice)
                callbacks.on_notice("IBKR 2188: " + message + " Waiting for historical bars/end marker.");
            return; // Advisory about freshness, not evidence that historical bars were rejected.
        }
        std::string hint;
        if (code == 420 || code == 354 || code == 10089 || code == 10091 || code == 10168 || code == 2188)
            hint = " Data entitlement missing. Use ibkr-quotes for explicitly delayed watchlist data; "
                   "this does not enable live bars. Verify API subscriptions/data sharing for the paper login. "
                   "See docs/IBKR.md for the distinction between quotes, bars and historical data.";
        fail("IBKR error " + std::to_string(code) + ": " + message + hint);
    }
    void accountSummary(int, const std::string &account, const std::string &tag, const std::string &value,
                        const std::string &currency) override {
        if (account != config.paper_account || tag != "TotalCashValue")
            return;
        if (currency != config.currency) {
            fail("account base currency differs from configured currency");
            return;
        }
        std::size_t n;
        double cash = std::stod(value, &n);
        if (n != value.size() || !std::isfinite(cash)) {
            fail("invalid account cash");
            return;
        }
        snapshot.cash = cash;
        cash_seen = true;
    }
    void accountSummaryEnd(int) override {
        account_end = true;
        if (!cash_seen)
            fail("no cash value for configured account/currency");
        completed();
    }
    void position(const std::string &account, const Contract &contract, Decimal quantity, double) override {
        if (account != config.paper_account || synchronized)
            return;
        const double q = DecimalFunctions::decimalToDouble(quantity);
        if (std::abs(q) > 1e-9)
            snapshot.positions[ticker(contract.conId)] = q;
    }
    void positionEnd() override {
        position_end = true;
        completed();
    }
    void openOrder(int id, const Contract &contract, const ::Order &order, const ::OrderState &) override {
        next_order = std::max(next_order, id + 1);
        if (order.account != config.paper_account)
            return;
        if (synchronized) {
            if (!orders.contains(id))
                fail("external order detected in paper account");
            return;
        }
        algo::Order o;
        o.id = static_cast<OrderId>(id);
        o.ticker = ticker(contract.conId);
        o.client_key = order.orderRef;
        o.quantity = DecimalFunctions::decimalToDouble(order.totalQuantity);
        o.side = order.action == "BUY" ? Side::Buy : Side::Sell;
        snapshot.open_orders.push_back({o, OrderStatus::Submitted, 0, 0, 0});
    }
    void openOrderEnd() override {
        order_end = true;
        completed();
    }
    void contractDetails(int req, const ContractDetails &details) override {
        if (!requests.contains(req))
            return;
        auto id = requests.at(req);
        const auto &i = instruments.at(id);
        const auto &c = details.contract;
        ++contract_matches[req];
        const std::string type = i.asset_class == AssetClass::Stock    ? "STK"
                                 : i.asset_class == AssetClass::Crypto ? "CRYPTO"
                                 : i.asset_class == AssetClass::Future ? "FUT"
                                                                       : "OPT";
        if (c.conId != i.con_id || c.currency != i.currency || c.secType != type || c.symbol != i.symbol) {
            fail("contract metadata mismatch; check catalogue against TWS");
            return;
        }
        if ((i.asset_class == AssetClass::Future || i.asset_class == AssetClass::Option) &&
            (c.multiplier.empty() || std::abs(std::stod(c.multiplier) - i.multiplier) > 1e-8)) {
            fail("contract multiplier mismatch");
            return;
        }
        if (!i.expiry.empty()) {
            std::string expiry = i.expiry;
            std::erase(expiry, '-');
            if (c.lastTradeDateOrContractMonth.substr(0, 8) != expiry) {
                fail("contract expiry mismatch");
                return;
            }
        }
        if (i.asset_class == AssetClass::Option &&
            (c.right != i.right || std::abs(c.strike - i.strike) > 1e-8)) {
            fail("option strike/right mismatch");
            return;
        }
        if (!std::isfinite(details.minTick) || details.minTick <= 0 || i.tick_size + 1e-9 < details.minTick) {
            fail("configured tick size is below contract minimum tick");
            return;
        }
        contracts[id] = c;
    }
    void contractDetailsEnd(int req) override {
        if (contract_matches[req] != 1)
            fail("contract resolution must return exactly one match");
        else if (fatal.empty())
            verified_contracts.insert(requests.at(req));
        completed();
    }
    void marketDataType(int req, int type) override {
        if (req == quote_request)
            watchlist_quote.market_data_type = type;
    }
    void tickPrice(int req, TickType field, double price, const TickAttrib &) override {
        if (req != quote_request || !std::isfinite(price) || price <= 0)
            return;
        // IBKR tick identifiers: bid/ask/last 1/2/4, delayed 66/67/68.
        if (field == 1 || field == 66) watchlist_quote.bid = price;
        if (field == 2 || field == 67) watchlist_quote.ask = price;
        if (field == 4 || field == 68) watchlist_quote.last = price;
        if (field == 66 || field == 67 || field == 68) {
            if (watchlist_quote.market_data_type != 4)
                watchlist_quote.market_data_type = 3;
        }
    }
    void historicalData(int req, const ::Bar &b) override {
        if (req != history_request)
            return;
        std::string date;
        if (b.time.size() == 8 && b.time.find_first_not_of("0123456789") == std::string::npos) {
            date = b.time.substr(0, 4) + "-" + b.time.substr(4, 2) + "-" + b.time.substr(6, 2);
        } else {
            std::size_t used;
            auto epoch = std::stoll(b.time, &used);
            if (used != b.time.size()) {
                fail("unexpected historical timestamp");
                return;
            }
            std::time_t time = static_cast<std::time_t>(epoch);
            std::tm utc{};
            gmtime_r(&time, &utc);
            std::ostringstream out;
            out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
            date = out.str();
        }
        if (!valid_timestamp(date) || !std::isfinite(b.open) || !std::isfinite(b.close) || b.open <= 0 ||
            b.close <= 0) {
            fail("invalid historical bar");
            return;
        }
        history.push_back({date, requests.at(req), b.open, b.close});
    }
    void historicalDataEnd(int req, const std::string &, const std::string &) override {
        if (req == history_request)
            history_done = true;
    }
    void orderStatus(int id, const std::string &status, Decimal filled, Decimal remaining, double, long long,
                     int, double, int, const std::string &why, double) override {
        if (!orders.contains(id))
            return;
        OrderStatus state = OrderStatus::Unknown;
        if (status == "Submitted" || status == "PreSubmitted")
            state = OrderStatus::Submitted;
        else if (status == "Filled")
            state = OrderStatus::Filled;
        else if (status == "Cancelled" || status == "ApiCancelled")
            state = OrderStatus::Cancelled;
        else if (status == "Inactive")
            state = OrderStatus::Rejected;
        else if (status == "PendingCancel")
            state = OrderStatus::PendingCancel;
        else if (status == "PendingSubmit" || status == "ApiPending")
            state = OrderStatus::PendingSubmit;
        const double q = DecimalFunctions::decimalToDouble(filled);
        if (state == OrderStatus::Submitted && q > 0)
            state = OrderStatus::PartiallyFilled;
        if (callbacks.on_order)
            callbacks.on_order(
                {orders.at(id).id, state, q, DecimalFunctions::decimalToDouble(remaining), why});
    }
    void execDetails(int, const Contract &contract, const Execution &e) override {
        if (e.acctNumber != config.paper_account)
            return;
        if (!orders.contains(e.orderId)) {
            fail("unrecognized execution: external activity or recovery required");
            return;
        }
        const auto &o = orders.at(e.orderId);
        if (callbacks.on_fill)
            callbacks.on_fill({e.execId, o.id, ticker(contract.conId),
                               e.side == "BOT" ? Side::Buy : Side::Sell,
                               DecimalFunctions::decimalToDouble(e.shares), e.price, e.time});
    }
    void commissionAndFeesReport(const CommissionAndFeesReport &c) override {
        if (callbacks.on_commission)
            callbacks.on_commission({c.execId, c.commissionAndFees, c.currency});
    }
    void realtimeBar(int req, long long time, double open, double, double, double close, Decimal, Decimal,
                     int) override {
        if (!requests.contains(req))
            return;
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        if (std::abs(static_cast<long long>(now) - (time + 5)) > config.stale_seconds) {
            fail("stale or future market data timestamp");
            return;
        }
        if (!std::isfinite(open) || !std::isfinite(close) || open <= 0 || close <= 0) {
            fail("invalid market data");
            return;
        }
        last_data = std::chrono::steady_clock::now();
        std::time_t end = static_cast<std::time_t>(time + 5);
        std::tm utc{};
        gmtime_r(&end, &utc);
        std::ostringstream date;
        date << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
        if (callbacks.on_bar)
            callbacks.on_bar({date.str(), requests.at(req), open, close});
    }
    void pump() {
        if (reader && client.isConnected()) {
            signal.waitForSignal();
            reader->processMsgs();
        }
        if (!fatal.empty())
            throw std::runtime_error(fatal);
    }
};
bool IbkrBroker::available() noexcept {
    return true;
}
IbkrBroker::IbkrBroker(IbkrConfig c) : impl_(std::make_unique<Impl>(c)) {
    validate_config(c);
}
IbkrBroker::~IbkrBroker() {
    disconnect();
}
void IbkrBroker::set_callbacks(BrokerCallbacks c) {
    impl_->callbacks = std::move(c);
}
void IbkrBroker::add_instrument(const Instrument &i) {
    validate_instrument(i);
    if (i.con_id <= 0 || i.currency != impl_->config.currency)
        throw std::invalid_argument("IBKR requires positive con_id and matching currency");
    if (!impl_->instruments.emplace(i.ticker, i).second)
        throw std::invalid_argument("duplicate instrument");
}
void IbkrBroker::connect() {
    auto &p = *impl_;
    if (p.client.isConnected())
        throw std::logic_error("already connected");
    p.fatal.clear();
    p.verified_account = false;
    p.next_order = -1;
    if (!p.client.eConnect(p.config.host.c_str(), p.config.port, p.config.client_id))
        throw std::runtime_error("cannot connect to paper TWS/Gateway; check API settings and port");
    p.reader = std::make_unique<EReader>(&p.client, &p.signal);
    p.reader->start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(p.config.timeout_seconds);
    try {
        while (p.next_order < 0 || !p.verified_account) {
            p.pump();
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("IBKR handshake/account verification timed out");
        }
    } catch (...) {
        disconnect();
        throw;
    }
}
void IbkrBroker::disconnect() {
    if (impl_) {
        impl_->synchronized = false;
        impl_->client.eDisconnect();
        impl_->reader.reset();
    }
}
bool IbkrBroker::ready() const {
    return impl_->client.isConnected() && impl_->synchronized && impl_->fatal.empty();
}
void IbkrBroker::request_snapshot() {
    auto &p = *impl_;
    if (!p.client.isConnected() || !p.verified_account)
        throw std::runtime_error("IBKR not connected");
    p.synchronized = false;
    p.position_end = p.account_end = p.order_end = p.cash_seen = false;
    p.snapshot = {};
    p.snapshot.account = p.config.paper_account;
    p.snapshot.currency = p.config.currency;
    p.client.reqPositions();
    p.client.reqAllOpenOrders();
    p.client.reqAccountSummary(1, "All", "TotalCashValue");
    for (const auto &[id, i] : p.instruments) {
        int req = p.next_request++;
        p.requests[req] = id;
        Contract c;
        c.conId = i.con_id;
        c.exchange = i.exchange;
        p.client.reqContractDetails(req, c);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(p.config.timeout_seconds);
    while (!p.synchronized) {
        p.pump();
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("IBKR snapshot/contract resolution timed out");
    }
    p.client.cancelAccountSummary(1);
}
void IbkrBroker::subscribe(std::uint64_t id) {
    auto &p = *impl_;
    if (!ready())
        throw std::runtime_error("not synchronized");
    if (p.subscriptions.contains(id))
        return;
    int req = p.next_request++;
    p.requests[req] = id;
    p.subscriptions[id] = req;
    p.last_data = std::chrono::steady_clock::now();
    p.client.reqRealTimeBars(req, p.contracts.at(id), 5, "TRADES", true, TagValueListSPtr{});
}
void IbkrBroker::unsubscribe(std::uint64_t id) {
    auto &p = *impl_;
    if (!p.subscriptions.contains(id))
        return;
    p.client.cancelRealTimeBars(p.subscriptions.at(id));
    p.requests.erase(p.subscriptions.at(id));
    p.subscriptions.erase(id);
}
IbkrQuote IbkrBroker::quote(std::uint64_t id, int type) {
    auto &p = *impl_;
    if (!p.config.read_only)
        throw std::runtime_error("watchlist quote diagnostics require read-only mode");
    if (!ready() || type < 1 || type > 4)
        throw std::invalid_argument("quote requires synchronization and market-data-type 1..4");
    const auto &contract = p.contracts.at(id);
    p.watchlist_quote = {};
    p.watchlist_quote.ticker = id;
    const int req = p.next_request++;
    p.quote_request = req;
    p.client.reqMarketDataType(type);
    p.client.reqMktData(req, contract, "", false, false, TagValueListSPtr{});
    try {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(p.config.timeout_seconds);
        // Allow all quote fields/type notifications to arrive, rather than using the first tick.
        while (std::chrono::steady_clock::now() < deadline)
            p.pump();
        p.client.cancelMktData(req);
        p.quote_request = -1;
        if (!p.watchlist_quote.bid && !p.watchlist_quote.ask && !p.watchlist_quote.last)
            throw std::runtime_error("no watchlist prices returned; check session, exchange and delayed-data availability");
        return p.watchlist_quote;
    } catch (...) {
        p.client.cancelMktData(req);
        p.quote_request = -1;
        throw;
    }
}
std::vector<Bar> IbkrBroker::historical_bars(std::uint64_t id, const std::string &duration,
                                             const std::string &bar_size, const std::string &end) {
    auto &p = *impl_;
    if (!ready())
        throw std::runtime_error("not synchronized");
    const auto &contract = p.contracts.at(id);
    p.history.clear();
    p.history_done = false;
    const int req = p.next_request++;
    p.history_request = req;
    p.requests[req] = id;
    p.client.reqHistoricalData(req, contract, end, duration, bar_size, "TRADES", 1, 2,
                               false, TagValueListSPtr{});
    try {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(p.config.timeout_seconds);
        while (!p.history_done) {
            p.pump();
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("historical data request timed out");
        }
        p.requests.erase(req);
        p.history_request = -1;
        if (p.history.empty())
            throw std::runtime_error("no historical data returned");
        return p.history;
    } catch (...) {
        p.client.cancelHistoricalData(req);
        p.requests.erase(req);
        p.history_request = -1;
        throw;
    }
}
void IbkrBroker::poll() {
    auto &p = *impl_;
    p.pump();
    if (!p.client.isConnected())
        throw std::runtime_error("IBKR disconnected");
    if (!p.subscriptions.empty() &&
        std::chrono::steady_clock::now() - p.last_data > std::chrono::seconds(p.config.stale_seconds))
        throw std::runtime_error("market data stale/unavailable; strategy halted");
}
void IbkrBroker::submit(const algo::Order &o) {
    auto &p = *impl_;
    if (p.config.read_only)
        throw std::runtime_error("IBKR order submission disabled: read-only mode");
    if (!ready())
        throw std::runtime_error("IBKR not ready");
    if (p.order_ids.contains(o.id))
        throw std::runtime_error("duplicate local order ID");
    const auto &i = p.instruments.at(o.ticker);
    if (!valid_quantity(o.quantity, i.lot_size))
        throw std::invalid_argument("invalid quantity");
    ::Order order;
    order.account = p.config.paper_account;
    order.action = o.side == Side::Buy ? "BUY" : "SELL";
    order.orderType = o.type == OrderType::Market ? "MKT" : "LMT";
    order.totalQuantity = DecimalFunctions::doubleToDecimal(o.quantity);
    order.tif = "DAY";
    order.orderRef = o.client_key;
    order.transmit = true;
    if (o.type == OrderType::Limit) {
        if (!o.limit_price || !std::isfinite(*o.limit_price) || *o.limit_price <= 0)
            throw std::invalid_argument("invalid limit");
        order.lmtPrice = *o.limit_price;
    }
    const int id = p.next_order++;
    p.orders.emplace(id, o);
    p.order_ids.emplace(o.id, id);
    p.client.placeOrder(id, p.contracts.at(o.ticker), order);
}
void IbkrBroker::cancel(OrderId id) {
    auto &p = *impl_;
    if (p.order_ids.contains(id))
        p.client.cancelOrder(p.order_ids.at(id), OrderCancel{});
}
#else
struct IbkrBroker::Impl {
    IbkrConfig config;
    BrokerCallbacks callbacks;
    explicit Impl(IbkrConfig c) : config(std::move(c)) {}
};
namespace {
[[noreturn]] void unavailable() {
    throw std::runtime_error(
        "IBKR adapter not built. Configure ALGO_WITH_IBKR=ON and IBKR_SDK_ROOT; see docs/IBKR.md.");
}
} // namespace
bool IbkrBroker::available() noexcept {
    return false;
}
IbkrBroker::IbkrBroker(IbkrConfig c) : impl_(std::make_unique<Impl>(c)) {
    validate_config(c);
}
IbkrBroker::~IbkrBroker() = default;
void IbkrBroker::set_callbacks(BrokerCallbacks c) {
    impl_->callbacks = std::move(c);
}
void IbkrBroker::add_instrument(const Instrument &) {
    unavailable();
}
void IbkrBroker::connect() {
    unavailable();
}
void IbkrBroker::disconnect() {}
bool IbkrBroker::ready() const {
    return false;
}
void IbkrBroker::request_snapshot() {
    unavailable();
}
void IbkrBroker::subscribe(std::uint64_t) {
    unavailable();
}
void IbkrBroker::unsubscribe(std::uint64_t) {
    unavailable();
}
std::vector<Bar> IbkrBroker::historical_bars(std::uint64_t, const std::string &, const std::string &,
                                             const std::string &) {
    unavailable();
}
IbkrQuote IbkrBroker::quote(std::uint64_t, int) {
    unavailable();
}
void IbkrBroker::poll() {
    unavailable();
}
void IbkrBroker::submit(const Order &) {
    unavailable();
}
void IbkrBroker::cancel(OrderId) {
    unavailable();
}
#endif
} // namespace algo
