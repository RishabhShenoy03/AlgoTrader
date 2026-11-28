#pragma once
#include "Bar.hpp"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace algo {

// Clock-aligned CRT research runner (corrected after review).
//
// Changes from the first version, each covered by a regression test:
//  - UTC -> exchange-local uses the system tz database (America/New_York), so midnight
//    crossings and the exact DST transition instants are correct;
//  - a candle is complete only if every expected 15-minute slot arrived exactly once and
//    on the grid; duplicates and off-grid timestamps are counted and rejected;
//  - a signal requires the close to be strictly inside BOTH range boundaries;
//  - the entry bar must be the exact expected next 15-minute slot, otherwise the setup is
//    skipped and counted;
//  - P&L is split into pre-slippage, slippage, fees and net with net = pre - slippage - fees;
//  - execution prices are rounded adversely to the tick for the order's side.

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
    double max_hold_hours{8}; // count of 15-minute bars; gaps while holding stop the run
    std::string cutoff_date; // YYYY-MM-DD ET: no entry on/after, positions forced out before
};

struct CrtClockCandle {
    std::string session_date; // exchange-local date of the bucket start
    std::string start_utc, end_utc;
    int start_minute{};
    int bars_seen{};
    int bars_expected{};
    double open{}, high{}, low{}, close{};
    bool complete{};
    std::string incomplete_reason;
};

struct CrtClockTrade {
    std::string range_start, sweep_start, entry_time, exit_time;
    int direction{1};
    double entry{}, stop{}, target{}, exit{};
    double entry_reference{}, exit_reference{}; // unslipped prices the fill is measured against
    std::string exit_reason; // target | stop | time | cutoff
    bool gap_through_stop{};
    bool ambiguous_bar{};    // stop and target both touched in one bar
    int bars_held{};
    double gross_pre_slippage{}, slippage{}, fees{}, net{};
};

struct CrtClockResult {
    bool ok{};
    std::string error;
    std::vector<CrtClockCandle> candles;
    std::vector<CrtClockTrade> trades;
    int incomplete_candles{};
    int duplicate_bars{};
    int off_grid_bars{};
    int pairs_examined{};
    int setups{};
    int skipped_double_sided{};
    int skipped_close_outside{};
    int skipped_gap_beyond_target{};
    int skipped_missing_entry_bar{};
    int skipped_open_position{};
    int skipped_cutoff{};
    double start_notional{}, end_equity{}, max_drawdown{};
};

// UTC ISO timestamp -> exchange-local (America/New_York) date and minute-of-day.
bool et_local_from_utc(const std::string &utc_iso, std::string &date, int &minute_of_day);

CrtClockResult run_crt_clock(const std::vector<Bar> &bars, const CrtClockConfig &config);
void write_crt_clock_report(const std::string &directory, const CrtClockResult &result,
                            const CrtClockConfig &config, const std::string &source);
} // namespace algo
