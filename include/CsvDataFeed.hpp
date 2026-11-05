#pragma once
#include "Bar.hpp"
#include <fstream>
#include <optional>
#include <string>
#include <unordered_set>

namespace algo {
class CsvDataFeed {
  public:
    explicit CsvDataFeed(std::string path);
    void open();
    std::optional<Bar> next();
    void reset();
    void set_range(std::string from, std::string to);

  private:
    std::optional<Bar> read_next();
    std::string from_, to_;
    std::string path_;
    std::ifstream input_;
    std::size_t line_number_{};
    std::string last_date_;
    std::unordered_set<std::uint64_t> seen_;
};
} // namespace algo
