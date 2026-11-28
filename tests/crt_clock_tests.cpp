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
void near(double a, double b, const std::string &what, double tol = 1e-6) {
    check(std::abs(a - b) <= tol,
          what + " (got " + std::to_string(a) + ", want " + std::to_string(b) + ")");
}

algo::Bar mk(const std::string &ts, double o, double h, double l, double c) {
    algo::Bar b;
    b.date = ts;
    b.ticker = 1;
    b.open = o;
    b.high = h;
    b.low = l;
    b.close = c;
    return b;
}

std::string stamp(const char *date, int hour, int minute) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%sT%02d:%02d:00Z", date, hour, minute);
    return buf;
}

void add_bucket(std::vector<algo::Bar> &out, const char *date, int start_hour, int start_minute,
                double o, double h, double l, double c, int bars = 16) {
    int hour = start_hour, minute = start_minute;
    for (int i = 0; i < bars; ++i) {
        out.push_back(mk(stamp(date, hour, minute), o, h, l, c));
        minute += 15;
        while (minute >= 60) {
            minute -= 60;
            ++hour;
        }
    }
}

algo::CrtClockConfig cfg() {
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

// 2026-01-05 EST: 09:30 ET = 14:30Z, 13:30 ET = 18:30Z.
std::vector<algo::Bar> bullish_fixture() {
    std::vector<algo::Bar> b;
    add_bucket(b, "2026-01-05", 14, 30, 105, 110, 100, 106);
    add_bucket(b, "2026-01-05", 18, 30, 104, 106, 101, 103, 15);
    b.push_back(mk(stamp("2026-01-05", 22, 15), 103, 106, 95, 105)); // sweep low, close inside
    b.push_back(mk(stamp("2026-01-05", 22, 30), 104, 105, 103, 104)); // exact entry bar
    b.push_back(mk(stamp("2026-01-05", 22, 45), 105, 111, 104, 110));
    return b;
}

// --- timezone ---------------------------------------------------------------------
void test_timezone_instants() {
    struct Case {
        const char *utc, *date;
        int minute;
    };
    // Ground truth from the IANA database (America/New_York).
    const Case cases[] = {
        {"2026-03-08T06:59:59Z", "2026-03-08", 1 * 60 + 59},   // last minute of EST
        {"2026-03-08T07:00:00Z", "2026-03-08", 3 * 60 + 0},    // DST begins, clock jumps
        {"2026-11-01T05:59:59Z", "2026-11-01", 1 * 60 + 59},   // last minute of EDT
        {"2026-11-01T06:00:00Z", "2026-11-01", 1 * 60 + 0},    // DST ends, clock repeats
        {"2026-01-06T03:00:00Z", "2026-01-05", 22 * 60 + 0},   // crosses midnight backwards
        {"2026-07-04T03:59:00Z", "2026-07-03", 23 * 60 + 59},  // previous local day
    };
    for (const auto &c : cases) {
        std::string date;
        int minute = -1;
        const bool ok = algo::et_local_from_utc(c.utc, date, minute);
        check(ok, std::string("conversion works for ") + c.utc);
        check(date == c.date, std::string(c.utc) + " date " + date + " want " + c.date);
        check(minute == c.minute,
              std::string(c.utc) + " minute " + std::to_string(minute) + " want " +
                  std::to_string(c.minute));
    }
}

// --- candle construction -----------------------------------------------------------
void test_duplicate_off_grid_and_missing() {
    auto b = bullish_fixture();
    // duplicate timestamp
    b.insert(b.begin() + 3, b[3]);
    // off-grid timestamp inside a bucket
    b.insert(b.begin() + 6, mk(stamp("2026-01-05", 15, 7), 105, 110, 100, 106));
    const auto r = algo::run_crt_clock(b, cfg());
    check(r.duplicate_bars == 1, "one duplicate timestamp counted");
    check(r.off_grid_bars == 1, "one off-grid timestamp counted");
}

void test_missing_bar_makes_candle_incomplete() {
    auto b = bullish_fixture();
    b.erase(b.begin() + 5); // drop one range-candle bar
    const auto r = algo::run_crt_clock(b, cfg());
    bool saw_incomplete = false;
    for (const auto &c : r.candles)
        if (!c.complete && c.session_date == "2026-01-05" && c.start_minute == 570)
            saw_incomplete = true;
    check(saw_incomplete, "missing bar makes the range candle incomplete");
    check(r.trades.empty(), "no trade without a complete range candle");
}

void test_missing_full_session_does_not_pair() {
    std::vector<algo::Bar> b;
    add_bucket(b, "2026-01-05", 14, 30, 105, 110, 100, 106);
    // 2026-01-05 13:30 bucket and the whole of 2026-01-06 are absent.
    add_bucket(b, "2026-01-07", 14, 30, 106, 111, 101, 107);
    add_bucket(b, "2026-01-07", 18, 30, 107, 108, 105, 106);
    b.push_back(mk(stamp("2026-01-07", 22, 30), 106, 107, 105, 106));
    const auto r = algo::run_crt_clock(b, cfg());
    check(r.pairs_examined == 1, "only the in-session pair is examined");
    for (const auto &t : r.trades)
        check(t.range_start >= "2026-01-07", "no pair spans the missing session");
    check(r.trades.empty(), "the in-session pair produced no trade");
}

// --- signals -----------------------------------------------------------------------
void test_close_above_range_high_is_not_bullish() {
    auto b = bullish_fixture();
    // Replace the sweep bar: sweeps the low but closes ABOVE the range high (110).
    b[31] = mk(stamp("2026-01-05", 22, 15), 103, 112, 95, 111.5);
    const auto r = algo::run_crt_clock(b, cfg());
    check(r.trades.empty(), "a close above the range high must not qualify as bullish");
}

void test_close_below_range_low_is_not_bearish() {
    std::vector<algo::Bar> b;
    add_bucket(b, "2026-01-05", 14, 30, 105, 110, 100, 106);
    add_bucket(b, "2026-01-05", 18, 30, 106, 111, 105, 107, 15);
    b.push_back(mk(stamp("2026-01-05", 22, 15), 106, 112, 99, 98.5)); // sweeps high, closes below low
    b.push_back(mk(stamp("2026-01-05", 22, 30), 100, 101, 99, 100));
    const auto r = algo::run_crt_clock(b, cfg());
    check(r.trades.empty(), "a close below the range low must not qualify as bearish");
}

void test_equality_is_not_inside() {
    auto b = bullish_fixture();
    b[31] = mk(stamp("2026-01-05", 22, 15), 103, 106, 95, 100.0); // closes exactly on the range low
    auto r = algo::run_crt_clock(b, cfg());
    check(r.trades.empty(), "close equal to the swept boundary is not 'inside'");

    auto b2 = bullish_fixture();
    b2[31] = mk(stamp("2026-01-05", 22, 15), 103, 111, 101, 110.0); // closes exactly on the high
    auto r2 = algo::run_crt_clock(b2, cfg());
    check(r2.trades.empty(), "close equal to the opposite boundary is not 'inside'");
}

void test_double_sided_sweep_skipped() {
    auto b = bullish_fixture();
    b[31] = mk(stamp("2026-01-05", 22, 15), 103, 115, 95, 105);
    const auto r = algo::run_crt_clock(b, cfg());
    check(r.trades.empty(), "double-sided sweep is skipped");
    check(r.skipped_double_sided == 1, "counted once");
}

// --- entry and lifecycle -----------------------------------------------------------
void test_missing_entry_bar_is_skipped() {
    auto b = bullish_fixture();
    b.erase(b.begin() + 32); // drop the 22:30 bar; 22:45 remains
    const auto r = algo::run_crt_clock(b, cfg());
    check(r.trades.empty(), "no entry without the exact expected bar");
    check(r.skipped_missing_entry_bar == 1, "counted as a missing entry bar");
}

void test_no_entry_while_position_open() {
    // Two signals in one session; the first trade is still open when the second entry bar arrives.
    std::vector<algo::Bar> b;
    add_bucket(b, "2026-01-05", 14, 30, 105, 110, 100, 106);          // range A
    add_bucket(b, "2026-01-05", 18, 30, 104, 106, 101, 103, 15);      // sweep A
    b.push_back(mk(stamp("2026-01-05", 22, 15), 103, 106, 95, 105));  // sweep low
    b.push_back(mk(stamp("2026-01-05", 22, 30), 104, 105, 103, 104)); // entry A
    // Trade stays open (no stop/target) while the next session forms a new signal.
    add_bucket(b, "2026-01-06", 14, 30, 104, 104.5, 103.5, 104);      // range B
    add_bucket(b, "2026-01-06", 18, 30, 104, 104.5, 103.5, 104, 15);  // sweep B
    b.push_back(mk(stamp("2026-01-06", 22, 15), 104, 104.4, 100, 103.8)); // sweeps low only
    b.push_back(mk(stamp("2026-01-06", 22, 30), 104, 104.5, 103.5, 104)); // would-be entry B
    auto c = cfg();
    c.max_hold_hours = 24; // the first trade is still open when the second signal fires
    const auto r = algo::run_crt_clock(b, c);
    check(r.trades.size() == 1, "only the first trade is taken");
    check(r.skipped_open_position >= 1, "the overlapping signal is counted, not taken later");
}

void test_cutoff_blocks_long_and_short() {
    auto c = cfg();
    c.cutoff_date = "2026-01-05";
    const auto r = algo::run_crt_clock(bullish_fixture(), c);
    check(r.trades.empty(), "long entry blocked on the cutoff date");
    check(r.skipped_cutoff >= 1, "counted as cutoff");

    std::vector<algo::Bar> b;
    add_bucket(b, "2026-01-05", 14, 30, 105, 110, 100, 106);
    add_bucket(b, "2026-01-05", 18, 30, 106, 111, 105, 107, 15);
    b.push_back(mk(stamp("2026-01-05", 22, 15), 106, 112, 105, 108));
    b.push_back(mk(stamp("2026-01-05", 22, 30), 107, 108, 106, 107));
    const auto rs = algo::run_crt_clock(b, c);
    check(rs.trades.empty(), "short entry blocked on the cutoff date");
}

// --- execution model ---------------------------------------------------------------
void test_adverse_tick_rounding() {
    auto b = bullish_fixture();
    auto c = cfg();
    c.tick_size = 0.5;
    c.slippage_bps = 1;
    const auto r = algo::run_crt_clock(b, c);
    check(r.trades.size() == 1, "one trade");
    if (r.trades.size() == 1) {
        const auto &t = r.trades.front();
        near(std::fmod(t.entry, 0.5), 0.0, "entry sits on the tick", 1e-9);
        check(t.entry >= t.entry_reference, "a long entry rounds up, never down");
        check(t.exit <= t.exit_reference, "a long exit rounds down, never up");
    }
}

void test_slippage_decomposition() {
    auto c = cfg();
    c.slippage_bps = 10;
    c.fee = 2.5;
    const auto r = algo::run_crt_clock(bullish_fixture(), c);
    check(r.trades.size() == 1, "one trade");
    if (r.trades.size() == 1) {
        const auto &t = r.trades.front();
        near(t.net, t.gross_pre_slippage - t.slippage - t.fees,
             "net = pre-slippage - slippage - fees", 1e-6);
        check(t.slippage > 0, "slippage is a cost on a long entry and exit");
        near(t.fees, 5.0, "two sides of fees");
        check(t.gross_pre_slippage > t.net, "pre-slippage P&L exceeds net");
    }
}

void test_gap_through_stop_is_adverse() {
    auto b = bullish_fixture();
    b[33] = mk(stamp("2026-01-05", 22, 45), 90, 92, 88, 89); // opens far below the stop
    const auto r = algo::run_crt_clock(b, cfg());
    check(r.trades.size() == 1, "one trade");
    if (r.trades.size() == 1) {
        check(r.trades.front().gap_through_stop, "gap flagged");
        check(r.trades.front().exit_reference <= 90.0, "fills at the worse open, not the stop");
    }
}

void test_reconciliation() {
    const auto r = algo::run_crt_clock(bullish_fixture(), cfg());
    double sum = 0;
    for (const auto &t : r.trades)
        sum += t.net;
    near(r.end_equity - r.start_notional, sum, "equity change equals summed net", 1e-6);
}
} // namespace

int main() {
    test_timezone_instants();
    test_duplicate_off_grid_and_missing();
    test_missing_bar_makes_candle_incomplete();
    test_missing_full_session_does_not_pair();
    test_close_above_range_high_is_not_bullish();
    test_close_below_range_low_is_not_bearish();
    test_equality_is_not_inside();
    test_double_sided_sweep_skipped();
    test_missing_entry_bar_is_skipped();
    test_no_entry_while_position_open();
    test_cutoff_blocks_long_and_short();
    test_adverse_tick_rounding();
    test_slippage_decomposition();
    test_gap_through_stop_is_adverse();
    test_reconciliation();
    if (failures) {
        std::fprintf(stderr, "%d clock-CRT check(s) failed\n", failures);
        return 1;
    }
    std::printf("Clock CRT (corrected): timezone instants, grid validation, signal boundaries, "
                "equality, missing entry bar, overlapping positions, cutoff, adverse rounding, "
                "slippage decomposition and reconciliation passed\n");
    return 0;
}
