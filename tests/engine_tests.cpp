#include "Backtester.hpp"
#include "EventJournal.hpp"
#include "IbkrBroker.hpp"
#include "MovingAverageStrategy.hpp"
#include "TradingApp.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unistd.h>
using namespace algo;
namespace {
int checks = 0;
void check(bool ok, const std::string &message) {
    ++checks;
    if (!ok)
        throw std::runtime_error(message);
}
void near(double actual, double expected) {
    check(std::abs(actual - expected) < 1e-7,
          "expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}
template <class F> void throws(F f) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception &) {
        caught = true;
    }
    check(caught, "expected an exception");
}
struct Temp {
    std::filesystem::path dir;
    Temp() {
        char pattern[] = "/tmp/algotrader-test-XXXXXX";
        auto p = mkdtemp(pattern);
        if (!p)
            throw std::runtime_error("mkdtemp");
        dir = p;
    }
    ~Temp() {
        std::filesystem::remove_all(dir);
    }
    std::string file(const std::string &name, const std::string &contents) {
        auto p = dir / name;
        std::ofstream(p) << contents;
        return p.string();
    }
};
Instrument stock(std::uint64_t id = 1) {
    Instrument i;
    i.ticker = id;
    i.symbol = "TEST";
    i.lot_size = 0.0001;
    return i;
}
Instrument future() {
    auto i = stock();
    i.asset_class = AssetClass::Future;
    i.lot_size = 1;
    i.multiplier = 5;
    i.initial_margin = 1000;
    i.expiry = "2026-12-18";
    return i;
}
Instrument option() {
    auto i = stock();
    i.asset_class = AssetClass::Option;
    i.lot_size = 1;
    i.multiplier = 100;
    i.expiry = "2026-12-18";
    i.strike = 100;
    i.right = "C";
    return i;
}
Order intent(Side side = Side::Buy, double quantity = 1) {
    Order o;
    o.ticker = 1;
    o.side = side;
    o.quantity = quantity;
    o.created_at = "2026-01-01";
    return o;
}
struct Simulation {
    Portfolio p;
    BrokerSim broker;
    OrderManager orders;
    FillProcessor fills;
    Simulation(double cash = 1000, double fee = 1, double slip = 0, double cap = 0)
        : p(cash), broker(fee, slip, cap), orders(broker), fills(p) {
        p.add_instrument(stock());
        broker.configure_account(p);
        BrokerCallbacks cb;
        cb.on_order = [this](const auto &u) { orders.on_update(u); };
        cb.on_fill = [this](const auto &f) {
            fills.on_fill(f);
            orders.on_fill(f);
        };
        cb.on_commission = [this](const auto &c) { fills.on_commission(c); };
        broker.set_callbacks(cb);
        broker.connect();
    }
};
struct Result {
    std::vector<Fill> fills;
    std::vector<EquityPoint> equity;
    double fees;
};
Result replay(const std::string &path, std::size_t window = 2, Instrument i = stock(), double cash = 1000,
              StrategyKind kind = StrategyKind::MovingAverage) {
    Portfolio p(cash);
    p.add_instrument(i);
    BrokerSim broker(1, 0);
    OrderManager orders(broker);
    FillProcessor fills(p);
    RiskManager risk({100000, 100000, 0, 1, 0});
    PerformanceReport report;
    CsvDataFeed feed(path);
    MovingAverageStrategy strategy(1, window, 1, kind);
    Backtester backtest(feed, broker, strategy, p, orders, fills, risk, report);
    backtest.run();
    check(orders.pending().empty(), "end-of-run orders must be cancelled");
    throws([&] { backtest.run(); });
    return {fills.executions(), report.points(), fills.fees()};
}
} // namespace
int main() {
    try {
        check(valid_timestamp("2024-02-29"), "leap date");
        check(!valid_timestamp("2026-02-29"), "invalid leap date");
        check(valid_timestamp("2026-01-01T23:59:59Z"), "UTC timestamp");
        check(!valid_timestamp("2026-01-01T24:00:00Z"), "invalid hour");
        check(!valid_timestamp("2026-01-01T10:00:00+08:00"), "explicit UTC only");
        auto bad = future();
        bad.initial_margin = 0;
        throws([&] { validate_instrument(bad); });
        bad = option();
        bad.right = "X";
        throws([&] { validate_instrument(bad); });
        Portfolio p(1000);
        p.add_instrument(stock());
        check(p.buy(1, 2.5, 103, 1), "fractional buy");
        near(p.cash(), 741.5);
        near(p.equity({{1, 101}}), 994);
        check(p.sell(1, 1, 105, 1), "partial sell");
        near(p.cash(), 845.5);
        near(p.shares(1), 1.5);
        near(p.positions().front().average_price, 103);
        const double cash = p.cash();
        check(!p.sell(1, 2, 105, 0), "cannot oversell");
        check(!p.buy(1, 1000, 100, 0), "cannot overspend");
        near(p.cash(), cash);
        check(!p.buy(1, 1, std::numeric_limits<double>::infinity(), 0), "nonfinite input rejected");
        check(!p.buy(1, 1, 1, -1), "negative fee rejected");
        throws([&] { p.equity({}); });
        throws([&] { p.equity({{1, -1}}); });
        check(p.sell(1, 1.5, 105, 1), "full close");
        near(p.shares(1), 0);
        check(p.positions().empty(), "closed position erased");
        throws([&] { p.add_instrument(stock()); });
        auto eur = stock(2);
        eur.currency = "EUR";
        throws([&] { p.add_instrument(eur); });
        throws([&] { p.restore(100, {{1, -1, 100}}); });
        near(p.shares(1), 0);
        Portfolio f(10000);
        f.add_instrument(future());
        check(f.buy(1, 2, 100, 2), "futures buy");
        near(f.cash(), 9998);
        near(f.margin_used(), 2000);
        near(f.equity({{1, 110}}), 10098);
        f.settle_futures({{1, 110}});
        near(f.cash(), 10098);
        near(f.positions().front().average_price, 110);
        check(f.sell(1, 1, 105, 1), "futures partial close");
        near(f.cash(), 10072);
        near(f.equity({{1, 105}}), 10047);
        f.settle_futures({{1, 120}});
        near(f.cash(), 10122);
        near(f.equity({{1, 120}}), 10122);
        Portfolio insufficient(100);
        insufficient.add_instrument(future());
        check(!insufficient.buy(1, 1, 100, 0), "futures margin enforced");
        Portfolio op(1000);
        op.add_instrument(option());
        check(op.buy(1, 1, 2.5, 1), "option premium buy");
        near(op.cash(), 749);
        near(op.equity({{1, 3}}), 1049);
        check(op.sell(1, 1, 3.5, 1), "option premium sell");
        near(op.cash(), 1098);
        check(!op.buy(1, 0.5, 2, 0), "no fractional option contracts");
        auto crypto = stock();
        crypto.asset_class = AssetClass::Crypto;
        Portfolio cp(1000);
        cp.add_instrument(crypto);
        check(cp.buy(1, 0.001, 60000, 1), "crypto fraction");
        near(cp.cash(), 939);
        Portfolio account(1000);
        account.add_instrument(stock());
        FillProcessor processor(account);
        Fill execution{"exec", 1, 1, Side::Buy, 2, 100, "2026-01-01"};
        processor.on_commission({"exec", 2, "USD"});
        near(account.cash(), 1000);
        processor.on_fill(execution);
        near(account.cash(), 798);
        processor.on_fill(execution);
        near(account.shares(1), 2);
        processor.on_commission({"exec", 2, "USD"});
        near(account.cash(), 798);
        processor.on_commission({"exec", 1, "USD"});
        near(account.cash(), 799);
        near(processor.fees(), 1);
        auto corrected = execution;
        corrected.quantity = 3;
        throws([&] { processor.on_fill(corrected); });
        throws([&] { processor.on_commission({"exec", 1, "EUR"}); });
        Simulation lifecycle;
        auto id = lifecycle.orders.submit(intent(Side::Buy, 2), 202, 0);
        lifecycle.orders.on_update({id, OrderStatus::Filled, 2, 0, ""});
        check(lifecycle.orders.pending().size() == 1, "status before executions must retain reservation");
        lifecycle.orders.on_fill({"a", id, 1, Side::Buy, 1, 100, ""});
        check(lifecycle.orders.pending().size() == 1, "await second fill");
        lifecycle.orders.on_fill({"a", id, 1, Side::Buy, 1, 100, ""});
        near(lifecycle.orders.all().front().filled_quantity, 1);
        lifecycle.orders.on_fill({"b", id, 1, Side::Buy, 1, 100, ""});
        check(lifecycle.orders.pending().empty(), "all executions accounted");
        lifecycle.orders.on_update({id, OrderStatus::Submitted, 0, 2, ""});
        check(lifecycle.orders.pending().empty(), "late status cannot regress terminal");
        Simulation sim(1000, 1, 0, 1);
        auto order = sim.orders.submit(intent(Side::Buy, 2), 202, 0);
        sim.broker.on_bar_open({"2026-01-01", 1, 100, 100});
        check(sim.fills.executions().empty(), "same-bar lookahead forbidden");
        sim.broker.on_bar_open({"2026-01-02", 1, 101, 99});
        near(sim.p.shares(1), 1);
        near(sim.p.cash(), 898);
        check(sim.orders.pending().size() == 1, "partial fill pending");
        near(sim.orders.pending().front().reserved_cash, 101);
        sim.broker.on_bar_open({"2026-01-03", 1, 102, 100});
        near(sim.p.shares(1), 2);
        near(sim.p.cash(), 795);
        near(sim.fills.fees(), 2);
        check(sim.orders.pending().empty(), "partial complete");
        sim.orders.cancel(order);
        near(sim.p.shares(1), 2);
        Simulation gap(100, 1);
        gap.orders.submit(intent());
        gap.broker.on_bar_open({"2026-01-02", 1, 101, 90});
        near(gap.p.shares(1), 0);
        check(gap.orders.all().front().status == OrderStatus::Rejected, "gap fill affordability");
        Simulation limit;
        auto lim = intent();
        lim.type = OrderType::Limit;
        lim.limit_price = 99;
        limit.orders.submit(lim);
        limit.broker.on_bar_open({"2026-01-02", 1, 100, 90});
        check(limit.fills.executions().empty(), "close crossing does not imply open fill");
        limit.broker.on_bar_open({"2026-01-03", 1, 98, 101});
        near(limit.p.cash(), 901);
        Simulation slip(1000, 1, 100);
        slip.orders.submit(intent());
        slip.broker.on_bar_open({"2026-01-02", 1, 100, 100});
        near(slip.fills.executions().front().price, 101);
        Simulation cancel;
        auto cancel_id = cancel.orders.submit(intent());
        cancel.orders.cancel(cancel_id);
        cancel.broker.on_bar_open({"2026-01-02", 1, 100, 100});
        check(cancel.fills.executions().empty(), "cancelled order not filled");
        RiskManager risk({1000, 2000, 100, 1, 0});
        auto o = intent();
        check(!risk.evaluate(o, account, {}, 100, true).allowed, "risk initially halted");
        risk.resume();
        check(risk.evaluate(o, account, {}, 100, true).allowed, "affordable intent");
        check(!risk.evaluate(o, account, {}, 100, false).allowed, "stale data rejected");
        auto large = o;
        large.quantity = 10;
        check(!risk.evaluate(large, account, {}, 100, true).allowed, "reserved buffer/cash enforced");
        check(!risk.evaluate(o, account, {{o, OrderStatus::Submitted, 0, 101, 0}}, 100, true).allowed,
              "pending duplicate rejected");
        auto sell = intent(Side::Sell, 3);
        check(!risk.evaluate(sell, account, {}, 100, true).allowed, "risk oversell");
        auto expired = intent();
        expired.created_at = "2026-12-18";
        check(!risk.evaluate(expired, op, {}, 2, true).allowed, "expired option cannot be ordered");
        PerformanceReport report;
        report.record("initial", 100);
        report.record("2026-01-01", 120);
        report.record("2026-01-02", 90);
        report.record("2026-01-03", 110);
        near(report.total_return(), 0.1);
        near(report.max_drawdown(), 0.25);
        throws([&] { report.record("2026-01-02", 100); });
        Temp temp;
        const std::string catalogue_header = "ticker,symbol,asset,currency,multiplier,lot_size,tick_size,"
                                             "initial_margin,expiry,strike,right,exchange,con_id\n";
        const std::string catalogue_row = "1,TEST,stock,USD,1,1,0.01,0,,0,,SMART,0";
        auto catalogue = temp.file("catalogue.csv", catalogue_header + catalogue_row + "\n");
        check(load_instruments(catalogue).size() == 1, "instrument CSV roundtrip");
        catalogue = temp.file("catalogue.csv", catalogue_header + catalogue_row + ",\n");
        throws([&] { load_instruments(catalogue); });
        auto good = temp.file(
            "good.csv", "date,ticker,open,close\n2026-01-01,1,100,100\n2026-01-02,1,100,101\n2026-01-03,1,"
                        "101,104\n2026-01-04,1,105,103\n2026-01-05,1,102,98\n2026-01-06,1,97,99\n");
        CsvDataFeed feed(good);
        feed.open();
        check(feed.next()->date == "2026-01-01", "feed first row");
        feed.reset();
        check(feed.next()->date == "2026-01-01", "feed reset");
        feed.set_range("2026-01-03", "2026-01-04");
        feed.reset();
        check(feed.next()->date == "2026-01-03", "from filter");
        check(feed.next()->date == "2026-01-04", "to inclusive");
        check(!feed.next(), "range EOF");
        throws([&] { feed.set_range("2026-02-30", ""); });
        for (const auto &row :
             {"2026-02-30,1,1,1", "2026-01-01,1,nan,1", "2026-01-01,1,1,1,extra", "2026-01-01,1,1,-1", ""}) {
            auto file = temp.file("bad.csv", std::string("date,ticker,open,close\n") + row + "\n");
            CsvDataFeed badfeed(file);
            badfeed.open();
            throws([&] { badfeed.next(); });
        }
        auto dup = temp.file("duplicate.csv", "date,ticker,open,close\n2026-01-01,1,1,1\n2026-01-01,1,1,1\n");
        CsvDataFeed df(dup);
        df.open();
        df.next();
        throws([&] { df.next(); });
        const auto r = replay(good);
        check(r.fills.size() == 2, "known replay has buy and sell");
        near(r.fills[0].price, 105);
        check(r.fills[0].executed_at == "2026-01-04", "signal executes next open");
        near(r.fills[1].price, 97);
        near(r.equity.back().equity, 990);
        near(r.fees, 2);
        {
            Portfolio initial(1000);
            initial.add_instrument(stock());
            initial.buy(1, 1, 100, 0);
            BrokerSim broker(1, 0);
            OrderManager orders(broker);
            FillProcessor fills(initial);
            RiskManager risks({10000, 10000, 0, 1, 0});
            PerformanceReport output;
            CsvDataFeed input(good);
            MovingAverageStrategy strategy(1, 2, 1);
            Backtester engine(input, broker, strategy, initial, orders, fills, risks, output);
            throws([&] { engine.run(); });
        }
        const auto repeat = replay(good);
        near(repeat.equity.back().equity, r.equity.back().equity);
        const auto mutated = temp.file(
            "future.csv", "date,ticker,open,close\n2026-01-01,1,100,100\n2026-01-02,1,100,101\n2026-01-03,1,"
                          "101,104\n2026-01-04,1,105,103\n2026-01-05,1,1000,1000\n2026-01-06,1,1000,1000\n");
        const auto causality = replay(mutated);
        near(causality.fills.front().price, r.fills.front().price);
        near(causality.equity[4].equity, r.equity[4].equity);
        auto expiry = temp.file(
            "expiry.csv", "date,ticker,open,close\n2026-12-15,1,2,2\n2026-12-16,1,2,2\n2026-12-18,1,2,2\n");
        throws([&] { replay(expiry, 2, option(), 1000, StrategyKind::BuyAndHold); });
        auto empty = temp.file("empty.csv", "date,ticker,open,close\n");
        throws([&] { replay(empty); });
        auto journal_path = (temp.dir / "events.journal").string();
        {
            EventJournal journal(journal_path);
            journal.open();
            journal.append({0, "time", "intent", "quoted \"value\""});
            journal.append({0, "time", "fill", "a"});
            check(journal.replay().size() == 2, "journal roundtrip");
            EventJournal another(journal_path);
            throws([&] { another.open(); });
        }
        {
            EventJournal journal(journal_path);
            journal.open();
            journal.append({0, "time", "commission", "1"});
            check(journal.replay().back().sequence == 3, "journal restart sequence");
        }
        {
            std::ofstream out(journal_path, std::ios::app);
            out << "torn";
        }
        {
            EventJournal journal(journal_path);
            throws([&] { journal.open(); });
        }
        {
            Portfolio paper(1000);
            paper.add_instrument(stock());
            BrokerSim broker(1, 0);
            broker.configure_account(paper);
            MovingAverageStrategy strategy(1, 2, 1, StrategyKind::BuyAndHold);
            OrderManager orders(broker);
            FillProcessor fills(paper);
            RiskManager risks({10000, 10000, 0, 1, 0});
            EventJournal journal((temp.dir / "paper.journal").string());
            TradingApp app(broker, strategy, paper, orders, fills, risks, journal);
            app.start();
            app.on_bar({"2026-01-01", 1, 100, 100});
            check(orders.pending().size() == 1, "paper intent submitted");
            broker.on_bar_open({"2026-01-02", 1, 101, 102});
            near(paper.cash(), 898);
            near(paper.shares(1), 1);
            app.on_bar({"2026-01-02", 1, 101, 102});
            check(orders.pending().empty(), "paper no duplicate buy");
            app.stop();
            check(journal.replay().size() >= 6, "paper events durably journaled");
            throws([&] { app.start(); }); // Never silently restart a process-local order map.
        }
        throws([] { IbkrBroker broker({"127.0.0.1", 7497, 17, "U123", true}); });
        if (!IbkrBroker::available()) {
            IbkrBroker broker({"127.0.0.1", 7497, 17, "DU123", true});
            throws([&] { broker.connect(); });
            check(!broker.ready(), "disabled adapter never ready");
        }
        std::cout << checks << " checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAILED after " << checks << " checks: " << e.what() << '\n';
        return 1;
    }
}
