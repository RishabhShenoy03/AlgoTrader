#pragma once
#include "Bar.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace algo {

// Candle Range Theory research runner: 4H range -> 15M entry, evaluated on a single
// intraday feed. This is a research path, not a production trading mode.
//
// Spec frozen for this experiment:
//   - A "4H candle" is 16 consecutive regular-hours 15-minute bars (four hours of
//     trading time). Because the paper account only has regular-hours history, candles
//     span the overnight close rather than aligning to a clock.
//   - Candle i sets the range from its high/low.
//   - Candle i+1 is the sweep: it must trade beyond one side of the range AND close back
//     inside it. Trading beyond without closing back inside is not a setup; that candle
//     then becomes the new range.
//   - Entry is at the open of the first 15-minute bar after the sweep candle closes,
//     fading the swept side (long when the low was swept, short when the high was).
//   - Stop sits at the sweep candle's extreme on the swept side, target at the opposite
//     edge of the range. When a single bar touches both, the stop is assumed to fill
//     first.
//   - Time exit after max_hold_bars bars, at that bar's close.
struct CrtConfig {
    std::size_t range_bars{16};    // 16 x 15 minutes = 4 hours of trading time
    std::size_t max_hold_bars{32}; // two candles
    double quantity{100};
    double fee{1.10};
    int slippage_bps{5};
    bool allow_shorts{true};
};

struct CrtTrade {
    std::string entry_time, exit_time;
    int direction{1}; // 1 long, -1 short
    double entry{}, stop{}, target{}, exit{};
    std::string exit_reason; // target | stop | time
    double pnl{};
    double r_multiple{};
};

struct CrtResult {
    std::vector<CrtTrade> trades;
    std::string error;
    bool ok{};
    double start_equity{}, end_equity{}, max_drawdown{};
    int setups{};
    int skipped_no_trade{};
};

CrtResult run_crt_backtest(const std::vector<Bar> &bars, const CrtConfig &config);

// Writes trades.csv, equity.csv and a markdown summary into `directory`.
void write_crt_report(const std::string &directory, const CrtResult &result, const CrtConfig &config,
                      const std::string &source, const std::string &range_description);

} // namespace algo
