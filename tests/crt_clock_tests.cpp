#include "CrtClock.hpp"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures = 0;

void check(bool ok, const std::string &what) {
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what.c_str());
        ++failures;
    }
}
void near(double a, double b, const std::string &what, double tol = 1e-9) {
    check(std::abs(a - b) <= tol, what + " (got " + std::to_string(a) + ", want " + std::to_string(b) + ")");
}

std::string stamp(const char *date, int hour, int minute) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%sT%02d:%02d:00Z", date, hour, minute);
    return buf;
}

algo::Bar bar(const std::string &ts, double o, double h, double l, double c) {
    algo::Bar b;
    b.date = ts;
    b.ticker = 1;
    b.open = o;
    b.high = h;
    b.low = l;
    b.close = c;
    return b;
}

// 2026-01-05 is EST (UTC-5): 09:30 ET = 14:30Z.
void add_candle(std::vector<algo::Bar> &out, const char *date, int start_hour, int start_minute,
                double o, double h, double l, double c, int bars = 16) {
    int hour = start_hour, minute = start_minute;
    for (int i = 0; i < bars; ++i) {
        out.push_back(bar(stamp(date, hour, minute), o, h, l, c));
        minute += 15;
        while (minute >= 60) {
            minute -= 60;
            ++hour;
        }
    }
}

algo::CrtClockConfig config() {
    algo::CrtClockConfig c;
    c.buckets = {{570, 240}, {810, 240}}; // 09:30-13:30, 13:30-17:30 ET
    c.multiplier = 100;
    c.tick_size = 0.1;
    c.quantity = 1;
    c.fee = 2.5;
    c.slippage_bps = 0;
    c.max_hold_hours = 8;
    return c;
}

std::vector<algo::Bar> bullish_fixture() {
    std::vector<algo::Bar> b;
    add_candle(b, "2026-01-05", 14, 30, 105, 110, 100, 106); // range 100..110
    add_candle(b, "2026-01-05", 18, 30, 104, 106, 101, 103, 15);
    b.push_back(bar(stamp("2026-01-05", 22, 15), 103, 106, 95, 105)); // sweep low, close inside
    b.push_back(bar(stamp("2026-01-05", 22, 30), 104, 105, 103, 104)); // entry bar, opens 104
    b.push_back(bar(stamp("2026-01-05", 22, 45), 105, 111, 104, 110)); // reaches 110 target
    b.push_back(bar(stamp("2026-01-05", 23, 0), 110, 111, 109, 110));
    return b;
}

void test_dst_offsets() {
    near(algo::et_offset_hours(2026, 1, 5), -5, "January is EST");
    near(algo::et_offset_hours(2026, 7, 5), -4, "July is EDT");
    near(algo::et_offset_hours(2026, 3, 7), -5, "day before spring forward is EST");
    near(algo::et_offset_hours(2026, 3, 9), -4, "day after spring forward is EDT");
    near(algo::et_offset_hours(2026, 10, 31), -4, "day before fall back is EDT");
    near(algo::et_offset_hours(2026, 11, 2), -5, "after fall back is EST");
}

void test_candles_and_completeness() {
    std::vector<algo::Bar> b;
    add_candle(b, "2026-01-05", 14, 30, 105, 110, 100, 106); // complete
    add_candle(b, "2026-01-05", 18, 30, 104, 106, 101, 103, 10); // only 10 of 16 bars
    add_candle(b, "2026-01-06", 14, 30, 106, 108, 104, 107);     // complete
    add_candle(b, "2026-01-06", 18, 30, 107, 109, 105, 108);
    const auto r = algo::run_crt_clock(b, config());
    check(r.ok, "run completes");
    check(r.candles.size() == 4, "four buckets seen");
    check(r.incomplete_candles == 1, "the 10-bar bucket is counted incomplete");
    int complete = 0;
    for (const auto &c : r.candles)
        complete += c.complete ? 1 : 0;
    check(complete == 3, "three complete candles");
}

void test_bullish_signal_and_target() {
    const auto r = algo::run_crt_clock(bullish_fixture(), config());
    check(r.ok, "bullish run completes");
    check(r.trades.size() == 1, "one trade");
    if (r.trades.size() == 1) {
        const auto &t = r.trades.front();
        check(t.direction == 1, "fades the swept low");
        check(t.entry_time == stamp("2026-01-05", 22, 30), "entry is the bar after the sweep closes");
        near(t.entry, 104, "entry at that bar's open");
        near(t.stop, 95, "stop at the sweep low");
        near(t.target, 110, "target at the range high");
        near(t.exit, 110, "target filled");
        check(t.exit_reason == "target", "exit reason target");
        near(t.gross, (110 - 104) * 100.0, "gross uses the contract multiplier");
        near(t.net, t.gross - 5.0, "net deducts both fees");
    }
}

void test_gap_through_stop_fills_at_open() {
    std::vector<algo::Bar> b;
    add_candle(b, "2026-01-05", 14, 30, 105, 110, 100, 106);
    add_candle(b, "2026-01-05", 18, 30, 104, 106, 101, 103, 15);
    b.push_back(bar(stamp("2026-01-05", 22, 15), 103, 106, 95, 105));
    b.push_back(bar(stamp("2026-01-05", 22, 30), 104, 105, 103, 104));
    b.push_back(bar(stamp("2026-01-05", 22, 45), 90, 92, 88, 89)); // gaps far below the stop
    const auto r = algo::run_crt_clock(b, config());
    check(r.trades.size() == 1, "one trade");
    if (r.trades.size() == 1) {
        const auto &t = r.trades.front();
        check(t.gap_through_stop, "the gap is flagged");
        near(t.exit, 90, "fills at the worse open, not at the 95 stop");
        near(t.gross, (90 - 104) * 100.0, "loss reflects the gap");
    }
}

void test_ambiguous_bar_counts_as_stop() {
    std::vector<algo::Bar> b;
    add_candle(b, "2026-01-05", 14, 30, 105, 110, 100, 106);
    add_candle(b, "2026-01-05", 18, 30, 104, 106, 101, 103, 15);
    b.push_back(bar(stamp("2026-01-05", 22, 15), 103, 106, 95, 105));
    b.push_back(bar(stamp("2026-01-05", 22, 30), 104, 105, 103, 104));
    b.push_back(bar(stamp("2026-01-05", 22, 45), 104, 112, 94, 105)); // touches both
    const auto r = algo::run_crt_clock(b, config());
    check(r.trades.size() == 1, "one trade");
    if (r.trades.size() == 1) {
        check(r.trades.front().ambiguous_bar, "ambiguity is recorded");
        check(r.trades.front().exit_reason == "stop", "resolves as the stop");
        near(r.trades.front().exit, 95, "stop price");
    }
}

void test_double_sided_sweep_skipped() {
    std::vector<algo::Bar> b;
    add_candle(b, "2026-01-05", 14, 30, 105, 110, 100, 106);
    add_candle(b, "2026-01-05", 18, 30, 104, 115, 95, 105); // pierces both sides
    b.push_back(bar(stamp("2026-01-05", 22, 30), 104, 105, 103, 104));
    const auto r = algo::run_crt_clock(b, config());
    check(r.trades.empty(), "no trade on a double-sided sweep");
    check(r.skipped_double_sided == 1, "counted as skipped");
}

void test_cutoff_blocks_entries() {
    auto c = config();
    c.cutoff_date = "2026-01-05";
    const auto r = algo::run_crt_clock(bullish_fixture(), c);
    check(r.trades.empty(), "no entry on or after the cutoff");
    check(r.skipped_cutoff >= 1, "counted as cutoff skip");
}

void test_reconciliation() {
    const auto r = algo::run_crt_clock(bullish_fixture(), config());
    double sum = 0;
    for (const auto &t : r.trades)
        sum += t.net;
    near(r.end_equity - r.start_equity, sum, "equity change equals summed net", 1e-6);
}

void test_tick_rounding() {
    auto c = config();
    c.tick_size = 0.1;
    const auto r = algo::run_crt_clock(bullish_fixture(), c);
    if (!r.trades.empty()) {
        const auto &t = r.trades.front();
        near(std::round(t.entry / 0.1) * 0.1, t.entry, "entry is on the tick", 1e-9);
        near(std::round(t.exit / 0.1) * 0.1, t.exit, "exit is on the tick", 1e-9);
    }
}

void test_no_buckets() {
    auto c = config();
    c.buckets.clear();
    const auto r = algo::run_crt_clock(bullish_fixture(), c);
    check(!r.ok, "no declared buckets is an error");
}
} // namespace

int main() {
    test_dst_offsets();
    test_candles_and_completeness();
    test_bullish_signal_and_target();
    test_gap_through_stop_fills_at_open();
    test_ambiguous_bar_counts_as_stop();
    test_double_sided_sweep_skipped();
    test_cutoff_blocks_entries();
    test_reconciliation();
    test_tick_rounding();
    test_no_buckets();
    if (failures) {
        std::fprintf(stderr, "%d clock-CRT check(s) failed\n", failures);
        return 1;
    }
    std::printf("Clock CRT: DST, candle completeness, signals, gaps, ambiguity, cutoff, "
                "multiplier, tick rounding and reconciliation passed (30 checks)\n");
    return 0;
}
