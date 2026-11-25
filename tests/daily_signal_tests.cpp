#include "DailyLedger.hpp"
#include "DailySignal.hpp"
#include "TradingCalendar.hpp"
#include "MovingAverageStrategy.hpp"
#include "Portfolio.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <vector>
using namespace algo;
namespace {
int checks = 0;
void check(bool ok, const std::string &message) {
    ++checks;
    if (!ok)
        throw std::runtime_error(message);
}
void near(double actual, double expected, const std::string &label) {
    check(std::abs(actual - expected) < 1e-9,
          label + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}
template <class F> void throws(F f, const std::string &label) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception &) {
        caught = true;
    }
    check(caught, "expected an exception: " + label);
}
std::string date_at(std::int64_t day_offset) { return utc_date(day_offset * 86400); }
std::string epoch_date(const std::string &iso) {
    std::int64_t days = 0;
    if (!parse_daily_date(iso, days))
        throw std::runtime_error("bad test date " + iso);
    return date_at(days);
}
struct Temp {
    std::filesystem::path dir;
    Temp() {
        char pattern[] = "/tmp/algotrader-signal-XXXXXX";
        auto p = mkdtemp(pattern);
        if (!p)
            throw std::runtime_error("mkdtemp");
        dir = p;
    }
    ~Temp() { std::filesystem::remove_all(dir); }
};
// A synthetic series: every close is 100 + i, so averages are exact and easy to reason about.
std::vector<Bar> ramp(std::size_t count, double step = 1.0, std::uint64_t ticker = 1) {
    std::int64_t start = 0;
    if (!parse_daily_date("2024-01-01", start))
        throw std::runtime_error("bad start date");
    std::vector<Bar> bars;
    for (std::size_t i = 0; i < count; ++i) {
        const double close = 100.0 + step * static_cast<double>(i);
        bars.push_back({date_at(start + static_cast<std::int64_t>(i)), ticker, close, close});
    }
    return bars;
}
std::int64_t now_after(const std::string &date, int hour = 23) {
    std::int64_t days = 0;
    if (!parse_daily_date(date, days))
        throw std::runtime_error("bad date");
    return days * 86400 + hour * 3600;
}
BrokerView flat_broker() {
    BrokerView broker;
    broker.confirmed = true;
    broker.account = "DU000000";
    broker.currency = "USD";
    broker.cash = 50000;
    broker.shares = 0;
    return broker;
}
// --- prior window --------------------------------------------------------------------
void test_prior_window() {
    const auto bars = ramp(120); // closes 100..219
    SignalRequest request;
    request.window = 100;
    request.quantity = 1;
    request.now_unix = now_after(bars.back().date);
    const auto signal = evaluate_signal(bars, request);
    check(signal.ok, "evaluation should succeed: " + signal.error);
    check(signal.decision_date == bars.back().date, "decision bar should be the latest session");

    double expected = 0;
    for (std::size_t i = bars.size() - 101; i + 1 < bars.size(); ++i)
        expected += bars[i].close;
    expected /= 100.0;
    near(signal.prior_average, expected, "prior 100-session average");
    near(signal.decision_close, bars.back().close, "decision close");
    check(signal.window_first == bars[bars.size() - 101].date, "window should start 100 sessions back");
    check(signal.window_last == bars[bars.size() - 2].date, "window must end at the prior session");
    check(signal.state == SignalState::AboveAverage, "rising ramp is above its average");
    check(signal.target_shares && *signal.target_shares == 1.0, "target should be one share");
    // The decision close must not be part of its own average.
    double with_decision = (expected * 100.0 + bars.back().close) / 101.0;
    check(std::abs(signal.prior_average - with_decision) > 1e-6,
          "decision close must be excluded from the average");
}

// --- agreement with the backtester ----------------------------------------------------
Instrument aapl() {
    Instrument instrument;
    instrument.ticker = 1;
    instrument.symbol = "AAPL";
    instrument.asset_class = AssetClass::Stock;
    instrument.currency = "USD";
    instrument.multiplier = 1;
    instrument.lot_size = 1;
    instrument.tick_size = 0.01;
    instrument.exchange = "SMART";
    instrument.con_id = 265598;
    return instrument;
}
// A portfolio that already holds `shares` of the instrument, so the strategy sees a position.
Portfolio held_portfolio(double shares) {
    Portfolio portfolio(100000.0, "USD");
    portfolio.add_instrument(aapl());
    if (shares > 0)
        check(portfolio.buy(1, shares, 100.0, 0.0), "test portfolio buy should succeed");
    return portfolio;
}
void test_agrees_with_strategy() {
    auto bars = ramp(120);
    bars.back().close = 1.0; // crash the decision close below the prior average
    SignalRequest request;
    request.window = 100;
    request.quantity = 1;
    request.now_unix = now_after(bars.back().date, 23);
    const auto signal = evaluate_signal(bars, request);
    check(signal.ok, "evaluation should succeed: " + signal.error);
    check(signal.state == SignalState::BelowAverage, "crash bar is below its average");
    check(signal.target_shares && *signal.target_shares == 0.0, "below-average target is flat");

    // Replay exactly as the backtester does: same strategy, same bars, a held position.
    MovingAverageStrategy strategy(1, 100, 1);
    auto portfolio = held_portfolio(1);
    for (std::size_t i = 0; i + 1 < bars.size(); ++i)
        strategy.on_bar(bars[i], portfolio);
    check(strategy.prior_average().has_value(), "strategy window should be full after replay");
    near(*strategy.prior_average(), signal.prior_average, "strategy average must match the report");
    const auto orders = strategy.on_bar(bars.back(), portfolio);
    check(orders.size() == 1 && orders.front().side == Side::Sell,
          "strategy should emit exactly one SELL for the same decision bar");
    near(orders.front().quantity, 1.0, "sell quantity should be the held shares");

    // Above-average case: a flat portfolio must want to buy one share.
    auto up = ramp(120);
    SignalRequest up_request;
    up_request.window = 100;
    up_request.now_unix = now_after(up.back().date, 23);
    const auto up_signal = evaluate_signal(up, up_request);
    check(up_signal.state == SignalState::AboveAverage, "ramp is above average");
    MovingAverageStrategy up_strategy(1, 100, 1);
    auto up_flat = held_portfolio(0);
    for (std::size_t i = 0; i + 1 < up.size(); ++i)
        up_strategy.on_bar(up[i], up_flat);
    near(*up_strategy.prior_average(), up_signal.prior_average, "average must match on the up series");
    const auto buy_orders = up_strategy.on_bar(up.back(), up_flat);
    check(buy_orders.size() == 1 && buy_orders.front().side == Side::Buy,
          "strategy should want to buy when above the average and flat");
}

// --- warmup ---------------------------------------------------------------------------
void test_warmup() {
    const auto bars = ramp(100); // exactly the window, no decision day history
    SignalRequest request;
    request.window = 100;
    request.now_unix = now_after(bars.back().date);
    const auto signal = evaluate_signal(bars, request);
    check(!signal.ok, "100 bars cannot produce a 100-day prior average plus a decision bar");
    check(signal.error.find("warmup") != std::string::npos, "error should mention warmup: " + signal.error);

    SignalRequest smaller;
    smaller.window = 99;
    smaller.now_unix = request.now_unix;
    const auto ok = evaluate_signal(bars, smaller);
    check(ok.ok, "99-day window should be satisfiable by 100 bars: " + ok.error);
    check(ok.prior_bars_available == 99, "observed prior bars should be 99");
}

// --- equality -------------------------------------------------------------------------
void test_equality() {
    auto bars = ramp(120, 0.0); // every close identical -> decision close equals the average
    SignalRequest request;
    request.window = 100;
    request.now_unix = now_after(bars.back().date);
    const auto signal = evaluate_signal(bars, request);
    check(signal.ok, "evaluation should succeed: " + signal.error);
    check(signal.state == SignalState::EqualToAverage, "equal close must be detected");
    check(!signal.target_shares.has_value(), "equality asserts no target, it retains state");

    const auto with_broker = propose_action(signal, flat_broker());
    check(with_broker.status == ProposalStatus::NoActionRequired, "equality with a known position is a hold");

    auto unknown = flat_broker();
    unknown.confirmed = false;
    unknown.error = "snapshot failed";
    const auto unknown_proposal = propose_action(signal, unknown);
    check(unknown_proposal.status == ProposalStatus::BlockedUnknownBroker,
          "equality with unknown position must not be actionable");
    check(!unknown_proposal.side.has_value(), "no side may be proposed when the position is unknown");
}

// --- session completion and freshness --------------------------------------------------
void test_session_and_freshness() {
    const std::string today = "2026-11-24";
    const std::int64_t early = now_after(today, 10);
    const std::int64_t late = now_after(today, 22);
    check(!session_completed(today, early), "a bar dated today before 22:00 UTC may still be forming");
    check(session_completed(today, late), "a bar dated today is complete after 22:00 UTC");
    check(session_completed(epoch_date("2026-11-23"), late), "yesterday is always complete");
    check(!session_completed(epoch_date("2026-11-25"), late), "tomorrow is never complete");

    auto bars = ramp(120);
    SignalRequest request;
    request.window = 100;
    request.now_unix = now_after(bars.back().date, 10);
    const auto forming = evaluate_signal(bars, request);
    check(!forming.ok, "a still-forming latest bar must be refused, not treated as current");
    check(forming.error.find("not a completed session") != std::string::npos,
          "error should explain session completion: " + forming.error);

    request.now_unix = now_after(bars.back().date, 23) + 10 * 86400;
    const auto stale = evaluate_signal(bars, request);
    check(!stale.ok, "a ten-day-old session must fail the freshness check");
    check(stale.error.find("stale") != std::string::npos, "error should mention staleness: " + stale.error);

    request.as_of = bars.back().date;
    const auto replay = evaluate_signal(bars, request);
    check(replay.ok, "an explicit as-of replay of a completed session is allowed: " + replay.error);
    check(replay.freshness_waived, "as-of must record that freshness was waived");
}

// --- data quality ---------------------------------------------------------------------
void test_data_validation() {
    auto bars = ramp(120);
    auto duplicate = bars;
    duplicate[10].date = duplicate[9].date;
    check(!validate_daily_series(duplicate).ok, "duplicate dates must fail");

    auto unsorted = bars;
    std::swap(unsorted[5], unsorted[6]);
    check(!validate_daily_series(unsorted).ok, "unsorted rows must fail");

    auto intraday = bars;
    intraday[3].date = "2024-01-04T15:30:00Z";
    check(!validate_daily_series(intraday).ok, "intraday timestamps must not be accepted as daily bars");

    auto zero = bars;
    zero[7].close = 0;
    check(!validate_daily_series(zero).ok, "zero price must fail");

    auto nan = bars;
    nan[9].open = std::nan("");
    check(!validate_daily_series(nan).ok, "NaN price must fail");

    auto mixed = bars;
    mixed[11].ticker = 2;
    check(!validate_daily_series(mixed).ok, "mixed instruments must fail");

    auto bad_date = bars;
    bad_date[12].date = "2024-02-30";
    check(!validate_daily_series(bad_date).ok, "non-existent calendar dates must fail");

    check(!validate_daily_series({}).ok, "empty series must fail");

    SignalRequest request;
    request.window = 100;
    request.now_unix = now_after("2024-05-01");
    check(!evaluate_signal(duplicate, request).ok, "evaluation must refuse a bad series");

    // as-of for a date that is not in the series
    request.as_of = "2030-01-01";
    check(!evaluate_signal(bars, request).ok, "unknown as-of date must fail");
}

// --- broker reconciliation ------------------------------------------------------------
void test_reconciliation() {
    auto bars = ramp(120);
    bars.back().close = 1.0; // below average -> target flat
    SignalRequest request;
    request.window = 100;
    request.now_unix = now_after(bars.back().date, 23);
    const auto sell_signal = evaluate_signal(bars, request);
    check(sell_signal.ok, "evaluation should succeed: " + sell_signal.error);

    auto broker = flat_broker();
    broker.shares = 1;
    const auto trim = propose_action(sell_signal, broker);
    check(trim.status == ProposalStatus::Actionable, "holding 1 with a flat target should be actionable");
    check(trim.side && *trim.side == Side::Sell, "action should be a SELL");
    near(trim.quantity, 1.0, "sell quantity");

    auto flat = flat_broker();
    const auto nothing = propose_action(sell_signal, flat);
    check(nothing.status == ProposalStatus::NoActionRequired, "flat with a flat target needs no action");

    auto two_shares = flat_broker();
    two_shares.shares = 2;
    const auto unexpected = propose_action(sell_signal, two_shares);
    check(unexpected.status == ProposalStatus::BlockedUnexpectedPosition,
          "an unexpected share count must block the proposal");
    check(!unexpected.side.has_value(), "no side may be proposed for an unexpected position");

    auto fractional = flat_broker();
    fractional.shares = 0.5;
    check(propose_action(sell_signal, fractional).status == ProposalStatus::BlockedUnexpectedPosition,
          "fractional holdings must block the proposal");

    auto open_orders = flat_broker();
    OrderState state;
    state.order.id = 7;
    state.order.ticker = 1;
    state.order.quantity = 1;
    open_orders.open_orders.push_back(state);
    const auto blocked = propose_action(sell_signal, open_orders);
    check(blocked.status == ProposalStatus::BlockedOpenOrders, "open orders must block the proposal");

    auto unknown = flat_broker();
    unknown.confirmed = false;
    unknown.error = "IBKR snapshot timed out";
    const auto unknown_proposal = propose_action(sell_signal, unknown);
    check(unknown_proposal.status == ProposalStatus::BlockedUnknownBroker,
          "an unconfirmed broker state must block the proposal");
    check(unknown_proposal.reason.find("IBKR snapshot timed out") != std::string::npos,
          "the broker error should be carried into the reason");

    // Buy side: above average, flat account.
    auto up = ramp(120);
    SignalRequest up_request;
    up_request.window = 100;
    up_request.now_unix = now_after(up.back().date, 23);
    const auto buy_signal = evaluate_signal(up, up_request);
    const auto buy = propose_action(buy_signal, flat_broker());
    check(buy.status == ProposalStatus::Actionable, "flat with a one-share target should be actionable");
    check(buy.side && *buy.side == Side::Buy, "action should be a BUY");
    const auto already_held = propose_action(buy_signal, broker);
    check(already_held.status == ProposalStatus::NoActionRequired, "already holding the target is a no-op");

    SignalEvaluation failed;
    failed.error = "stale data";
    check(propose_action(failed, flat_broker()).status == ProposalStatus::BlockedNoSignal,
          "a failed evaluation cannot produce a proposal");
}

// --- report writing -------------------------------------------------------------------
void test_report_files() {
    auto bars = ramp(120);
    SignalRequest request;
    request.window = 100;
    request.now_unix = now_after(bars.back().date, 23);
    const auto signal = evaluate_signal(bars, request);
    check(signal.ok, "evaluation should succeed: " + signal.error);
    const auto proposal = propose_action(signal, flat_broker());
    std::vector<Bar> used(bars.end() - 101, bars.end());
    ReportContext context{"AAPL", "USD", "unit-test source", "algotrader daily-signal --test",
                          request.now_unix};

    Temp temp;
    const auto files = write_signal_report(temp.dir.string(), signal, flat_broker(), proposal, context, used);
    for (const auto &path : {files.markdown, files.json, files.bars_csv, files.command})
        check(std::filesystem::exists(path), "report file should exist: " + path);

    std::ifstream md(files.markdown);
    std::stringstream md_text;
    md_text << md.rdbuf();
    check(md_text.str().find("Prior 100-session average") != std::string::npos, "markdown should state the average");
    check(md_text.str().find("NOT SUBMITTED") != std::string::npos ||
              md_text.str().find("No order was submitted") != std::string::npos,
          "markdown must be explicit that nothing was submitted");
    check(md_text.str().find("2024-") != std::string::npos, "markdown should carry the session dates");

    std::ifstream json(files.json);
    std::stringstream json_text;
    json_text << json.rdbuf();
    check(json_text.str().find("\"submitted\": false") != std::string::npos, "json must record submitted=false");
    check(json_text.str().find("\"window\": 100") != std::string::npos, "json must record the window");

    std::ifstream bars_csv(files.bars_csv);
    std::stringstream csv_text;
    csv_text << bars_csv.rdbuf();
    std::size_t lines = 0;
    for (char ch : csv_text.str())
        lines += ch == '\n';
    check(lines == 102, "bars-used.csv should hold a header plus 101 rows, got " + std::to_string(lines));

    // A second run must never clobber the first: it gets its own directory and the
    // original bytes stay untouched.
    const auto first_bytes = std::ifstream(files.markdown);
    std::stringstream before_text;
    before_text << first_bytes.rdbuf();
    const auto second = write_signal_report(temp.dir.string(), signal, flat_broker(), proposal, context, used);
    check(second.directory != files.directory, "a second run must create a different directory");
    std::ifstream recheck(files.markdown);
    std::stringstream after_text;
    after_text << recheck.rdbuf();
    check(before_text.str() == after_text.str(), "the earlier report must not be overwritten");
}

// --- trading calendar -----------------------------------------------------------------
void test_calendar() {
    // 2026 US equity full closures, computed from the holiday rules.
    for (const char *day : {"2026-01-01", "2026-01-19", "2026-02-16", "2026-04-03", "2026-05-25",
                            "2026-06-19", "2026-07-03", "2026-09-07", "2026-11-26", "2026-12-25"})
        check(!is_us_equity_trading_day(day), std::string("holiday should close the market: ") + day);
    // Weekends.
    check(!is_us_equity_trading_day("2026-11-21"), "Saturday is not a trading day");
    check(!is_us_equity_trading_day("2026-11-22"), "Sunday is not a trading day");
    // Ordinary weekdays.
    for (const char *day : {"2026-11-24", "2026-11-25", "2026-11-27", "2026-07-02", "2026-12-24"})
        check(is_us_equity_trading_day(day), std::string("should be a trading day: ") + day);
    // A Saturday July 4th is observed on the Friday before, and the Saturday itself is a weekend.
    check(!is_us_equity_trading_day("2026-07-04"), "July 4 2026 falls on a Saturday");
    throws([] { is_us_equity_trading_day("2026-13-01"); }, "invalid calendar date should throw");

    const std::int64_t day = 20000; // arbitrary day number
    check(seconds_until_session_cutoff(day * 86400 + 10 * 3600) == 12 * 3600,
          "10:00 UTC should be 12h before the 22:00 UTC cutoff");
    check(seconds_until_session_cutoff(day * 86400 + 23 * 3600) < 0,
          "after the cutoff the remaining time is negative");
}

// --- decision ledger ------------------------------------------------------------------
void test_ledger() {
    Temp temp;
    const auto path = (temp.dir / "ledger.csv").string();
    check(!ledger_has_session(path, "AAPL", "2026-11-24"), "missing ledger must not report a session");

    LedgerEntry entry{"2026-11-24T22:30:00Z", "2026-11-24", "AAPL", "above-average", "1",
                      "0", "0", "ACTIONABLE", "", "", "", "results/x"};
    append_ledger(path, entry);
    check(!ledger_has_session(path, "AAPL", "2026-11-24"),
          "a report-only row must not block execution for that session");
    check(!ledger_has_session(path, "AAPL", "2026-11-23"), "other sessions stay unrecorded");
    check(!ledger_has_session(path, "MSFT", "2026-11-24"), "other symbols stay unrecorded");

    entry.status = "EXEC:INTENT";
    entry.side = "BUY";
    entry.quantity = "1";
    entry.limit_price = "271.9";
    append_ledger(path, entry);
    check(ledger_has_session(path, "AAPL", "2026-11-24"),
          "an execution row must block the session");
    const auto rows = read_ledger(path);
    check(rows.size() == 2, "both rows must be readable");
    check(rows[1].status == "EXEC:INTENT" && rows[1].side == "BUY", "fields must round-trip");
    check(rows[1].limit_price == "271.9", "limit price must round-trip");

    // Only one header, no matter how many appends.
    std::ifstream raw(path);
    std::stringstream text;
    text << raw.rdbuf();
    std::size_t headers = 0, lines = 0;
    std::istringstream scan(text.str());
    std::string line;
    while (std::getline(scan, line)) {
        ++lines;
        if (line.rfind("decided_at,", 0) == 0)
            ++headers;
    }
    check(headers == 1 && lines == 3, "ledger must keep a single header and append rows");

    LedgerEntry bad = entry;
    bad.report_dir = "results/a,b";
    throws([&] { append_ledger(path, bad); }, "a comma in a field must be refused");

    // Malformed content fails closed rather than silently ignoring the session.
    temp.dir;
    std::ofstream corrupt((temp.dir / "corrupt.csv"));
    corrupt << "decided_at,session\n2026-11-24T22:30:00Z,2026-11-24\n";
    corrupt.close();
    throws([&] { ledger_has_session((temp.dir / "corrupt.csv").string(), "AAPL", "2026-11-24"); },
           "an unreadable ledger must throw instead of answering");
}

// --- execution plan -------------------------------------------------------------------
void test_execution_plan() {
    SignalEvaluation signal;
    signal.ok = true;
    signal.decision_close = 100.13;
    signal.quantity = 1;
    Proposal buy;
    buy.status = ProposalStatus::Actionable;
    buy.side = Side::Buy;
    buy.quantity = 1;

    const auto upward = plan_execution(signal, buy, 0.05, 25);
    check(upward.ok, "buy plan should be produced: " + upward.error);
    near(upward.limit_price, 100.40, "buy limit rounds up to the tick");
    near(upward.reference_close, 100.13, "reference close");

    Proposal sell;
    sell.status = ProposalStatus::Actionable;
    sell.side = Side::Sell;
    sell.quantity = 1;
    const auto downward = plan_execution(signal, sell, 0.05, 25);
    check(downward.ok, "sell plan should be produced: " + downward.error);
    near(downward.limit_price, 99.85, "sell limit rounds down to the tick");

    const auto zero_band = plan_execution(signal, buy, 0.01, 0);
    check(zero_band.ok, "zero band is allowed");
    near(zero_band.limit_price, 100.13, "zero band keeps the close when it is tick aligned");

    Proposal none;
    none.status = ProposalStatus::NoActionRequired;
    check(!plan_execution(signal, none, 0.01, 25).ok, "a non-actionable proposal has no plan");

    check(!plan_execution(signal, buy, 0.0, 25).ok, "invalid tick size has no plan");
    check(!plan_execution(signal, buy, 0.01, 501).ok, "band above 500 bps is refused");

    SignalEvaluation failed;
    check(!plan_execution(failed, buy, 0.01, 25).ok, "a failed evaluation has no plan");
}
} // namespace

int main() {
    try {
        test_prior_window();
        test_agrees_with_strategy();
        test_warmup();
        test_equality();
        test_session_and_freshness();
        test_data_validation();
        test_reconciliation();
        test_report_files();
        test_calendar();
        test_ledger();
        test_execution_plan();
    } catch (const std::exception &e) {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 1;
    }
    std::cout << "Daily signal: prior window, strategy agreement, warmup, equality, session "
                 "completion, freshness, data quality, reconciliation, report immutability, "
                 "calendar, ledger and execution planning passed ("
              << checks << " checks)\n";
    return 0;
}
