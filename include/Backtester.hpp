#pragma once
#include "CsvDataFeed.hpp"
#include "BrokerSim.hpp"
#include "Strategy.hpp"
#include "OrderManager.hpp"
#include "FillProcessor.hpp"
#include "RiskManager.hpp"
#include "PerformanceReport.hpp"

// Declaration-only scaffold; implementations are intentionally deferred.
class Backtester {
public:
    // Wire callbacks with explicit lifetime ownership; start with fresh run state.
    Backtester(CsvDataFeed& feed, BrokerSim& broker, Strategy& strategy, Portfolio& portfolio, OrderManager& orders, FillProcessor& fills, RiskManager& risk, PerformanceReport& report);
    // For each bar: fill previous intents at open, process events, mark close equity, evaluate strategy, risk-check and queue next-open orders. Report unfilled final orders; no synthetic terminal fill.
    void run();

private:
    CsvDataFeed& feed_;
    BrokerSim& broker_;
    Strategy& strategy_;
    Portfolio& portfolio_;
    OrderManager& orders_;
    FillProcessor& fills_;
    RiskManager& risk_;
    PerformanceReport& report_;
};
