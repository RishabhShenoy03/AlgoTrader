#include "CrtClock.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace algo {
namespace {

struct Stamp {
    int year{}, month{}, day{}, hour{}, minute{};
};

bool parse_stamp(const std::string &s, Stamp &out) {
    if (s.size() < 16)
        return false;
    try {
        out.year = std::stoi(s.substr(0, 4));
        out.month = std::stoi(s.substr(5, 2));
        out.day = std::stoi(s.substr(8, 2));
        out.hour = std::stoi(s.substr(11, 2));
        out.minute = std::stoi(s.substr(14, 2));
    } catch (...) {
        return false;
    }
    return out.month >= 1 && out.month <= 12 && out.day >= 1 && out.day <= 31 && out.hour >= 0 &&
           out.hour < 24 && out.minute >= 0 && out.minute < 60;
}

int days_in_month(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return d[m - 1];
}

// Day of week, 0 = Sunday (Sakamoto).
int weekday(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3)
        --y;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

int nth_sunday(int y, int m, int n) {
    const int first = 1 + (7 - weekday(y, m, 1)) % 7;
    return first + 7 * (n - 1);
}

// Add minutes in exchange-local time; returns the local date plus minutes of day.
void advance_local(Stamp &s, int add_minutes) {
    int total = s.hour * 60 + s.minute + add_minutes;
    while (total >= 1440) {
        total -= 1440;
        if (++s.day > days_in_month(s.year, s.month)) {
            s.day = 1;
            if (++s.month > 12) {
                s.month = 1;
                ++s.year;
            }
        }
    }
    s.hour = total / 60;
    s.minute = total % 60;
}

std::string iso(int y, int m, int d) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
    return buf;
}

double round_to_tick(double price, double tick) {
    if (!(tick > 0))
        return price;
    return std::round(price / tick) * tick;
}

} // namespace

int et_offset_hours(int year, int month, int day) {
    // US DST: starts second Sunday of March, ends first Sunday of November.
    const int start = nth_sunday(year, 3, 2);
    const int end = nth_sunday(year, 11, 1);
    const bool dst = (month > 3 && month < 11) || (month == 3 && day >= start) ||
                     (month == 11 && day < end);
    return dst ? -4 : -5;
}

CrtClockResult run_crt_clock(const std::vector<Bar> &bars, const CrtClockConfig &config) {
    CrtClockResult result;
    if (bars.empty()) {
        result.error = "no bars";
        return result;
    }
    if (config.buckets.empty()) {
        result.error = "no declared clock buckets";
        return result;
    }
    for (const auto &b : config.buckets)
        if (b.start_minute < 0 || b.start_minute >= 1440 || b.length_minutes <= 0 ||
            b.length_minutes % 15 != 0 || b.start_minute + b.length_minutes > 1440) {
            result.error = "bucket must start in-day and end in-day in 15-minute steps";
            return result;
        }

    // Group bars into candles keyed by (local date, bucket start).
    struct Builder {
        CrtClockCandle candle;
        bool started{};
    };
    std::map<std::pair<std::string, int>, Builder> builders;
    std::vector<std::pair<std::string, int>> order;

    for (const auto &bar : bars) {
        Stamp utc;
        if (!parse_stamp(bar.date, utc)) {
            result.error = "unparseable timestamp " + bar.date;
            return result;
        }
        // Convert to exchange-local time (ET).
        Stamp local = utc;
        local.hour += et_offset_hours(utc.year, utc.month, utc.day);
        advance_local(local, 0); // normalise negatives/overflow across midnight
        const int minute_of_day = local.hour * 60 + local.minute;
        const std::string date = iso(local.year, local.month, local.day);

        const CrtBucket *bucket = nullptr;
        for (const auto &b : config.buckets)
            if (minute_of_day >= b.start_minute && minute_of_day < b.start_minute + b.length_minutes) {
                bucket = &b;
                break;
            }
        if (bucket == nullptr)
            continue; // outside every declared window

        auto key = std::make_pair(date, bucket->start_minute);
        auto &entry = builders[key];
        if (!entry.started) {
            entry.started = true;
            entry.candle.session_date = date;
            entry.candle.start_minute = bucket->start_minute;
            entry.candle.start_utc = bar.date;
            entry.candle.open = bar.open;
            entry.candle.high = bar.high;
            entry.candle.low = bar.low;
            entry.candle.close = bar.close;
            entry.candle.bars_expected = bucket->length_minutes / 15;
            order.push_back(key);
        } else {
            entry.candle.high = std::max(entry.candle.high, bar.high);
            entry.candle.low = std::min(entry.candle.low, bar.low);
            entry.candle.close = bar.close;
        }
        entry.candle.end_utc = bar.date;
        ++entry.candle.bars_seen;
    }

    // Keep only complete candles, in time order, and require consecutive buckets.
    for (const auto &key : order) {
        auto candle = builders[key].candle;
        candle.complete = candle.bars_seen == candle.bars_expected;
        if (!candle.complete)
            ++result.incomplete_candles;
        result.candles.push_back(candle);
    }
    std::sort(result.candles.begin(), result.candles.end(),
              [](const CrtClockCandle &a, const CrtClockCandle &b) { return a.start_utc < b.start_utc; });

    std::vector<CrtClockCandle> usable;
    for (const auto &c : result.candles)
        if (c.complete)
            usable.push_back(c);
    if (usable.size() < 2) {
        result.error = "fewer than two complete candles";
        result.ok = true; // a valid run with nothing to do, not a failure
        return result;
    }

    result.start_equity = config.quantity * config.multiplier * usable.front().open;
    double equity = result.start_equity;
    const double slip = config.slippage_bps / 10000.0;

    std::map<std::string, std::size_t> bar_index;
    for (std::size_t i = 0; i < bars.size(); ++i)
        bar_index.emplace(bars[i].date, i);

    std::size_t index_of_position_end = 0; // no new entry before this bar index
    for (std::size_t i = 0; i + 1 < usable.size(); ++i) {
        const auto &range = usable[i];
        const auto &sweep = usable[i + 1];
        // Only consecutive buckets form a pair: a gap means a session break or a missing
        // session, and a sweep across it would not be the frozen setup.
        Stamp a, b;
        parse_stamp(range.start_utc, a);
        parse_stamp(sweep.start_utc, b);
        // A pair is only ever two *consecutive* buckets of the declared schedule. If the
        // following bucket is missing or incomplete it is not silently skipped in favour of
        // a candle hours later: that would not be the frozen setup.
        std::size_t range_bucket = 0;
        while (range_bucket < config.buckets.size() &&
               config.buckets[range_bucket].start_minute != range.start_minute)
            ++range_bucket;
        if (range_bucket >= config.buckets.size())
            continue;
        const bool last_bucket = range_bucket + 1 == config.buckets.size();
        const bool same_day_next =
            range.session_date == sweep.session_date &&
            sweep.start_minute == config.buckets[range_bucket + (last_bucket ? 0 : 1)].start_minute &&
            !last_bucket;
        const bool next_session_first =
            last_bucket && sweep.start_minute == config.buckets.front().start_minute &&
            sweep.start_utc > range.start_utc;
        if (!same_day_next && !next_session_first)
            continue;

        ++result.pairs_examined;
        const bool swept_low = sweep.low < range.low;
        const bool swept_high = sweep.high > range.high;
        if (swept_low && swept_high) {
            ++result.skipped_double_sided;
            continue;
        }
        int direction = 0;
        if (swept_low && sweep.close > range.low)
            direction = 1;
        else if (swept_high && sweep.close < range.high) {
            if (!config.allow_shorts) {
                ++result.skipped_cutoff; // counted separately below when reported
                continue;
            }
            direction = -1;
        }
        if (direction == 0)
            continue;

        const double stop = direction == 1 ? sweep.low : sweep.high;
        const double target = direction == 1 ? range.high : range.low;
        if (!config.cutoff_date.empty() && sweep.session_date >= config.cutoff_date) {
            ++result.skipped_cutoff;
            continue;
        }
        auto next = bar_index.upper_bound(sweep.end_utc);
        if (next == bar_index.end()) {
            ++result.skipped_no_next_bar;
            continue;
        }
        const std::size_t entry_index = next->second;
        if (entry_index < index_of_position_end)
            continue;
        const Bar &entry_bar = bars[entry_index];
        if (!config.cutoff_date.empty() && entry_bar.date.substr(0, 10) >= config.cutoff_date) {
            ++result.skipped_cutoff;
            continue;
        }
        if ((direction == 1 && entry_bar.open >= target) ||
            (direction == -1 && entry_bar.open <= target)) {
            ++result.skipped_gap_beyond_target;
            continue;
        }
        if ((direction == 1 && entry_bar.open <= stop) ||
            (direction == -1 && entry_bar.open >= stop)) {
            ++result.skipped_gap_beyond_target; // entry would already be invalid
            continue;
        }

        ++result.setups;
        CrtClockTrade trade;
        trade.range_start = range.start_utc;
        trade.sweep_start = sweep.start_utc;
        trade.entry_time = entry_bar.date;
        trade.direction = direction;
        trade.stop = stop;
        trade.target = target;
        trade.entry = round_to_tick(entry_bar.open * (1.0 + direction * slip), config.tick_size);

        const std::size_t max_bars = static_cast<std::size_t>(std::max(1.0, config.max_hold_hours * 4));
        std::size_t exit_index = entry_index;
        double exit_price = 0;
        std::string reason;
        for (std::size_t j = entry_index; j < bars.size(); ++j) {
            const Bar &bar = bars[j];
            if ((j - entry_index) >= max_bars) {
                exit_price = bars[j - 1].close;
                exit_index = j - 1;
                reason = "time";
                break;
            }
            if (!config.cutoff_date.empty() && bar.date.substr(0, 10) >= config.cutoff_date) {
                exit_price = bars[j - 1].close;
                exit_index = j - 1;
                reason = "cutoff";
                break;
            }
            const bool stop_hit = direction == 1 ? bar.low <= trade.stop : bar.high >= trade.stop;
            const bool target_hit = direction == 1 ? bar.high >= trade.target : bar.low <= trade.target;
            if (stop_hit && target_hit) {
                trade.ambiguous_bar = true;
                exit_price = trade.stop;
                if ((direction == 1 && bar.open < trade.stop) ||
                    (direction == -1 && bar.open > trade.stop)) {
                    exit_price = bar.open;
                    trade.gap_through_stop = true;
                }
                exit_index = j;
                reason = "stop";
                break;
            }
            if (stop_hit) {
                // A gap through the stop fills at the worse open, never at the stop price.
                if ((direction == 1 && bar.open < trade.stop) ||
                    (direction == -1 && bar.open > trade.stop)) {
                    exit_price = bar.open;
                    trade.gap_through_stop = true;
                } else {
                    exit_price = trade.stop;
                }
                exit_index = j;
                reason = "stop";
                break;
            }
            if (target_hit) {
                exit_price = trade.target;
                exit_index = j;
                reason = "target";
                break;
            }
            if (j + 1 == bars.size()) {
                exit_price = bar.close;
                exit_index = j;
                reason = "time";
                break;
            }
        }
        if (reason.empty()) {
            exit_price = bars.back().close;
            exit_index = bars.size() - 1;
            reason = "time";
        }
        trade.exit = round_to_tick(exit_price * (1.0 - direction * slip), config.tick_size);
        trade.exit_time = bars[exit_index].date;
        trade.exit_reason = reason;
        trade.bars_held = static_cast<int>(exit_index - entry_index) + 1;
        trade.gross = direction * (trade.exit - trade.entry) * config.multiplier * config.quantity;
        trade.fees = 2.0 * config.fee;
        trade.slippage = direction *
                         ((exit_price - entry_bar.open) - (trade.exit - trade.entry)) *
                         config.multiplier * config.quantity;
        trade.net = trade.gross - trade.fees;
        equity += trade.net;
        result.trades.push_back(trade);
        index_of_position_end = exit_index + 1;
        i = static_cast<std::size_t>(std::max<std::ptrdiff_t>(
            0, static_cast<std::ptrdiff_t>(i))); // keep scanning from this candle
    }

    result.end_equity = equity;
    double peak = result.start_equity, running = result.start_equity;
    for (const auto &t : result.trades) {
        running += t.net;
        peak = std::max(peak, running);
        result.max_drawdown = std::max(result.max_drawdown, 1.0 - running / peak);
    }
    result.ok = true;
    return result;
}

void write_crt_clock_report(const std::string &directory, const CrtClockResult &result,
                            const CrtClockConfig &config, const std::string &source) {
    std::filesystem::create_directories(directory);
    std::ofstream trades(directory + "/trades.csv");
    trades << "range_start,sweep_start,entry_time,exit_time,direction,entry,stop,target,exit,"
              "exit_reason,gap_through_stop,ambiguous_bar,bars_held,gross,fees,net\n"
           << std::setprecision(10);
    for (const auto &t : result.trades)
        trades << t.range_start << ',' << t.sweep_start << ',' << t.entry_time << ','
               << t.exit_time << ',' << (t.direction == 1 ? "long" : "short") << ',' << t.entry
               << ',' << t.stop << ',' << t.target << ',' << t.exit << ',' << t.exit_reason << ','
               << (t.gap_through_stop ? 1 : 0) << ',' << (t.ambiguous_bar ? 1 : 0) << ','
               << t.bars_held << ',' << t.gross << ',' << t.fees << ',' << t.net << '\n';

    std::ofstream candles(directory + "/candles.csv");
    candles << "session_date,start_utc,end_utc,start_minute,bars_seen,bars_expected,complete,open,"
               "high,low,close\n"
            << std::setprecision(10);
    for (const auto &c : result.candles)
        candles << c.session_date << ',' << c.start_utc << ',' << c.end_utc << ','
                << c.start_minute << ',' << c.bars_seen << ',' << c.bars_expected << ','
                << (c.complete ? 1 : 0) << ',' << c.open << ',' << c.high << ',' << c.low << ','
                << c.close << '\n';

    std::ofstream curve(directory + "/equity.csv");
    curve << "trade,timestamp,equity\n" << std::setprecision(10);
    double running = result.start_equity;
    curve << "0,start," << running << '\n';
    for (std::size_t i = 0; i < result.trades.size(); ++i) {
        running += result.trades[i].net;
        curve << (i + 1) << ',' << result.trades[i].exit_time << ',' << running << '\n';
    }

    int wins = 0, gaps = 0, ambiguous = 0;
    double gross = 0, fees = 0, net = 0;
    for (const auto &t : result.trades) {
        if (t.net > 0)
            ++wins;
        gaps += t.gap_through_stop ? 1 : 0;
        ambiguous += t.ambiguous_bar ? 1 : 0;
        gross += t.gross;
        fees += t.fees;
        net += t.net;
    }
    std::ostringstream md;
    md << "# Clock-aligned CRT - " << (config.buckets.empty() ? 0 : config.buckets.front().start_minute / 60)
       << ":" << std::setw(2) << std::setfill('0')
       << (config.buckets.empty() ? 0 : config.buckets.front().start_minute % 60)
       << " ET buckets\n\n"
       << "- Source: " << source << "\n"
       << "- Multiplier " << config.multiplier << ", tick " << config.tick_size << ", quantity "
       << config.quantity << "\n"
       << "- Costs (assumption): " << config.fee << " per side, " << config.slippage_bps
       << " bps slippage\n"
       << "- Cutoff date: " << (config.cutoff_date.empty() ? "none" : config.cutoff_date) << "\n\n"
       << "| Metric | Value |\n| --- | ---: |\n"
       << "| Candles built / incomplete | " << result.candles.size() << " / "
       << result.incomplete_candles << " |\n"
       << "| Candle pairs examined | " << result.pairs_examined << " |\n"
       << "| Setups taken | " << result.setups << " |\n"
       << "| Skipped: double-sided sweep | " << result.skipped_double_sided << " |\n"
       << "| Skipped: gap beyond target/stop | " << result.skipped_gap_beyond_target << " |\n"
       << "| Skipped: cutoff | " << result.skipped_cutoff << " |\n"
       << "| Trades | " << result.trades.size() << " |\n"
       << "| Wins | " << wins << " |\n"
       << "| Gross P&L | " << gross << " |\n"
       << "| Fees | " << -fees << " |\n"
       << "| Net P&L | " << net << " |\n"
       << "| Start equity | " << result.start_equity << " |\n"
       << "| End equity | " << result.end_equity << " |\n"
       << "| Max drawdown | " << 100.0 * result.max_drawdown << "% |\n"
       << "| Gap-through-stop trades | " << gaps << " |\n"
       << "| Ambiguous (stop+target same bar) | " << ambiguous << " |\n";
    std::ofstream(directory + "/report.md") << md.str();
}
} // namespace algo
