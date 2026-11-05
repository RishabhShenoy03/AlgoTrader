#pragma once
#include "FillProcessor.hpp"
#include <string>
#include <vector>

namespace algo {
struct EquityPoint {
    std::string timestamp;
    double equity{};
};
class PerformanceReport {
  public:
    void record(std::string timestamp, double equity);
    double total_return() const;
    double max_drawdown() const;
    void write_csv(const std::string &path) const;
    void write_trades(const std::string &path, const FillProcessor &fills) const;
    const std::vector<EquityPoint> &points() const noexcept;
    void reset();

  private:
    std::vector<EquityPoint> equity_;
};
} // namespace algo
