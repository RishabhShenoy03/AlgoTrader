#include "CrtClock.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
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

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + static_cast<int>(doe) - 719468;
}

std::string iso_date(int y, int m, int d) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
    return buf;
}

void use_exchange_timezone() {
    // Single-threaded research runner: setting TZ once is enough, and glibc reads the real
    // tz database rather than a hand-coded DST rule.
    static bool done = false;
    if (!done) {
        setenv("TZ", "America/New_York", 1);
        tzset();
        done = true;
    }
}

double round_adverse(double price, double tick, int direction, bool is_entry) {
    if (!(tick > 0))
        return price;
    const double units = price / tick;
    // Buying (or covering a short) rounds up; selling rounds down. Entry and exit use the
    // side of the order, which is the same as the trade direction at entry and the
    // opposite at exit.
    const bool round_up = is_entry ? direction == 1 : direction == -1;
    return (round_up ? std::ceil(units - 1e-9) : std::floor(units + 1e-9)) * tick;
}

std::string minutes_to_hm(int m) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d:%02d", m / 60, m % 60);
    return buf;
}

} // namespace

bool et_local_from_utc(const std::string &utc_iso, std::string &date, int &minute_of_day) {
    Stamp s;
    if (!parse_stamp(utc_iso, s))
        return false;
    use_exchange_timezone();
    // Build the UTC epoch from the civil date (no timegm dependency on struct tm fields).
    const std::int64_t days = days_from_civil(s.year, static_cast<unsigned>(s.month),
                                              static_cast<unsigned>(s.day));
    const std::time_t epoch = static_cast<std::time_t>(days * 86400 + s.hour * 3600 + s.minute * 60);
    std::tm local{};
    if (localtime_r(&epoch, &local) == nullptr)
        return false;
    date = iso_date(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    minute_of_day = local.tm_hour * 60 + local.tm_min;
    return true;
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
    if (!std::isfinite(config.max_hold_hours) || config.max_hold_hours <= 0 ||
        std::floor(config.max_hold_hours * 4) != config.max_hold_hours * 4) {
        result.error = "max hold must be a positive multiple of 15 minutes"; return result;
    }
    for (const auto &b : config.buckets) {
        if (b.start_minute < 0 || b.start_minute >= 1440 || b.length_minutes <= 0 ||
            b.length_minutes % 15 != 0 || b.start_minute + b.length_minutes > 1440) {
            result.error = "bucket must start and end in-day on 15-minute steps";
            return result;
        }
    }

    // Build candles: every expected 15-minute slot must arrive exactly once, on the grid.
    struct Key {
        std::string date;
        int start_minute;
        bool operator<(const Key &o) const {
            return date != o.date ? date < o.date : start_minute < o.start_minute;
        }
    };
    struct Builder {
        CrtClockCandle candle;
        std::set<int> slots;          // minute offsets already filled
        std::map<int, const Bar *> by_slot;
        bool started{};
    };
    std::map<Key, Builder> builders;
    std::set<std::string> seen_timestamps;

    for (const auto &bar : bars) {
        if (!seen_timestamps.insert(bar.date).second) {
            ++result.duplicate_bars;
            continue;
        }
        std::string date;
        int minute_of_day = 0;
        if (!et_local_from_utc(bar.date, date, minute_of_day)) {
            result.error = "unparseable timestamp " + bar.date;
            return result;
        }
        if (minute_of_day % 15 != 0) {
            ++result.off_grid_bars;
            continue;
        }
        for (const auto &b : config.buckets) {
            if (minute_of_day < b.start_minute || minute_of_day >= b.start_minute + b.length_minutes)
                continue;
            Builder &entry = builders[Key{date, b.start_minute}];
            if (!entry.started) {
                entry.started = true;
                entry.candle.session_date = date;
                entry.candle.start_minute = b.start_minute;
                entry.candle.start_utc = bar.date;
                entry.candle.bars_expected = b.length_minutes / 15;
                entry.candle.open = bar.open;
                entry.candle.high = bar.high;
                entry.candle.low = bar.low;
                entry.candle.close = bar.close;
            } else {
                entry.candle.high = std::max(entry.candle.high, bar.high);
                entry.candle.low = std::min(entry.candle.low, bar.low);
                entry.candle.close = bar.close;
            }
            entry.candle.end_utc = bar.date;
            ++entry.candle.bars_seen;
            entry.slots.insert(minute_of_day - b.start_minute);
            entry.by_slot[minute_of_day - b.start_minute] = &bar;
            break;
        }
    }

    for (auto &[key, entry] : builders) {
        auto &c = entry.candle;
        c.complete = static_cast<int>(entry.slots.size()) == c.bars_expected &&
                     c.bars_seen == c.bars_expected;
        if (!c.complete) {
            std::ostringstream why;
            why << "slots " << entry.slots.size() << "/" << c.bars_expected << ", bars "
                << c.bars_seen;
            if (c.bars_seen > static_cast<int>(entry.slots.size()))
                why << " (duplicate slot)";
            c.incomplete_reason = why.str();
            ++result.incomplete_candles;
        }
        result.candles.push_back(c);
    }
    std::sort(result.candles.begin(), result.candles.end(),
              [](const CrtClockCandle &a, const CrtClockCandle &b) { return a.start_utc < b.start_utc; });

    std::vector<CrtClockCandle> usable;
    for (const auto &c : result.candles)
        if (c.complete)
            usable.push_back(c);
    if (usable.size() < 2) {
        result.ok = true; // valid run, nothing to test
        return result;
    }

    result.start_notional = config.quantity * config.multiplier * usable.front().open;
    double equity = result.start_notional;
    const double slip = config.slippage_bps / 10000.0;

    std::map<std::string, std::size_t> bar_index;
    for (std::size_t i = 0; i < bars.size(); ++i)
        bar_index.emplace(bars[i].date, i);

    std::size_t position_open_until = 0;
    for (std::size_t i = 0; i + 1 < usable.size(); ++i) {
        const auto &range = usable[i];
        const auto &sweep = usable[i + 1];

        // Consecutive buckets only: same session next bucket, or the first bucket of a later
        // session when the range is the last bucket of its session.
        std::size_t range_bucket = 0;
        while (range_bucket < config.buckets.size() &&
               config.buckets[range_bucket].start_minute != range.start_minute)
            ++range_bucket;
        if (range_bucket >= config.buckets.size())
            continue;
        const bool last_bucket = range_bucket + 1 == config.buckets.size();
        const bool same_day_next =
            !last_bucket && range.session_date == sweep.session_date &&
            sweep.start_minute == config.buckets[range_bucket + 1].start_minute;
        // Without an exchange calendar, cross-session adjacency cannot be verified.
        // Refuse such pairs rather than skipping missing sessions.
        if (!same_day_next)
            continue;

        ++result.pairs_examined;
        const bool swept_low = sweep.low < range.low;
        const bool swept_high = sweep.high > range.high;
        if (swept_low && swept_high) {
            ++result.skipped_double_sided;
            continue;
        }
        // The close must be strictly inside BOTH boundaries; equality is not inside.
        int direction = 0;
        if (swept_low && sweep.close > range.low && sweep.close < range.high)
            direction = 1;
        else if (swept_high && sweep.close < range.high && sweep.close > range.low) {
            if (config.allow_shorts)
                direction = -1;
        }
        if (direction == 0) {
            if ((swept_low || swept_high) && !config.allow_shorts)
                ++result.skipped_close_outside;
            continue;
        }

        const double stop = direction == 1 ? sweep.low : sweep.high;
        const double target = direction == 1 ? range.high : range.low;
        if (!config.cutoff_date.empty() && sweep.session_date >= config.cutoff_date) {
            ++result.skipped_cutoff;
            continue;
        }

        // The entry bar must be the exact expected next 15-minute slot after the sweep candle.
        std::string expected_entry;
        {
            Stamp s;
            parse_stamp(sweep.end_utc, s);
            const std::int64_t days = days_from_civil(s.year, unsigned(s.month), unsigned(s.day));
            const std::time_t epoch =
                std::time_t(days * 86400 + s.hour * 3600 + s.minute * 60 + 15 * 60);
            std::tm g{};
            gmtime_r(&epoch, &g);
            char buf[32];
            std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:00Z", g.tm_year + 1900,
                          g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min);
            expected_entry = buf;
        }
        auto found = bar_index.find(expected_entry);
        if (found == bar_index.end()) {
            ++result.skipped_missing_entry_bar;
            continue;
        }
        const std::size_t entry_index = found->second;
        const auto signal_end = bar_index.find(sweep.end_utc);
        if (signal_end == bar_index.end() || signal_end->second < position_open_until ||
            entry_index < position_open_until) {
            ++result.skipped_open_position;
            continue;
        }
        const Bar &entry_bar = bars[entry_index];
        std::string entry_date;
        int entry_minute = 0;
        et_local_from_utc(entry_bar.date, entry_date, entry_minute);
        if (!config.cutoff_date.empty() && entry_date >= config.cutoff_date) {
            ++result.skipped_cutoff;
            continue;
        }
        if ((direction == 1 && entry_bar.open >= target) ||
            (direction == -1 && entry_bar.open <= target) ||
            (direction == 1 && entry_bar.open <= stop) ||
            (direction == -1 && entry_bar.open >= stop)) {
            ++result.skipped_gap_beyond_target;
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
        trade.entry_reference = entry_bar.open;
        trade.entry = round_adverse(entry_bar.open * (1.0 + direction * slip), config.tick_size,
                                    direction, true);

        const std::size_t max_bars =
            static_cast<std::size_t>(std::max(1.0, config.max_hold_hours * 4));
        std::size_t exit_index = entry_index;
        double exit_reference = 0;
        std::string reason;
        for (std::size_t j = entry_index; j < bars.size(); ++j) {
            const Bar &bar = bars[j];
            std::string bar_date;
            int bar_minute = 0;
            et_local_from_utc(bar.date, bar_date, bar_minute);

            if (!config.cutoff_date.empty() && bar_date >= config.cutoff_date) {
                result.error = "open position reached cutoff: no verified scheduled exit; run stopped";
                return result;
            }
            // Holding duration is a count of observed 15-minute bars, not elapsed time.
            // A gap while holding makes stop/target ordering unknowable; fail closed.
            if (j > entry_index) {
                Stamp prior, current;
                if (!parse_stamp(bars[j - 1].date, prior) || !parse_stamp(bar.date, current)) {
                    result.error = "invalid timestamp while holding"; return result;
                }
                const auto seconds = [](const Stamp &s) {
                    return days_from_civil(s.year, unsigned(s.month), unsigned(s.day)) * 86400 +
                           s.hour * 3600 + s.minute * 60;
                };
                if (seconds(current) - seconds(prior) != 900) {
                    result.error = "missing 15-minute bar while holding; exit is unknown";
                    return result;
                }
            }
            const bool stop_hit = direction == 1 ? bar.low <= trade.stop : bar.high >= trade.stop;
            const bool target_hit =
                direction == 1 ? bar.high >= trade.target : bar.low <= trade.target;
            const bool gap_beyond_stop =
                (direction == 1 && bar.open < trade.stop) ||
                (direction == -1 && bar.open > trade.stop);

            if (stop_hit && target_hit) {
                trade.ambiguous_bar = true;
                trade.gap_through_stop = gap_beyond_stop;
                exit_reference = gap_beyond_stop ? bar.open : trade.stop;
                exit_index = j;
                reason = "stop";
                break;
            }
            if (stop_hit) {
                trade.gap_through_stop = gap_beyond_stop;
                exit_reference = gap_beyond_stop ? bar.open : trade.stop;
                exit_index = j;
                reason = "stop";
                break;
            }
            if (target_hit) {
                exit_reference = trade.target;
                exit_index = j;
                reason = "target";
                break;
            }
            if (j - entry_index + 1 >= max_bars) {
                exit_reference = bar.close;
                exit_index = j;
                reason = "time";
                break;
            }
            if (j + 1 == bars.size()) {
                result.error = "data ended with an open position; exit is unknown";
                return result;
            }
        }
        if (reason.empty()) {
            exit_reference = bars.back().close;
            exit_index = bars.size() - 1;
            reason = "time";
        }

        trade.exit_reference = exit_reference;
        trade.exit = round_adverse(exit_reference * (1.0 - direction * slip), config.tick_size,
                                   direction, false);
        trade.exit_time = bars[exit_index].date;
        trade.exit_reason = reason;
        trade.bars_held = static_cast<int>(exit_index - entry_index) + 1;

        const double units = config.quantity * config.multiplier;
        trade.gross_pre_slippage = direction * (exit_reference - entry_bar.open) * units;
        const double gross_as_filled = direction * (trade.exit - trade.entry) * units;
        trade.slippage = trade.gross_pre_slippage - gross_as_filled;
        trade.fees = 2.0 * config.fee;
        trade.net = trade.gross_pre_slippage - trade.slippage - trade.fees;
        equity += trade.net;
        result.trades.push_back(trade);
        position_open_until = exit_index + 1;
    }

    result.end_equity = equity;
    double peak = result.start_notional, running = result.start_notional;
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
    trades << "range_start,sweep_start,entry_time,exit_time,direction,entry_reference,entry,stop,"
              "target,exit_reference,exit,exit_reason,gap_through_stop,ambiguous_bar,bars_held,"
              "gross_pre_slippage,slippage,fees,net\n"
           << std::setprecision(10);
    for (const auto &t : result.trades)
        trades << t.range_start << ',' << t.sweep_start << ',' << t.entry_time << ','
               << t.exit_time << ',' << (t.direction == 1 ? "long" : "short") << ','
               << t.entry_reference << ',' << t.entry << ',' << t.stop << ',' << t.target << ','
               << t.exit_reference << ',' << t.exit << ',' << t.exit_reason << ','
               << (t.gap_through_stop ? 1 : 0) << ',' << (t.ambiguous_bar ? 1 : 0) << ','
               << t.bars_held << ',' << t.gross_pre_slippage << ',' << t.slippage << ','
               << t.fees << ',' << t.net << '\n';

    std::ofstream candles(directory + "/candles.csv");
    candles << "session_date,start_utc,end_utc,start_time_et,bars_seen,bars_expected,complete,"
               "reason,open,high,low,close\n"
            << std::setprecision(10);
    for (const auto &c : result.candles)
        candles << c.session_date << ',' << c.start_utc << ',' << c.end_utc << ','
                << minutes_to_hm(c.start_minute) << ',' << c.bars_seen << ','
                << c.bars_expected << ',' << (c.complete ? 1 : 0) << ',' << c.incomplete_reason
                << ',' << c.open << ',' << c.high << ',' << c.low << ',' << c.close << '\n';

    // Realised-only curve: one point per closed trade, no mark-to-market.
    std::ofstream curve(directory + "/equity.csv");
    curve << "trade,timestamp,realised_equity\n" << std::setprecision(10);
    double running = result.start_notional;
    curve << "0,start," << running << '\n';
    for (std::size_t i = 0; i < result.trades.size(); ++i) {
        running += result.trades[i].net;
        curve << (i + 1) << ',' << result.trades[i].exit_time << ',' << running << '\n';
    }

    int wins = 0, gaps = 0, ambiguous = 0;
    double pre = 0, slip = 0, fees = 0, net = 0;
    for (const auto &t : result.trades) {
        if (t.net > 0)
            ++wins;
        gaps += t.gap_through_stop ? 1 : 0;
        ambiguous += t.ambiguous_bar ? 1 : 0;
        pre += t.gross_pre_slippage;
        slip += t.slippage;
        fees += t.fees;
        net += t.net;
    }
    std::ostringstream md;
    md << std::fixed << std::setprecision(2)
       << "# Clock-aligned CRT (corrected) - buckets from "
       << minutes_to_hm(config.buckets.empty() ? 0 : config.buckets.front().start_minute) << " ET\n\n"
       << "- Source: " << source << "\n"
       << "- Multiplier " << config.multiplier << ", tick " << config.tick_size << ", quantity "
       << config.quantity << "\n"
       << "- Costs (assumption): " << config.fee << " per side, " << config.slippage_bps
       << " bps slippage\n"
       << "- Cutoff: " << (config.cutoff_date.empty() ? "none" : config.cutoff_date + " ET") << "\n\n"
       << "| Metric | Value |\n| --- | ---: |\n"
       << "| Candles built | " << result.candles.size() << " |\n"
       << "| Incomplete candles | " << result.incomplete_candles << " |\n"
       << "| Duplicate timestamps | " << result.duplicate_bars << " |\n"
       << "| Off-grid timestamps | " << result.off_grid_bars << " |\n"
       << "| Candle pairs examined | " << result.pairs_examined << " |\n"
       << "| Setups taken | " << result.setups << " |\n"
       << "| Skipped: double-sided sweep | " << result.skipped_double_sided << " |\n"
       << "| Skipped: close outside the range | " << result.skipped_close_outside << " |\n"
       << "| Skipped: missing entry bar | " << result.skipped_missing_entry_bar << " |\n"
       << "| Skipped: position already open | " << result.skipped_open_position << " |\n"
       << "| Skipped: gap beyond target/stop | " << result.skipped_gap_beyond_target << " |\n"
       << "| Skipped: cutoff | " << result.skipped_cutoff << " |\n"
       << "| Trades | " << result.trades.size() << " |\n"
       << "| Wins | " << wins << " |\n"
       << "| Gross P&L before slippage | " << pre << " |\n"
       << "| Slippage | " << -slip << " |\n"
       << "| Fees | " << -fees << " |\n"
       << "| Net P&L | " << net << " |\n"
       << "| Start notional (1 contract x price x multiplier) | " << result.start_notional << " |\n"
       << "| Max drawdown, realised trades only | " << 100.0 * result.max_drawdown << "% |\n"
       << "| Gap-through-stop trades | " << gaps << " |\n"
       << "| Ambiguous (stop+target same bar) | " << ambiguous << " |\n\n"
       << "Equity here is **realised trade P&L on a fixed notional**, not an account balance: no "
          "margin, no mark-to-market and no cash interest are modelled.\n";
    std::ofstream(directory + "/report.md") << md.str();
}
} // namespace algo
