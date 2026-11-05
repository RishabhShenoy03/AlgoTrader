#pragma once
#include "Bar.hpp"
#include <fstream>
#include <optional>
#include <string>

// Declaration-only scaffold; implementations are intentionally deferred.
class CsvDataFeed {
public:
    // Store path; document date,ticker,open,close schema and delimiter.
    explicit CsvDataFeed(std::string path);
    // Open file; validate header; fail with actionable path/line diagnostics.
    void open();
    // Parse finite positive prices and ordered dates; nullopt only for clean EOF, throw on malformed input.
    std::optional<Bar> next();
    // Reopen and reset validation state for deterministic replay.
    void reset();

private:
    std::string path_;
    std::ifstream input_;
    std::size_t line_number_{};
};
