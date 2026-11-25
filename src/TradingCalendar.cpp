#include "TradingCalendar.hpp"
#include "DailySignal.hpp"
#include <stdexcept>

namespace algo {
namespace {

enum class Weekday { Sunday, Monday, Tuesday, Wednesday, Thursday, Friday, Saturday };

// 1970-01-01 was a Thursday, which is index 4 with Sunday = 0.
Weekday weekday_of(std::int64_t days) {
    return static_cast<Weekday>(((days % 7) + 7 + 4) % 7);
}

std::int64_t day_number(int year, unsigned month, unsigned day) {
    std::int64_t days = 0;
    if (!parse_daily_date(iso_date(year, month, day), days))
        throw std::runtime_error("calendar: could not build a date");
    return days;
}

std::int64_t nth_weekday_of_month(int year, unsigned month, Weekday weekday, int n) {
    const std::int64_t first = day_number(year, month, 1);
    const int delta = (static_cast<int>(weekday) - static_cast<int>(weekday_of(first)) + 7) % 7;
    return first + delta + 7 * (n - 1);
}

std::int64_t last_weekday_of_month(int year, unsigned month, unsigned month_days, Weekday weekday) {
    const std::int64_t last = day_number(year, month, month_days);
    const int delta = (static_cast<int>(weekday_of(last)) - static_cast<int>(weekday) + 7) % 7;
    return last - delta;
}

// Anonymous Gregorian algorithm for Easter Sunday.
std::int64_t easter_sunday(int year) {
    const int a = year % 19, b = year / 100, c = year % 100, d = b / 4, e = b % 4;
    const int f = (b + 8) / 25, g = (b - f + 1) / 3, h = (19 * a + b - d - g + 15) % 30;
    const int i = c / 4, k = c % 4, l = (32 + 2 * e + 2 * i - h - k) % 7;
    const int m = (a + 11 * h + 22 * l) / 451;
    const int month = (h + l - 7 * m + 114) / 31;
    const int day = ((h + l - 7 * m + 114) % 31) + 1;
    return day_number(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
}

// NYSE observance: a Saturday holiday moves to the preceding Friday, a Sunday
// holiday to the following Monday. New Year's Day is the documented exception
// (a Saturday Jan 1 is not observed in the previous December); when Jan 1 falls on
// a Saturday this rule would name Dec 31 of the prior year, which is outside the
// year being tested, so no false closure is produced for the tested year.
std::int64_t observed(std::int64_t days) {
    const auto wd = weekday_of(days);
    if (wd == Weekday::Saturday)
        return days - 1;
    if (wd == Weekday::Sunday)
        return days + 1;
    return days;
}

bool is_holiday(std::int64_t days, int year) {
    return days == observed(day_number(year, 1, 1)) ||                       // New Year's Day
           days == nth_weekday_of_month(year, 1, Weekday::Monday, 3) ||      // MLK Day
           days == nth_weekday_of_month(year, 2, Weekday::Monday, 3) ||      // Washington's Birthday
           days == easter_sunday(year) - 2 ||                                // Good Friday
           days == last_weekday_of_month(year, 5, 31, Weekday::Monday) ||    // Memorial Day
           days == observed(day_number(year, 6, 19)) ||                      // Juneteenth
           days == observed(day_number(year, 7, 4)) ||                       // Independence Day
           days == nth_weekday_of_month(year, 9, Weekday::Monday, 1) ||      // Labor Day
           days == nth_weekday_of_month(year, 11, Weekday::Thursday, 4) ||   // Thanksgiving
           days == observed(day_number(year, 12, 25));                       // Christmas Day
}
} // namespace

bool is_us_equity_trading_day(const std::string &date) {
    std::int64_t days = 0;
    if (!parse_daily_date(date, days))
        throw std::invalid_argument("trading calendar requires an ISO YYYY-MM-DD date");
    const auto wd = weekday_of(days);
    if (wd == Weekday::Saturday || wd == Weekday::Sunday)
        return false;
    return !is_holiday(days, std::stoi(date.substr(0, 4)));
}

std::int64_t seconds_until_session_cutoff(std::int64_t now_unix) {
    constexpr std::int64_t kCutoffHour = 22; // 16:00 ET plus a settling buffer
    const std::int64_t day = now_unix / 86400;
    return (day * 86400 + kCutoffHour * 3600) - now_unix;
}
} // namespace algo
