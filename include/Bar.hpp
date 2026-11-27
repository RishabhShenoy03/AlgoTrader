#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace algo {

// One candle. `date` is an ISO-8601 trading date (YYYY-MM-DD) or a UTC timestamp
// (YYYY-MM-DDTHH:MM:SSZ) for intraday feeds, so the same replay path handles daily and
// intraday data. High/low/volume are required for any rule that reasons about the
// candle's range (CRT, breakouts with stops) rather than only its close; a feed that
// supplies only open/close sets high = max(open, close) and low = min(open, close).
struct Bar {
    std::string date;
    std::uint64_t ticker;
    double open;
    double close;
    // Range and volume follow open/close so existing {date, ticker, open, close}
    // aggregate initialisers keep their meaning. A feed without them leaves 0, and
    // `with_range()` fills high/low from open/close.
    double high{};
    double low{};
    double volume{};
};

// Bars built from a close-only source carry no range; derive it so range-aware rules
// see a valid candle instead of zeroes.
inline Bar with_range(Bar b) {
    if (!(b.high > 0) || !(b.low > 0)) {
        b.high = std::max(b.open, b.close);
        b.low = std::min(b.open, b.close);
    }
    return b;
}
} // namespace algo
