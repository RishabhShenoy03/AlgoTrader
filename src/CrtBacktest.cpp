#include "CrtBacktest.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace algo {
namespace {

struct Candle {
    std::size_t first_bar{};
    std::size_t last_bar{};
    double high{};
    double low{};
    double close{};
    double open{};
};

bool candle_of(const std::vector<Bar> &bars, std::size_t first, std::size_t count, Candle &out) {
    if (first + count > bars.size())
        return false;
    out.first_bar = first;
    out.last_bar = first + count - 1;
    out.open = bars[first].open;
    out.close = bars[out.last_bar].close;
    out.high = bars[first].high;
    out.low = bars[first].low;
    for (std::size_t i = first + 1; i <= out.last_bar; ++i) {
        out.high = std::max(out.high, bars[i].high);
        out.low = std::min(out.low, bars[i].low);
    }
    return true;
}

} // namespace

CrtResult run_crt_backtest(const std::vector<Bar> &bars, const CrtConfig &config) {
    CrtResult result;
    if (bars.size() < config.range_bars * 2 + 2) {
        result.error = "not enough bars for a range candle plus a sweep candle";
        return result;
    }
    if (config.range_bars == 0 || config.quantity <= 0 || !std::isfinite(config.quantity)) {
        result.error = "invalid range_bars or quantity";
        return result;
    }

    const double slip = config.slippage_bps / 10000.0;
    double equity = config.quantity * bars.front().close;
    result.start_equity = equity;

    std::size_t range_first = 0;
    Candle range;
    if (!candle_of(bars, range_first, config.range_bars, range)) {
        result.error = "cannot build the first range candle";
        return result;
    }

    std::size_t position_open_until = 0; // no position while index < this
    for (;;) {
        const std::size_t sweep_first = range.last_bar + 1;
        Candle sweep;
        if (!candle_of(bars, sweep_first, config.range_bars, sweep))
            break;

        const bool swept_high = sweep.high > range.high;
        const bool swept_low = sweep.low < range.low;
        const bool closed_inside_high = swept_high && sweep.close < range.high;
        const bool closed_inside_low = swept_low && sweep.close > range.low;

        const std::size_t entry_index = sweep.last_bar + 1;
        const bool can_enter = entry_index < bars.size() && entry_index >= position_open_until;

        int direction = 0;
        double stop = 0, target = 0;
        if (can_enter && closed_inside_low && !closed_inside_high) {
            direction = 1;
            stop = sweep.low;
            target = range.high;
        } else if (can_enter && closed_inside_high && !closed_inside_low) {
            if (!config.allow_shorts) {
                ++result.skipped_no_trade;
            } else {
                direction = -1;
                stop = sweep.high;
                target = range.low;
            }
        }

        if (direction != 0 && ((direction == 1 && target > bars[entry_index].open) ||
                              (direction == -1 && target < bars[entry_index].open))) {
            ++result.setups;
            double entry = bars[entry_index].open * (1.0 + direction * slip);
            const double risk = direction == 1 ? entry - stop : stop - entry;
            if (!(risk > 0)) {
                range = sweep;
                continue;
            }

            CrtTrade trade;
            trade.entry_time = bars[entry_index].date;
            trade.direction = direction;
            trade.entry = entry;
            trade.stop = stop;
            trade.target = target;

            double exit_price = 0;
            std::string reason;
            std::size_t exit_index = entry_index;
            const std::size_t limit = std::min(bars.size() - 1, entry_index + config.max_hold_bars);
            for (std::size_t i = entry_index; i <= limit; ++i) {
                const Bar &bar = bars[i];
                // Conservative: when one bar covers both levels, the stop is assumed first.
                const bool stop_hit = direction == 1 ? bar.low <= stop : bar.high >= stop;
                const bool target_hit = direction == 1 ? bar.high >= target : bar.low <= target;
                if (stop_hit) {
                    // A bar that opens beyond the stop fills at the open, not at the stop:
                    // assuming the stop price on a gap is the classic way a backtest invents
                    // money that a real stop order could not have captured.
                    exit_price = direction == 1 ? std::min(stop, bar.open) : std::max(stop, bar.open);
                    reason = "stop";
                    exit_index = i;
                    break;
                }
                if (target_hit) {
                    exit_price = target;
                    reason = "target";
                    exit_index = i;
                    break;
                }
                if (i == limit) {
                    exit_price = bar.close;
                    reason = "time";
                    exit_index = i;
                }
            }
            exit_price *= (1.0 - direction * slip);
            trade.exit = exit_price;
            trade.exit_time = bars[exit_index].date;
            trade.exit_reason = reason;
            trade.pnl = direction * (exit_price - entry) * config.quantity - 2.0 * config.fee;
            trade.r_multiple = trade.pnl / (risk * config.quantity);
            equity += trade.pnl;
            result.trades.push_back(trade);
            position_open_until = exit_index + 1;
            // The next range starts with the candle after the one that produced the trade.
            range = sweep;
            std::size_t next = sweep.last_bar + 1;
            if (!candle_of(bars, next, config.range_bars, range))
                break;
            continue;
        }

        // No setup: the sweep candle becomes the new range.
        range = sweep;
    }

    result.end_equity = equity;
    double peak = result.start_equity;
    for (const auto &trade : result.trades) {
        (void)trade;
    }
    // Rebuild the curve to measure drawdown in trade order.
    double running = result.start_equity;
    for (const auto &trade : result.trades) {
        running += trade.pnl;
        peak = std::max(peak, running);
        result.max_drawdown = std::max(result.max_drawdown, 1.0 - running / peak);
    }
    result.ok = true;
    return result;
}

void write_crt_report(const std::string &directory, const CrtResult &result, const CrtConfig &config,
                      const std::string &source, const std::string &range_description) {
    std::filesystem::create_directories(directory);
    std::ofstream trades(directory + "/trades.csv");
    trades << "entry_time,exit_time,direction,entry,stop,target,exit,exit_reason,pnl,r_multiple\n"
           << std::setprecision(10);
    for (const auto &t : result.trades)
        trades << t.entry_time << ',' << t.exit_time << ',' << (t.direction == 1 ? "long" : "short")
               << ',' << t.entry << ',' << t.stop << ',' << t.target << ',' << t.exit << ','
               << t.exit_reason << ',' << t.pnl << ',' << t.r_multiple << '\n';

    int wins = 0, stops = 0, targets = 0, times = 0;
    double gross_win = 0, gross_loss = 0;
    for (const auto &t : result.trades) {
        if (t.pnl > 0)
            ++wins;
        if (t.exit_reason == "stop")
            ++stops;
        else if (t.exit_reason == "target")
            ++targets;
        else
            ++times;
        if (t.pnl > 0)
            gross_win += t.pnl;
        else
            gross_loss -= t.pnl;
    }
    const double total = result.end_equity - result.start_equity;
    const double return_pct = 100.0 * total / result.start_equity;

    std::ostringstream md;
    std::filesystem::create_directories(directory);
    // Realised equity after each closed trade: the same series the return and drawdown are
    // computed from. Unrealised excursions are not in it (stated in the report).
    std::ofstream curve(directory + "/equity.csv");
    curve << "trade,timestamp,equity\n" << std::setprecision(10);
    double running_equity = result.start_equity;
    curve << "0,start," << running_equity << '\n';
    for (std::size_t i = 0; i < result.trades.size(); ++i) {
        running_equity += result.trades[i].pnl;
        curve << (i + 1) << ',' << result.trades[i].exit_time << ',' << running_equity << '\n';
    }
    md << "# CRT backtest - " << range_description << "\n\n"
       << "- Source: " << source << "\n"
       << "- Candle: " << config.range_bars << " x 15m bars (4 hours of trading time)\n"
       << "- Max hold: " << config.max_hold_bars << " bars; quantity " << config.quantity << "\n"
       << "- Costs: " << config.fee << " per side, " << config.slippage_bps << " bps slippage\n\n"
       << "| Metric | Value |\n| --- | ---: |\n"
       << "| Setups taken | " << result.setups << " |\n"
       << "| Trades | " << result.trades.size() << " |\n"
       << "| Win rate | "
       << (result.trades.empty() ? 0.0 : 100.0 * wins / result.trades.size()) << "% |\n"
       << "| Exits: target / stop / time | " << targets << " / " << stops << " / " << times << " |\n"
       << "| Net P&L | " << std::fixed << std::setprecision(2) << total << " |\n"
       << "| Return on deployed capital | " << return_pct << "% |\n"
       << "| Max drawdown | " << 100.0 * result.max_drawdown << "% |\n"
       << "| Sum of winning trades / losing trades (net of costs) | " << gross_win << " / "
       << gross_loss << " |\n\n"
       << "## Rules frozen for this run\n\n"
       << "Range candle sets high/low; the next candle must trade beyond one side and close\n"
       << "back inside. Entry is the next 15-minute bar's open, fading the swept side. Stop at\n"
       << "the sweep extreme, target at the opposite range edge, time exit after the maximum\n"
       << "hold. When one bar touches both levels the stop is assumed to fill first, so the\n"
       << "result errs pessimistic on every ambiguous bar.\n\n"
       << "Candles are built from regular-hours bars only, so a \"4H\" candle is four hours of\n"
       << "trading time and spans the overnight close. It is not aligned to a clock.\n";
    std::ofstream(directory + "/report.md") << md.str();
}
} // namespace algo
