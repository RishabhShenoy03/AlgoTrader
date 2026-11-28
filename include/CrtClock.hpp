#pragma once
#include "Bar.hpp"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace algo {

// Clock-aligned CRT research runner.
//
// Unlike CrtBacktest (which groups N bars and calls that a candle), this builds candles
// from declared clock windows in a declared timezone, so a candle that spans a session
// break, a weekend or a missing bar is not silently treated as four continuous hours.
//
// Every candle records how many 15-minute bars it actually received. A candle is complete
// only when it received exactly the expected number; incomplete candles are never used as
// a range or a signal candle, and the run reports how many were dropped.

struct CrtBucket {
    int start_minute; // minutes after midnight, exchange local time
    int length_minutes;
};

struct CrtClockConfig {
    std::vector<CrtBucket> buckets; // e.g. {570,240} = 09:30 for 4 hours
    double multiplier{1};
    double tick_size{0.01};
    double quantity{1};
    double fee{2.50};        // assumption per side, not an IBKR quote
    int slippage_bps{2};
    bool allow_shorts{true};
    double max_hold_hours{8};
    std::string cutoff_date; // YYYY-MM-DD: no entry on/after, positions forced out before
};

struct CrtClockCandle {
    std::string session_date; // exchange-local date of the bucket start
    std::string start_utc, end_utc;
    int start_minute{};
    int bars_seen{};
    int bars_expected{};
    double open{}, high{}, low{}, close{};
    bool complete{};
};

struct CrtClockTrade {
    std::string range_start, sweep_start, entry_time, exit_time;
    int direction{1};
    double entry{}, stop{}, target{}, exit{};
    std::string exit_reason; // target | stop | time | cutoff
    bool gap_through_stop{};
    bool ambiguous_bar{};    // stop and target both touched in one bar
    int bars_held{};
    double gross{}, slippage{}, fees{}, net{};
};

struct CrtClockResult {
    bool ok{};
    std::string error;
    std::vector<CrtClockCandle> candles;
    std::vector<CrtClockTrade> trades;
    int incomplete_candles{};
    int pairs_examined{};
    int setups{};
    int skipped_double_sided{};
    int skipped_gap_beyond_target{};
    int skipped_no_next_bar{};
    int skipped_cutoff{};
    double start_equity{}, end_equity{}, max_drawdown{};
};

// Exchange-local helper: US Eastern offset for a UTC timestamp, using the US DST rule
// (second Sunday of March to first Sunday of November). Exposed for tests.
int et_offset_hours(int year, int month, int day);

CrtClockResult run_crt_clock(const std::vector<Bar> &bars, const CrtClockConfig &config);
void write_crt_clock_report(const std::string &directory, const CrtClockResult &result,
                            const CrtClockConfig &config, const std::string &source);
} // namespace algo
