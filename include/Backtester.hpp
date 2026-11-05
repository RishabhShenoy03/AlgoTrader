#pragma once
#include "BrokerSim.hpp"
#include "CsvDataFeed.hpp"
#include "FillProcessor.hpp"
#include "OrderManager.hpp"
#include "PerformanceReport.hpp"
#include "RiskManager.hpp"
#include "Strategy.hpp"
#include <functional>

namespace algo {
class Backtester {
  public:
    Backtester(CsvDataFeed &, BrokerSim &, Strategy &, Portfolio &, OrderManager &, FillProcessor &,
               RiskManager &, PerformanceReport &);
    ~Backtester();
    void run();
    const std::vector<std::string> &decisions() const noexcept;

  private:
    void batch(const std::vector<Bar> &bars);
    CsvDataFeed &feed_;
    BrokerSim &broker_;
    Strategy &strategy_;
    Portfolio &portfolio_;
    OrderManager &orders_;
    FillProcessor &fills_;
    RiskManager &risk_;
    PerformanceReport &report_;
    std::unordered_map<std::uint64_t, double> marks_;
    std::vector<std::string> decisions_;
    bool ran_{};
};
} // namespace algo
