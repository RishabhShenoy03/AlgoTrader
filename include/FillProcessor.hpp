#pragma once
#include "Fill.hpp"
#include "Portfolio.hpp"
#include <unordered_map>
#include <vector>

namespace algo {
class FillProcessor {
  public:
    explicit FillProcessor(Portfolio &portfolio);
    void on_fill(const Fill &fill);
    void on_commission(const Commission &commission);
    const std::vector<Fill> &executions() const noexcept;
    double fees() const noexcept;

  private:
    Portfolio &portfolio_;
    std::unordered_map<std::string, Fill> applied_;
    std::unordered_map<std::string, Commission> commissions_;
    std::vector<Fill> fills_;
    double fees_{};
};
} // namespace algo
