#include "CrtBacktest.hpp"
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int failures = 0;

void check(bool condition, const std::string &what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what.c_str());
        ++failures;
    }
}

void near(double actual, double expected, const std::string &what) {
    check(std::abs(actual - expected) < 1e-6, what + " (got " + std::to_string(actual) + ")");
}

std::vector<algo::Bar> bars(std::size_t count, double open, double high, double low, double close) {
    std::vector<algo::Bar> out;
    for (std::size_t i = 0; i < count; ++i) {
        algo::Bar b;
        b.date = "2026-01-05T" + std::to_string(10 + i / 4) + ":00:00Z";
        b.ticker = 1;
        b.open = open;
        b.high = high;
        b.low = low;
        b.close = close;
        out.push_back(b);
    }
    return out;
}

void append(std::vector<algo::Bar> &into, std::size_t count, double open, double high, double low,
            double close) {
    for (auto &b : bars(count, open, high, low, close))
        into.push_back(b);
}

algo::CrtConfig config() {
    algo::CrtConfig c;
    c.range_bars = 4;      // short candles keep the fixture readable
    c.max_hold_bars = 6;
    c.quantity = 10;
    c.fee = 0;
    c.slippage_bps = 0;
    return c;
}

// Range 100..110, then a candle that sweeps the low and closes back inside: long setup.
std::vector<algo::Bar> long_setup() {
    std::vector<algo::Bar> b = bars(4, 105, 110, 100, 106);   // range candle
    append(b, 4, 104, 106, 95, 105);                          // sweep low, close inside
    return b;
}

void test_target_exit() {
    auto b = long_setup();
    append(b, 1, 104, 105, 103, 104.5);   // entry bar, opens 104
    append(b, 1, 105, 111, 104, 110.5);   // trades through the target at 110
    const auto r = algo::run_crt_backtest(b, config());
    check(r.ok, "target scenario runs");
    check(r.trades.size() == 1, "one trade");
    if (r.trades.size() == 1) {
        const auto &t = r.trades.front();
        check(t.direction == 1, "fades the swept low with a long");
        near(t.entry, 104, "enters at the next bar's open");
        near(t.stop, 95, "stop at the sweep low");
        near(t.target, 110, "target at the opposite range edge");
        near(t.exit, 110, "exits at the target");
        check(t.exit_reason == "target", "exit reason is target");
        near(t.pnl, 60, "(110 - 104) * 10 shares");
    }
}

void test_stop_exit() {
    auto b = long_setup();
    append(b, 1, 104, 105, 103, 104.5);
    append(b, 1, 104, 104, 94, 95);       // takes out the stop
    const auto r = algo::run_crt_backtest(b, config());
    check(r.trades.size() == 1, "stop scenario produces one trade");
    if (r.trades.size() == 1) {
        check(r.trades.front().exit_reason == "stop", "exit reason is stop");
        near(r.trades.front().exit, 95, "exits at the stop");
        near(r.trades.front().pnl, -90, "(95 - 104) * 10 shares");
        check(r.trades.front().r_multiple < 0, "a stop is a negative R");
    }
}

void test_stop_wins_when_one_bar_covers_both() {
    auto b = long_setup();
    append(b, 1, 104, 105, 103, 104.5);
    append(b, 1, 104.5, 111, 94, 104);    // touches both target and stop
    const auto r = algo::run_crt_backtest(b, config());
    if (r.trades.size() == 1)
        check(r.trades.front().exit_reason == "stop",
              "an ambiguous bar must resolve as the stop, not the target");
    else
        check(false, "ambiguous bar still produces a trade");
}

void test_time_exit() {
    auto b = long_setup();
    append(b, 8, 104, 106, 101, 105);     // drifts inside both levels
    const auto r = algo::run_crt_backtest(b, config());
    check(r.trades.size() == 1, "time scenario produces one trade");
    if (r.trades.size() == 1) {
        check(r.trades.front().exit_reason == "time", "exit reason is time");
        near(r.trades.front().exit, 105, "exits at the close");
    }
}

void test_no_setup_when_close_stays_outside() {
    std::vector<algo::Bar> b = bars(4, 105, 110, 100, 106);
    append(b, 4, 104, 115, 103, 114);     // breaks out and closes above the range
    append(b, 4, 110, 111, 105, 107);     // new range
    append(b, 1, 107, 108, 106, 107.5);
    append(b, 1, 107, 111, 106, 110);
    const auto r = algo::run_crt_backtest(b, config());
    check(r.trades.empty(), "a close outside the range is not a setup");
    check(r.end_equity == r.start_equity, "no trade leaves equity unchanged");
}

void test_short_setup() {
    std::vector<algo::Bar> b = bars(4, 105, 110, 100, 106);
    append(b, 4, 106, 115, 105, 108);     // sweeps the high, closes inside
    append(b, 1, 108, 109, 107, 108.5);   // entry bar
    append(b, 1, 108, 109, 99, 100);      // reaches the range low
    const auto r = algo::run_crt_backtest(b, config());
    check(r.trades.size() == 1, "short scenario produces one trade");
    if (r.trades.size() == 1) {
        const auto &t = r.trades.front();
        check(t.direction == -1, "fades the swept high with a short");
        near(t.stop, 115, "stop above the swept high");
        near(t.target, 100, "target at the range low");
        check(t.exit_reason == "target", "short reaches its target");
    }
}

void test_long_only_skips_shorts() {
    std::vector<algo::Bar> b = bars(4, 105, 110, 100, 106);
    append(b, 4, 106, 115, 105, 108);
    append(b, 1, 108, 109, 107, 108.5);
    append(b, 1, 108, 109, 99, 100);
    auto c = config();
    c.allow_shorts = false;
    const auto r = algo::run_crt_backtest(b, c);
    check(r.trades.empty(), "long-only ignores a swept high");
    check(r.skipped_no_trade == 1, "the skipped short is counted");
}

void test_insufficient_bars() {
    auto b = bars(3, 1, 1, 1, 1);
    const auto r = algo::run_crt_backtest(b, config());
    check(!r.ok, "too few bars is an error, not an empty result");
}
} // namespace

int main() {
    test_target_exit();
    test_stop_exit();
    test_stop_wins_when_one_bar_covers_both();
    test_time_exit();
    test_no_setup_when_close_stays_outside();
    test_short_setup();
    test_long_only_skips_shorts();
    test_insufficient_bars();
    if (failures) {
        std::fprintf(stderr, "%d CRT check(s) failed\n", failures);
        return 1;
    }
    std::printf("CRT backtest: exit reasons, side selection, ambiguity pessimism and input "
                "validation passed (26 checks)\n");
    return 0;
}
