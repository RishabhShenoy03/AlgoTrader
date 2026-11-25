#pragma once
#include <cstdint>
#include <string>

namespace algo {

// US equity calendar for scheduling only: weekends plus NYSE full-closure holidays.
// Early-close sessions are treated as normal days; there is no half-day convention.
// Holiday rules are generated per year, so no table needs updating annually, but the
// observed-date convention must be re-checked against NYSE notices when rules change.
bool is_us_equity_trading_day(const std::string &date);

// Seconds from now_unix until 22:00 UTC on the current UTC date (the session cutoff
// used by the daily signal report). Zero or negative when the cutoff has passed.
std::int64_t seconds_until_session_cutoff(std::int64_t now_unix);
} // namespace algo
