#include "Backtester.hpp"
#include "DailyLedger.hpp"
#include "CrtBacktest.hpp"
#include "CrtClock.hpp"
#include "DailySignal.hpp"
#include "IbkrBroker.hpp"
#include "TradingCalendar.hpp"
#include "MovingAverageStrategy.hpp"
#include "TradingApp.hpp"
#include <algorithm>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <thread>
using namespace algo;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) {
    interrupted = 1;
}
class Strategies final : public Strategy {
    std::vector<std::unique_ptr<Strategy>> children_;

  public:
    void add(std::unique_ptr<Strategy> s) {
        children_.push_back(std::move(s));
    }
    void reset() override {
        for (auto &s : children_)
            s->reset();
    }
    std::vector<Order> on_bar(const Bar &b, const Portfolio &p) override {
        std::vector<Order> out;
        for (auto &s : children_) {
            auto orders = s->on_bar(b, p);
            out.insert(out.end(), orders.begin(), orders.end());
        }
        return out;
    }
};
// Read-only daily signal report, with an optional guarded paper execution.
// Nothing is sent unless --execute AND --enable-orders yes are both supplied and the
// cycle is live, reconciled, unrecorded and approved by RiskManager.
struct DailyExecution {
    bool terminal{};
    bool filled{};
    double filled_quantity{};
    std::string status{"unknown"};
};

DailyExecution submit_daily_order(const IbkrConfig &config, const Instrument &instrument,
                                  const Order &order, int seconds, EventJournal &journal) {
    DailyExecution result;
    IbkrBroker broker(config);
    broker.add_instrument(instrument);
    auto snapshot = std::make_shared<BrokerSnapshot>();
    BrokerCallbacks callbacks;
    callbacks.on_snapshot = [snapshot](const BrokerSnapshot &s) { *snapshot = s; };
    callbacks.on_order = [&](const OrderUpdate &update) {
        result.filled_quantity = std::max(result.filled_quantity, update.filled_quantity);
        result.terminal = update.status == OrderStatus::Filled ||
                          update.status == OrderStatus::Cancelled ||
                          update.status == OrderStatus::Rejected;
        if (update.status == OrderStatus::Filled)
            result.status = "filled";
        else if (update.status == OrderStatus::Cancelled)
            result.status = "cancelled";
        else if (update.status == OrderStatus::Rejected)
            result.status = "rejected";
        else if (update.status == OrderStatus::PartiallyFilled)
            result.status = "partially-filled";
        journal.append({0, "", "order", update.reason});
    };
    callbacks.on_fill = [&](const Fill &fill) {
        result.filled = true;
        journal.append({0, "", "execution",
                        fill.execution_id + " " + std::to_string(fill.quantity) + "@" +
                            std::to_string(fill.price)});
    };
    callbacks.on_notice = [&](const std::string &message) {
        std::cout << "IBKR notice: " << message << '\n';
        journal.append({0, "", "broker_notice", message});
    };
    broker.set_callbacks(callbacks);
    broker.connect();
    try {
        broker.request_snapshot();
        if (!snapshot->complete)
            throw std::runtime_error("snapshot did not complete before submission");
        if (!snapshot->open_orders.empty())
            throw std::runtime_error("open orders appeared before submission");
        const auto held = snapshot->positions.find(instrument.ticker);
        const double shares = held == snapshot->positions.end() ? 0.0 : held->second;
        journal.append({0, "", "intent",
                        std::string(side_name(order.side)) + " " + std::to_string(order.quantity) +
                            " " + instrument.symbol + " at " +
                            std::to_string(order.limit_price.value_or(0)) + " from " +
                            std::to_string(shares) + " shares"});
        broker.submit(order);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (!result.terminal && !interrupted && std::chrono::steady_clock::now() < deadline)
            broker.poll();
        if (!result.terminal) {
            broker.cancel(order.id);
            const auto cancel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!result.terminal && std::chrono::steady_clock::now() < cancel_deadline)
                broker.poll();
            if (!result.terminal)
                result.status = "uncertain";
        }
        broker.disconnect();
    } catch (...) {
        broker.disconnect();
        throw;
    }
    return result;
}

int run_contract_lookup(const std::unordered_map<std::string, std::string> &args) {
    auto value = [&](const std::string &key, const std::string &fallback = "") {
        auto it = args.find(key);
        return it == args.end() ? fallback : it->second;
    };
    const auto symbol = value("--symbol");
    if (symbol.empty())
        throw std::invalid_argument("ibkr-contract needs --symbol");
    IbkrConfig config{value("--host", "127.0.0.1"),
                      std::stoi(value("--port", "4002")),
                      std::stoi(value("--client-id", "17")),
                      value("--account"),
                      true,
                      value("--currency", "USD"),
                      std::stoi(value("--timeout", "15")),
                      30};
    IbkrBroker ibkr(config);
    ibkr.connect();
    std::vector<ContractInfo> found;
    try {
        found = ibkr.resolve_contracts(symbol, value("--sec-type", "FUT"), value("--exchange", "SMART"),
                                       value("--expiry", ""), value("--currency", "USD"));
        ibkr.disconnect();
    } catch (...) {
        ibkr.disconnect();
        throw;
    }
    for (const auto &c : found)
        std::cout << c.con_id << "  " << c.symbol << " " << c.sec_type << " " << c.exchange << " "
                  << c.currency << "  expiry " << c.expiry << "  multiplier " << c.multiplier
                  << "  minTick " << c.min_tick << "  class " << c.trading_class << "  tz "
                  << c.time_zone << "  " << c.long_name << '\n';
    if (!found.empty())
        std::cout << "tradingHours: " << found.front().trading_hours << '\n';
    std::cout << found.size() << " contract(s)\n";
    return 0;
}

int run_crt_clock(const std::unordered_map<std::string, std::string> &args,
                  const std::vector<Instrument> &instruments, int argc, char **argv) {
    auto value = [&](const std::string &key, const std::string &fallback = "") {
        auto it = args.find(key);
        return it == args.end() ? fallback : it->second;
    };
    auto number = [&](const std::string &key, const std::string &fallback) {
        const auto text = value(key, fallback);
        std::size_t used = 0;
        const double parsed = std::stod(text, &used);
        if (used != text.size() || !std::isfinite(parsed))
            throw std::invalid_argument("invalid numeric option " + key);
        return parsed;
    };
    const auto data_path = value("--data");
    if (data_path.empty())
        throw std::invalid_argument("crt-clock needs --data FILE");
    if (instruments.size() != 1)
        throw std::invalid_argument("crt-clock runs one instrument");

    CrtClockConfig config;
    const auto buckets = value("--buckets", "570:240,810:240");
    std::stringstream bs(buckets);
    std::string item;
    while (std::getline(bs, item, ',')) {
        const auto colon = item.find(':');
        if (colon == std::string::npos)
            throw std::invalid_argument("--buckets expects start:minutes pairs");
        config.buckets.push_back({std::stoi(item.substr(0, colon)), std::stoi(item.substr(colon + 1))});
    }
    const auto &instrument = instruments.front();
    config.multiplier = instrument.multiplier;
    config.tick_size = instrument.tick_size;
    config.quantity = number("--quantity", "1");
    config.fee = number("--fee", "2.50");
    config.slippage_bps = static_cast<int>(number("--slippage-bps", "2"));
    config.max_hold_hours = number("--max-hold-hours", "8");
    config.cutoff_date = value("--cutoff");
    if (args.contains("--long-only"))
        config.allow_shorts = false;

    std::vector<Bar> bars;
    CsvDataFeed feed(data_path);
    feed.open();
    while (auto b = feed.next())
        if (b->ticker == instrument.ticker)
            bars.push_back(*b);

    const auto result = run_crt_clock(bars, config);
    if (!result.ok)
        throw std::runtime_error(result.error);

    std::string command;
    for (int n = 0; n < argc; ++n)
        command += (n ? " " : "") + std::string(argv[n]);
    const auto stamp_now = utc_timestamp(static_cast<std::int64_t>(std::time(nullptr)));
    const auto directory = value("--output", "results/crt-clock") + "/" +
                           utc_date(static_cast<std::int64_t>(std::time(nullptr))) + "_" + stamp_now;
    write_crt_clock_report(directory, result, config,
                           "CSV " + data_path + " (" + std::to_string(bars.size()) + " bars); command: " + command);

    double gross = 0, net = 0;
    for (const auto &t : result.trades) {
        gross += t.gross;
        net += t.net;
    }
    std::cout << "Clock CRT: " << instrument.symbol << '\n'
              << "  candles " << result.candles.size() << " (" << result.incomplete_candles
              << " incomplete), pairs " << result.pairs_examined << '\n'
              << "  setups " << result.setups << ", trades " << result.trades.size() << ", gross "
              << gross << ", net " << net << ", max dd " << 100.0 * result.max_drawdown << "%\n"
              << "  skipped: double-sided " << result.skipped_double_sided << ", gap "
              << result.skipped_gap_beyond_target << ", cutoff " << result.skipped_cutoff
              << ", no-next-bar " << result.skipped_no_next_bar << '\n'
              << "  report " << directory << "/report.md\n";
    return 0;
}

int run_crt_backtest(const std::unordered_map<std::string, std::string> &args,
                     const std::vector<Instrument> &instruments, int argc, char **argv) {
    auto value = [&](const std::string &key, const std::string &fallback = "") {
        auto it = args.find(key);
        return it == args.end() ? fallback : it->second;
    };
    auto number = [&](const std::string &key, const std::string &fallback) {
        const auto text = value(key, fallback);
        std::size_t used = 0;
        const double parsed = std::stod(text, &used);
        if (used != text.size() || !std::isfinite(parsed))
            throw std::invalid_argument("invalid numeric option " + key);
        return parsed;
    };
    const auto data_path = value("--data");
    if (data_path.empty())
        throw std::invalid_argument("crt-backtest needs --data FILE with intraday OHLC bars");
    if (instruments.size() != 1)
        throw std::invalid_argument("crt-backtest runs one instrument at a time");

    CrtConfig config;
    config.range_bars = static_cast<std::size_t>(number("--range-bars", "16"));
    config.max_hold_bars = static_cast<std::size_t>(number("--max-hold-bars", "32"));
    config.quantity = number("--quantity", "100");
    config.fee = number("--fee", "1.10");
    config.slippage_bps = static_cast<int>(number("--slippage-bps", "5"));
    if (args.contains("--long-only"))
        config.allow_shorts = false;

    std::vector<Bar> bars;
    CsvDataFeed feed(data_path);
    feed.open();
    while (auto bar = feed.next())
        if (bar->ticker == instruments.front().ticker)
            bars.push_back(*bar);
    if (bars.empty())
        throw std::runtime_error("no bars for the requested ticker in " + data_path);

    const auto result = run_crt_backtest(bars, config);
    if (!result.ok)
        throw std::runtime_error(result.error);

    std::string command;
    for (int n = 0; n < argc; ++n)
        command += (n ? " " : "") + std::string(argv[n]);
    const auto stamp = utc_timestamp(static_cast<std::int64_t>(std::time(nullptr)));
    const auto directory = value("--output", "results/crt") + "/" + utc_date(
                               static_cast<std::int64_t>(std::time(nullptr))) + "_" + stamp;
    const auto range_description =
        std::to_string(config.range_bars) + "x15m candles";
    write_crt_report(directory, result, config,
                     "CSV " + data_path + " (" + std::to_string(bars.size()) + " bars, " +
                         bars.front().date + " to " + bars.back().date + "); command: " + command,
                     range_description);

    int wins = 0;
    for (const auto &t : result.trades)
        if (t.pnl > 0)
            ++wins;
    std::cout << "CRT backtest: " << instruments.front().symbol << '\n'
              << "  bars " << bars.size() << " (" << bars.front().date << " to "
              << bars.back().date << ")\n"
              << "  setups " << result.setups << ", trades " << result.trades.size() << ", wins "
              << wins << '\n'
              << "  net P&L " << (result.end_equity - result.start_equity) << " on "
              << result.start_equity << " deployed (" << 100.0 * (result.end_equity - result.start_equity) / result.start_equity
              << "%)\n"
              << "  max drawdown " << 100.0 * result.max_drawdown << "%\n"
              << "  report " << directory << "/report.md\n";
    return 0;
}

int run_daily_signal(const std::unordered_map<std::string, std::string> &args,
                     const std::vector<Instrument> &instruments, int argc, char **argv) {
    auto value = [&](const std::string &key, const std::string &fallback = "") {
        auto it = args.find(key);
        return it == args.end() ? fallback : it->second;
    };
    auto number = [&](const std::string &key, const std::string &fallback) {
        const auto text = value(key, fallback);
        std::size_t used = 0;
        const double parsed = std::stod(text, &used);
        if (used != text.size() || !std::isfinite(parsed))
            throw std::invalid_argument("invalid numeric option " + key);
        return parsed;
    };
    auto whole = [&](const std::string &key, const std::string &fallback) {
        const double parsed = number(key, fallback);
        if (parsed < 0 || parsed > 10000000 || std::floor(parsed) != parsed)
            throw std::invalid_argument("invalid integer option " + key);
        return static_cast<int>(parsed);
    };

    if (instruments.size() != 1)
        throw std::invalid_argument("daily-signal supports exactly one instrument");
    const auto &instrument = instruments.front();
    if (instrument.asset_class != AssetClass::Stock)
        throw std::invalid_argument("daily-signal currently supports one stock/ETF");

    const bool execute = args.contains("--execute");
    const bool orders_enabled = value("--enable-orders") == "yes";
    if (args.contains("--enable-orders") && !execute)
        throw std::invalid_argument(
            "daily-signal is read-only unless --execute is also given; refusing an order flag on "
            "its own");
    if (execute && !orders_enabled)
        throw std::invalid_argument("--execute requires --enable-orders yes");
    const bool pump_until_close = args.contains("--wait-until-close");

    SignalRequest request;
    request.window = static_cast<std::size_t>(whole("--window", "100"));
    request.quantity = number("--quantity", "1");
    request.as_of = value("--as-of");
    request.max_stale_days = whole("--max-stale-days", "5");
    request.now_unix = static_cast<std::int64_t>(std::time(nullptr));
    if (!request.as_of.empty() && (request.as_of.size() != 10 || !valid_timestamp(request.as_of)))
        throw std::invalid_argument("--as-of requires YYYY-MM-DD");

    std::string command;
    for (int n = 0; n < argc; ++n)
        command += (n ? " " : "") + std::string(argv[n]);

    const auto bars_path = value("--bars");
    const auto account = value("--account");
    const auto port = whole("--port", "0");
    const bool live = !account.empty() || port != 0;
    if (bars_path.empty() && !live)
        throw std::invalid_argument(
            "daily-signal needs --bars FILE for offline use or --port/--account for a live "
            "read-only IBKR session");
    if (live && (account.size() < 3 || !account.starts_with("DU") || port < 1))
        throw std::invalid_argument("live mode requires --account DU... and a valid --port");
    if (execute && (!bars_path.empty() || !live))
        throw std::invalid_argument("--execute needs a live paper session, not --bars");
    const int band_bps = whole("--limit-band-bps", "25");
    if (band_bps > 500)
        throw std::invalid_argument("--limit-band-bps must be 0..500");
    const auto output_root = value("--output", "results/daily-signal");
    const auto ledger_path = value("--ledger", output_root + "/ledger.csv");

    if (pump_until_close) {
        if (!live)
            throw std::invalid_argument("--wait-until-close needs a live session");
        if (!is_us_equity_trading_day(utc_date(request.now_unix))) {
            std::cout << "Daily signal: " << utc_date(request.now_unix)
                      << " is not a US equity trading day; nothing to do.\n";
            return 0;
        }
        while (!interrupted) {
            const auto wait =
                seconds_until_session_cutoff(static_cast<std::int64_t>(std::time(nullptr)));
            if (wait <= 0)
                break;
            std::cout << "Waiting " << wait << "s for the session cutoff (22:00 UTC)\n";
            std::this_thread::sleep_for(std::chrono::seconds(std::min<std::int64_t>(wait, 30)));
        }
        request.now_unix = static_cast<std::int64_t>(std::time(nullptr));
    }

    std::vector<Bar> bars;
    BrokerView broker;
    std::string source;

    if (!bars_path.empty()) {
        CsvDataFeed feed(bars_path);
        feed.open();
        while (auto bar = feed.next())
            if (bar->ticker == instrument.ticker)
                bars.push_back(*bar);
        source = "CSV " + bars_path + " (offline, no broker session)";
        broker.confirmed = false;
        broker.error = "offline mode: no broker session was opened (--bars supplied)";
    }

    if (live) {
        IbkrConfig config{value("--host", "127.0.0.1"),
                          port,
                          whole("--client-id", "17"),
                          account,
                          true, // the reporting session is always read-only
                          value("--currency", "USD"),
                          whole("--timeout", "15"),
                          whole("--stale-seconds", "30")};
        IbkrBroker ibkr(config);
        ibkr.add_instrument(instrument);
        auto snapshot = std::make_shared<BrokerSnapshot>();
        BrokerCallbacks callbacks;
        callbacks.on_snapshot = [snapshot](const BrokerSnapshot &s) { *snapshot = s; };
        callbacks.on_notice = [](const std::string &message) {
            std::cout << "IBKR notice: " << message << '\n';
        };
        ibkr.set_callbacks(callbacks);
        ibkr.connect();
        try {
            std::string snapshot_error;
            try {
                ibkr.request_snapshot();
            } catch (const std::exception &e) {
                snapshot_error = e.what();
            }
            if (snapshot_error.empty() && snapshot->complete) {
                broker.confirmed = true;
                broker.account = snapshot->account;
                broker.currency = snapshot->currency;
                broker.cash = snapshot->cash;
                const auto it = snapshot->positions.find(instrument.ticker);
                broker.shares = it == snapshot->positions.end() ? 0.0 : it->second;
                for (const auto &state : snapshot->open_orders)
                    if (state.order.ticker == instrument.ticker)
                        broker.open_orders.push_back(state);
            } else {
                broker.confirmed = false;
                broker.error = snapshot_error.empty() ? "snapshot did not complete" : snapshot_error;
            }
            if (bars_path.empty()) {
                if (!broker.confirmed)
                    throw std::runtime_error(
                        "cannot read historical bars after an incomplete snapshot: " +
                        broker.error +
                        "; rerun with --bars FILE to report the signal while the broker state is "
                        "unknown");
                bars = ibkr.historical_bars(instrument.ticker, value("--duration", "2 Y"), "1 day",
                                            value("--end-time", ""));
                source = "IBKR Gateway " + value("--host", "127.0.0.1") + ":" +
                         std::to_string(port) + ", contract " + std::to_string(instrument.con_id) +
                         " " + instrument.symbol + ", TRADES, regular hours, duration " +
                         value("--duration", "2 Y") + ", bars 1 day, retrieved " +
                         utc_timestamp(request.now_unix);
            }
            ibkr.disconnect();
        } catch (...) {
            ibkr.disconnect();
            throw;
        }
    }

    const auto signal = evaluate_signal(bars, request);
    const auto proposal = propose_action(signal, broker);

    std::vector<Bar> bars_used;
    if (signal.ok) {
        std::size_t decision_index = bars.size() - 1;
        for (std::size_t i = 0; i < bars.size(); ++i)
            if (bars[i].date == signal.decision_date)
                decision_index = i;
        const auto first = static_cast<std::ptrdiff_t>(decision_index - signal.window);
        bars_used.assign(bars.begin() + first,
                         bars.begin() + static_cast<std::ptrdiff_t>(decision_index) + 1);
    } else if (!bars.empty()) {
        bars_used.assign(bars.end() - 1, bars.end());
    }

    ReportContext context{instrument.symbol, instrument.currency, source, command, request.now_unix};
    const auto files = write_signal_report(output_root, signal, broker, proposal, context, bars_used);

    std::cout << "Daily signal: " << instrument.symbol << '\n';
    if (signal.ok) {
        std::cout << "  decision session " << signal.decision_date << " close "
                  << signal.decision_close << " " << instrument.currency << '\n'
                  << "  prior " << signal.window << "-session average " << signal.prior_average
                  << " (" << signal.window_first << " to " << signal.window_last << ")\n"
                  << "  signal " << signal_state_name(signal.state) << '\n';
    } else {
        std::cout << "  signal unavailable: " << signal.error << '\n';
    }
    if (broker.confirmed)
        std::cout << "  broker " << broker.account << " position " << broker.shares
                  << " share(s), " << broker.open_orders.size() << " open order(s), cash "
                  << broker.cash << " " << broker.currency << '\n';
    else
        std::cout << "  broker state unknown: " << broker.error << '\n';
    std::cout << "  status " << proposal_status_name(proposal.status) << ": "
              << (signal.ok ? proposal.reason : signal.error) << '\n';
    if (proposal.status == ProposalStatus::Actionable && proposal.side)
        std::cout << "  proposed " << side_name(*proposal.side) << " " << proposal.quantity
                  << " share(s) — NOT SUBMITTED\n";
    std::cout << "  report " << files.markdown << '\n';

    // A failed evaluation has no decision session; record the newest bar instead so the
    // run is still auditable, and skip the ledger entirely if there is no bar at all.
    const std::string ledger_session =
        signal.ok ? signal.decision_date : (bars.empty() ? std::string() : bars.back().date);
    LedgerEntry entry{utc_timestamp(request.now_unix),
                      ledger_session,
                      instrument.symbol,
                      signal.ok ? signal_state_name(signal.state) : "unavailable",
                      signal.ok && signal.target_shares
                          ? std::to_string(static_cast<int>(*signal.target_shares))
                          : std::string("retain"),
                      broker.confirmed ? std::to_string(static_cast<int>(broker.shares)) : "unknown",
                      broker.confirmed ? std::to_string(broker.open_orders.size()) : "unknown",
                      proposal_status_name(proposal.status),
                      "",
                      "",
                      "",
                      files.directory};

    if (!execute) {
        if (ledger_session.empty()) {
            std::cout << "  ledger skipped: no session or bar to record\n";
        } else {
            append_ledger(ledger_path, entry);
            std::cout << "  ledger " << ledger_path << '\n';
        }
        return signal.ok ? 0 : 1;
    }

    // ---- guarded paper execution -------------------------------------------------
    const auto plan = plan_execution(signal, proposal, instrument.tick_size, band_bps);
    if (proposal.status != ProposalStatus::Actionable || !proposal.side)
        throw std::runtime_error("nothing to execute: " + proposal.reason);
    if (!plan.ok)
        throw std::runtime_error("no execution plan: " + plan.error);
    if (ledger_has_session(ledger_path, instrument.symbol, signal.decision_date))
        throw std::runtime_error("session " + signal.decision_date + " is already recorded in " +
                                 ledger_path + "; refusing to trade it twice");

    Portfolio portfolio(broker.cash, instrument.currency);
    portfolio.add_instrument(instrument);
    if (std::abs(broker.shares) > 1e-9)
        portfolio.restore(broker.cash, {{instrument.ticker, broker.shares, 0.0}});
    RiskManager risk({number("--max-order", "1000"), number("--max-position", "1000"),
                      number("--cash-buffer", "100"), number("--fee", "1.1"),
                      number("--slippage-bps", "5")});
    risk.resume(); // the snapshot taken by this run is the reconciliation
    Order order;
    order.id = 1;
    order.client_key = "daily/" + signal.decision_date + "/" + instrument.symbol + "/" +
                       (plan.side == Side::Buy ? "buy" : "sell");
    order.ticker = instrument.ticker;
    order.side = plan.side;
    order.type = OrderType::Limit;
    order.quantity = plan.quantity;
    order.limit_price = plan.limit_price;
    order.created_at = utc_timestamp(request.now_unix);
    const auto decision =
        risk.evaluate(order, portfolio, broker.open_orders, plan.reference_close, true);
    if (!decision.allowed)
        throw std::runtime_error("risk manager refused the order: " + decision.reason);

    entry.status = "EXEC:INTENT";
    entry.side = side_name(plan.side);
    entry.quantity = std::to_string(static_cast<int>(plan.quantity));
    entry.limit_price = std::to_string(plan.limit_price);
    append_ledger(ledger_path, entry);

    EventJournal journal(files.directory + "/execution.journal");
    journal.open();
    const auto execution = submit_daily_order(
        IbkrConfig{value("--host", "127.0.0.1"), port, whole("--client-id", "17"), account,
                   false, // submission needs the paper account's Read-Only API setting off
                   value("--currency", "USD"), whole("--timeout", "15"),
                   whole("--stale-seconds", "30")},
        instrument, order, whole("--seconds", "120"), journal);

    entry.status = "EXEC:" + (execution.status == "unknown" ? std::string("uncertain")
                                                          : execution.status);
    entry.quantity = std::to_string(static_cast<int>(execution.filled_quantity));
    append_ledger(ledger_path, entry);

    std::cout << "  execution " << execution.status << ": filled " << execution.filled_quantity
              << " of " << plan.quantity << " at limit " << plan.limit_price << " (journal "
              << files.directory << "/execution.journal)\n";
    if (execution.status == "uncertain")
        std::cout << "  WARNING: outcome unconfirmed — reconcile the paper account before the next "
                     "run\n";
    return 0;
}

void help() {
    std::cout << R"(AlgoTrader: C++ backtesting and IBKR paper trading

algotrader backtest --instruments FILE --data FILE [options]
algotrader ibkr-quotes --instruments FILE --port PORT --account DU... --market-data-type 3
  Read-only watchlist quotes; type 3 requests delayed data, never strategy bars.
algotrader ibkr-status --instruments FILE --port PORT --account DU...   account snapshot only
algotrader ibkr-readonly --instruments FILE --port PORT --account DU... [options]
algotrader ibkr-history --instruments FILE --port PORT --account DU... --output results/history
  --duration "1 M" --bar-size "1 day" --end-time "20260131-23:59:59"
algotrader ibkr-buy --instruments FILE --port PORT --account DU... --limit-price PRICE
  Preview by default; add --enable-orders yes to send ONE stock share (maximum limit $1000).
algotrader ibkr-paper --instruments FILE --port PORT --account DU... --enable-orders yes [options]
algotrader crt-backtest --instruments FILE --data INTRADAY_OHLC.csv [options]
  Candle Range Theory research: 4H range (16x15m bars) -> 15m entry, stop and target.
algotrader daily-signal --instruments FILE [--bars FILE | --port PORT --account DU...] [options]
  Daily SMA report: prior-window average, strategy target, broker position and open
  orders. Read-only unless --execute --enable-orders yes are BOTH given.
  --bars uses a CSV export and no broker; --wait-until-close blocks to 22:00 UTC.

Options (all values explicit):
  --strategy sma|breakout|mean-reversion|buy-hold   default sma
  --window 20 --quantity 1 --threshold 0.02        strategy parameters
  --from YYYY-MM-DD --to YYYY-MM-DD              inclusive date range; warmup starts at from
  --max-session-loss 0.05                         IBKR stop threshold versus startup cash
  --cash 100000 --currency USD                    backtest account
  --fee 1 --slippage-bps 5 --max-fill 0            per-fill fee, adverse slippage, partial-fill cap
  --max-order 100000 --max-position 200000 --cash-buffer 0
  --output results/run                           equity/trades/decisions/summary/config exports
  --host 127.0.0.1 --client-id 17 --seconds 60     IBKR session settings
  --journal results/paper.journal                 new path for each reconciled paper session
  --timeout 15 --stale-seconds 30                 connection and data freshness bounds

IBKR mode uses one instrument and completed 5-second TRADES bars, regular hours.
IBKR orders require ibkr-paper or ibkr-buy AND --enable-orders yes.
Backtest prices must be actual quotes for the selected instrument, including option premiums.
Long-only stocks/ETFs, crypto spot, futures and options; one account currency.
Close derivatives before expiry. Exercise, assignment and futures rolls are not simulated.
)";
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc < 2 || std::string(argv[1]) == "--help") {
            help();
            return 0;
        }
        const std::string mode = argv[1];
        if (mode != "backtest" && mode != "daily-signal" && mode != "crt-backtest" && mode != "crt-clock" && mode != "ibkr-contract" && mode != "ibkr-readonly" && mode != "ibkr-paper" && mode != "ibkr-history" && mode != "ibkr-buy" && mode != "ibkr-status" && mode != "ibkr-quotes")
            throw std::invalid_argument("unknown mode; use --help");
        std::unordered_map<std::string, std::string> args;
        const std::set<std::string> allowed = {"--instruments",   "--data",        "--strategy",
                                               "--window",        "--quantity",    "--threshold",
                                               "--cash",          "--currency",    "--fee",
                                               "--slippage-bps",  "--max-fill",    "--max-order",
                                               "--max-position",  "--cash-buffer", "--output",
                                               "--host",          "--port",        "--client-id",
                                               "--account",       "--seconds",     "--journal",
                                               "--enable-orders", "--timeout",     "--stale-seconds",
                                               "--from",          "--to",          "--max-session-loss",
                                               "--duration",      "--bar-size",    "--end-time", "--limit-price", "--market-data-type",
                                               "--bars",          "--as-of",       "--max-stale-days",
                                               "--ledger",        "--limit-band-bps", "--execute",
                                               "--range-bars", "--max-hold-bars", "--symbol",
                                               "--sec-type", "--expiry", "--exchange",
                                               "--buckets", "--max-hold-hours", "--cutoff",
                                               "--wait-until-close", "--extended-hours", "--range-bars", "--max-hold-bars",
                                               "--long-only", "--symbol", "--sec-type", "--expiry",
                                               "--exchange"};
        // Most options take a value; a few are bare switches.
        const std::set<std::string> switches = {"--execute",        "--wait-until-close",
                                                "--extended-hours", "--long-only"};
        for (int i = 2; i < argc;) {
            const std::string key = argv[i];
            if (!allowed.contains(key))
                throw std::invalid_argument("invalid option " + key);
            if (switches.contains(key)) {
                if (!args.emplace(key, "true").second)
                    throw std::invalid_argument("duplicate option " + key);
                i += 1;
                continue;
            }
            if (i + 1 >= argc || !args.emplace(key, argv[i + 1]).second)
                throw std::invalid_argument("invalid, duplicate or missing option value");
            i += 2;
        }
        auto value = [&](const std::string &key, const std::string &fallback = "") {
            return args.contains(key) ? args.at(key) : fallback;
        };
        auto num = [&](const std::string &key, const std::string &fallback) {
            auto s = value(key, fallback);
            std::size_t n;
            double x = std::stod(s, &n);
            if (n != s.size() || !std::isfinite(x))
                throw std::invalid_argument("invalid numeric option " + key);
            return x;
        };
        auto integer = [&](const std::string &key, const std::string &fallback) {
            double x = num(key, fallback);
            if (x < 0 || x > 10000000 || std::floor(x) != x)
                throw std::invalid_argument("invalid integer option " + key);
            return static_cast<int>(x);
        };
        // Contract lookup needs no instrument catalogue.
        if (mode == "ibkr-contract")
            return run_contract_lookup(args);
        const auto instruments = load_instruments(value("--instruments"));
        if (mode == "daily-signal")
            return run_daily_signal(args, instruments, argc, argv);
        if (mode == "crt-backtest")
            return run_crt_backtest(args, instruments, argc, argv);
        if (mode == "crt-clock")
            return run_crt_clock(args, instruments, argc, argv);
        Portfolio portfolio(num("--cash", "100000"), value("--currency", "USD"));
        for (const auto &i : instruments)
            portfolio.add_instrument(i);
        StrategyKind kind;
        const auto name = value("--strategy", "sma");
        if (name == "sma")
            kind = StrategyKind::MovingAverage;
        else if (name == "breakout")
            kind = StrategyKind::Breakout;
        else if (name == "mean-reversion")
            kind = StrategyKind::MeanReversion;
        else if (name == "buy-hold")
            kind = StrategyKind::BuyAndHold;
        else
            throw std::invalid_argument("unknown strategy");
        Strategies strategy;
        for (const auto &i : instruments) {
            const double quantity = num("--quantity", "1");
            if (!valid_quantity(quantity, i.lot_size))
                throw std::invalid_argument("quantity violates instrument lot size");
            strategy.add(std::make_unique<MovingAverageStrategy>(i.ticker, integer("--window", "20"),
                                                                 quantity, kind, num("--threshold", "0.02")));
        }
        const double fee = num("--fee", "1"), slip = num("--slippage-bps", "5");
        RiskManager risk({num("--max-order", "100000"), num("--max-position", "200000"),
                          num("--cash-buffer", "0"), fee, slip});
        FillProcessor fills(portfolio);
        PerformanceReport report;
        if (mode == "backtest") {
            BrokerSim broker(fee, slip, num("--max-fill", "0"));
            OrderManager orders(broker);
            CsvDataFeed feed(value("--data"));
            feed.set_range(value("--from"), value("--to"));
            Backtester engine(feed, broker, strategy, portfolio, orders, fills, risk, report);
            engine.run();
            const std::filesystem::path output = value("--output", "results/run");
            std::filesystem::create_directories(output);
            report.write_csv((output / "equity.csv").string());
            report.write_trades((output / "trades.csv").string(), fills);
            std::ofstream decisions(output / "decisions.csv");
            decisions << "timestamp,ticker,decision,reason\n";
            for (const auto &d : engine.decisions())
                decisions << d << '\n';
            std::ofstream summary(output / "summary.csv");
            summary << "initial_equity,final_equity,total_return,max_drawdown,executions,fees\n"
                    << std::setprecision(17) << report.points().front().equity << ','
                    << report.points().back().equity << ',' << report.total_return() << ','
                    << report.max_drawdown() << ',' << fills.executions().size() << ',' << fills.fees()
                    << '\n';
            std::ofstream config(output / "run.txt");
            for (int n = 0; n < argc; ++n)
                config << std::quoted(argv[n]) << ' ';
            config << '\n';
            if (!std::filesystem::exists(output / "instruments.csv") ||
                !std::filesystem::equivalent(value("--instruments"), output / "instruments.csv"))
                std::filesystem::copy_file(value("--instruments"), output / "instruments.csv",
                                           std::filesystem::copy_options::overwrite_existing);
            decisions.flush();
            summary.flush();
            config.flush();
            if (!decisions || !summary || !config)
                throw std::runtime_error("report write failed");
            std::cout << std::setprecision(10) << "Final equity: " << report.points().back().equity
                      << "\nReturn: " << 100 * report.total_return()
                      << "%\nMax drawdown: " << 100 * report.max_drawdown()
                      << "%\nExecutions: " << fills.executions().size() << "\nFees: " << fills.fees()
                      << "\nReports: " << output.string() << '\n';
        } else {
            if (instruments.size() != 1)
                throw std::invalid_argument("IBKR first release supports one instrument per process/account");
            const bool manual = mode == "ibkr-buy";
            const bool trading = mode == "ibkr-paper" || (manual && value("--enable-orders") == "yes");
            if (trading && value("--enable-orders") != "yes")
                throw std::invalid_argument("paper submission requires --enable-orders yes");
            IbkrConfig config{value("--host", "127.0.0.1"),
                              integer("--port", "0"),
                              integer("--client-id", "17"),
                              value("--account"),
                              !trading,
                              value("--currency", "USD"),
                              integer("--timeout", "15"),
                              integer("--stale-seconds", "30")};
            // A historical pull is a data request: it must not fail because the account
            // holds some other instrument that this run's catalogue does not describe.
            config.data_only = mode == "ibkr-history" || mode == "ibkr-quotes";
            IbkrBroker broker(config);
            broker.add_instrument(instruments.front());
            std::signal(SIGINT, signal_handler);
            std::signal(SIGTERM, signal_handler);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(integer("--seconds", "60"));
            if (manual) {
                const auto &instrument = instruments.front();
                const double limit = num("--limit-price", "0");
                if (instrument.asset_class != AssetClass::Stock || instrument.currency != "USD" ||
                    instrument.multiplier != 1 || num("--quantity", "1") != 1 ||
                    limit <= 0 || limit > 1000 ||
                    std::abs(limit / instrument.tick_size - std::round(limit / instrument.tick_size)) > 1e-6)
                    throw std::invalid_argument("ibkr-buy requires one USD stock share and a tick-aligned limit in (0, 1000]");
                const auto seconds = integer("--seconds", "60");
                if (seconds < 1 || seconds > 300)
                    throw std::invalid_argument("ibkr-buy --seconds must be 1..300");
                BrokerSnapshot snapshot;
                bool terminal = false;
                bool rejected = false;
                double reported_filled = 0;
                std::unique_ptr<EventJournal> journal;
                auto record = [&](const std::string &type, const std::string &message) {
                    std::cout << type << ": " << message << std::endl;
                    if (journal)
                        journal->append({0, "", type, message});
                };
                BrokerCallbacks cb;
                cb.on_snapshot = [&](const auto &s) { snapshot = s; };
                cb.on_order = [&](const auto &u) {
                    reported_filled = std::max(reported_filled, u.filled_quantity);
                    terminal = u.status == OrderStatus::Filled || u.status == OrderStatus::Cancelled ||
                               u.status == OrderStatus::Rejected;
                    rejected = u.status == OrderStatus::Rejected;
                    const char *state = u.status == OrderStatus::Filled ? "Filled" :
                        u.status == OrderStatus::Cancelled ? "Cancelled" :
                        rejected ? "Rejected" : u.status == OrderStatus::Submitted ? "Submitted" :
                        u.status == OrderStatus::PendingCancel ? "PendingCancel" :
                        u.status == OrderStatus::PendingSubmit ? "PendingSubmit" :
                        u.status == OrderStatus::PartiallyFilled ? "PartiallyFilled" : "Unknown";
                    record("order", std::string(state) + " filled=" + std::to_string(u.filled_quantity) +
                           " remaining=" + std::to_string(u.remaining_quantity) + " " + u.reason);
                };
                cb.on_notice = [&](const auto &message) { record("broker_notice", message); };
                cb.on_fill = [&](const auto &f) {
                    record("execution", f.execution_id + " quantity=" + std::to_string(f.quantity) +
                           " price=" + std::to_string(f.price));
                };
                cb.on_commission = [&](const auto &c) {
                    record("commission", c.execution_id + " " + std::to_string(c.amount) + " " + c.currency);
                };
                broker.set_callbacks(cb);
                broker.connect();
                broker.request_snapshot();
                if (!snapshot.complete || !snapshot.positions.empty() || !snapshot.open_orders.empty())
                    throw std::runtime_error("first-order test requires a complete snapshot, no positions and no open orders");
                if (snapshot.cash < limit + 100)
                    throw std::runtime_error("insufficient USD cash including $100 buffer");
                std::cout << "Paper account " << snapshot.account << ": BUY 1 " << instrument.symbol
                          << " LIMIT " << limit << " USD, DAY, regular hours.\n";
                if (!trading) {
                    std::cout << "Preview only. Add --enable-orders yes to submit. No order sent.\n";
                    broker.disconnect();
                    return 0;
                }
                const std::filesystem::path path = value("--journal", "results/manual-buy.journal");
                if (path.has_parent_path())
                    std::filesystem::create_directories(path.parent_path());
                journal = std::make_unique<EventJournal>(path.string());
                journal->open();
                if (!journal->replay().empty())
                    throw std::runtime_error("journal already used; reconcile previous order before using a new path");
                Order order;
                order.id = 1;
                order.client_key = "manual-buy-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
                order.ticker = instrument.ticker;
                order.quantity = 1;
                order.type = OrderType::Limit;
                order.limit_price = limit;
                record("intent", snapshot.account + " " + instrument.symbol + " BUY 1 LIMIT " +
                       std::to_string(limit) + " " + order.client_key);
                bool attempted = false;
                try {
                    if (interrupted)
                        throw std::runtime_error("interrupted before submission");
                    attempted = true;
                    broker.submit(order);
                    const auto stop = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
                    while (!terminal && !interrupted && std::chrono::steady_clock::now() < stop)
                        broker.poll();
                    if (!terminal) {
                        broker.cancel(order.id);
                        record("cancel_requested", "Waiting up to 10 seconds for acknowledgement");
                        const auto cancel_stop = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                        while (!terminal && std::chrono::steady_clock::now() < cancel_stop)
                            broker.poll();
                    }
                    if (!terminal)
                        throw std::runtime_error("cancellation unconfirmed; inspect paper account before retrying");
                } catch (...) {
                    if (attempted && !terminal) {
                        try { broker.cancel(order.id); } catch (...) {}
                        std::cerr << "Order outcome uncertain. Inspect paper orders before retrying.\n";
                    }
                    broker.disconnect();
                    throw;
                }
                broker.disconnect();
                std::cout << "Test ended. Broker-reported filled quantity: " << reported_filled << ".\n";
                if (reported_filled > 0)
                    std::cout << "Filled shares remain in the paper account.\n";
                else
                    std::cout << "No shares were reported filled.\n";
                std::cout << "Journal: " << path << '\n';
                // A completed cancellation is not a successful purchase.
                return rejected ? 1 : (reported_filled > 0 ? 0 : 2);
            } else if (!trading) {
                BrokerCallbacks cb;
                cb.on_snapshot = [](const auto &s) {
                    std::cout << "Account: " << s.account << " Cash: " << s.cash << ' ' << s.currency
                              << " Positions: " << s.positions.size()
                              << " Open orders: " << s.open_orders.size() << '\n';
                    for (const auto &[ticker, quantity] : s.positions)
                        std::cout << "Position ticker=" << ticker << " quantity=" << quantity << '\n';
                };
                cb.on_bar = [](const auto &b) {
                    std::cout << b.date << " ticker=" << b.ticker << " close=" << b.close << '\n';
                };
                cb.on_notice = [](const auto &message) { std::cout << message << '\n'; };
                broker.set_callbacks(cb);
                broker.connect();
                broker.request_snapshot();
                if (mode == "ibkr-quotes") {
                    const auto q = broker.quote(instruments.front().ticker, integer("--market-data-type", "3"));
                    const char *labels[] = {"unknown", "live", "frozen", "delayed", "delayed-frozen"};
                    const int type = q.market_data_type;
                    std::cout << "Watchlist quote: " << instruments.front().symbol << " type="
                              << (type >= 0 && type <= 4 ? labels[type] : "unknown") << '\n';
                    if (q.bid) std::cout << "bid=" << *q.bid << '\n';
                    if (q.ask) std::cout << "ask=" << *q.ask << '\n';
                    if (q.last) std::cout << "last=" << *q.last << '\n';
                    std::cout << "Diagnostic only: prices may be delayed/frozen; no orders or strategy signals.\n";
                    broker.disconnect();
                    return 0;
                }
                if (mode == "ibkr-status") {
                    broker.disconnect();
                    return 0;
                }
                if (mode == "ibkr-history") {
                    const auto bars =
                        broker.historical_bars(instruments.front().ticker, value("--duration", "1 M"),
                                               value("--bar-size", "1 day"), value("--end-time"),
                                               !args.contains("--extended-hours"));
                    const std::filesystem::path output = value("--output", "results/history");
                    std::filesystem::create_directories(output);
                    std::ofstream out(output / "bars.csv");
                    out << std::setprecision(17) << "date,ticker,open,high,low,close,volume\n";
                    for (const auto &b : bars)
                        out << b.date << ',' << b.ticker << ',' << b.open << ',' << b.high << ','
                            << b.low << ',' << b.close << ',' << b.volume << '\n';
                    out.flush();
                    if (!out)
                        throw std::runtime_error("historical CSV write failed");
                    std::cout << "Saved " << bars.size() << " bars to " << (output / "bars.csv").string()
                              << '\n';
                    broker.disconnect();
                    return 0;
                }
                broker.subscribe(instruments.front().ticker);
                while (!interrupted && std::chrono::steady_clock::now() < deadline)
                    broker.poll();
                broker.disconnect();
            } else {
                const std::filesystem::path journal_path = value("--journal", "results/paper.journal");
                if (journal_path.has_parent_path())
                    std::filesystem::create_directories(journal_path.parent_path());
                EventJournal journal(journal_path.string());
                OrderManager orders(broker);
                TradingApp app(broker, strategy, portfolio, orders, fills, risk, journal,
                               num("--max-session-loss", "0.05"));
                app.start();
                try {
                    broker.subscribe(instruments.front().ticker);
                    while (!interrupted && std::chrono::steady_clock::now() < deadline)
                        broker.poll();
                    app.stop();
                } catch (...) {
                    try {
                        app.stop();
                    } catch (const std::exception &cleanup) {
                        std::cerr << "Shutdown incomplete: " << cleanup.what() << '\n';
                    }
                    throw;
                }
                std::cout << "Paper session ended. Executions: " << fills.executions().size()
                          << " Journal: " << journal_path.string() << '\n';
            }
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
