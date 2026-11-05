#include "Backtester.hpp"
#include "IbkrBroker.hpp"
#include "MovingAverageStrategy.hpp"
#include "TradingApp.hpp"
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
void help() {
    std::cout << R"(AlgoTrader: C++ backtesting and IBKR paper trading

algotrader backtest --instruments FILE --data FILE [options]
algotrader ibkr-readonly --instruments FILE --port PORT --account DU... [options]
algotrader ibkr-history --instruments FILE --port PORT --account DU... --output results/history
  --duration "1 M" --bar-size "1 day" --end-time "20260131-23:59:59"
algotrader ibkr-paper --instruments FILE --port PORT --account DU... --enable-orders yes [options]

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
IBKR orders are disabled unless mode=ibkr-paper AND --enable-orders yes.
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
        if (mode != "backtest" && mode != "ibkr-readonly" && mode != "ibkr-paper" && mode != "ibkr-history")
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
                                               "--duration",      "--bar-size",    "--end-time"};
        for (int i = 2; i < argc; i += 2) {
            if (i + 1 >= argc || !allowed.contains(argv[i]) || !args.emplace(argv[i], argv[i + 1]).second)
                throw std::invalid_argument("invalid, duplicate or missing option value");
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
        const auto instruments = load_instruments(value("--instruments"));
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
            const bool trading = mode == "ibkr-paper";
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
            IbkrBroker broker(config);
            broker.add_instrument(instruments.front());
            std::signal(SIGINT, signal_handler);
            std::signal(SIGTERM, signal_handler);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(integer("--seconds", "60"));
            if (!trading) {
                BrokerCallbacks cb;
                cb.on_snapshot = [](const auto &s) {
                    std::cout << "Account: " << s.account << " Cash: " << s.cash << ' ' << s.currency
                              << " Positions: " << s.positions.size()
                              << " Open orders: " << s.open_orders.size() << '\n';
                };
                cb.on_bar = [](const auto &b) {
                    std::cout << b.date << " ticker=" << b.ticker << " close=" << b.close << '\n';
                };
                broker.set_callbacks(cb);
                broker.connect();
                broker.request_snapshot();
                if (mode == "ibkr-history") {
                    const auto bars =
                        broker.historical_bars(instruments.front().ticker, value("--duration", "1 M"),
                                               value("--bar-size", "1 day"), value("--end-time"));
                    const std::filesystem::path output = value("--output", "results/history");
                    std::filesystem::create_directories(output);
                    std::ofstream out(output / "bars.csv");
                    out << std::setprecision(17) << "date,ticker,open,close\n";
                    for (const auto &b : bars)
                        out << b.date << ',' << b.ticker << ',' << b.open << ',' << b.close << '\n';
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
