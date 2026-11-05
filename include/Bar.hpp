#pragma once
#include <cstdint>
#include <string>

// Initial replay format: ISO-8601 trading date, internal instrument ID, open/close.
// Intraday timestamps, volume and corporate actions are later extensions.
struct Bar {
    std::string date;
    std::uint64_t ticker;
    double open;
    double close;
};
