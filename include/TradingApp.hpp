#pragma once
#include "Broker.hpp"
#include "Strategy.hpp"
#include "OrderManager.hpp"
#include "FillProcessor.hpp"
#include "RiskManager.hpp"
#include "EventJournal.hpp"

// Declaration-only scaffold; implementations are intentionally deferred.
class TradingApp {
public:
    // Wire callbacks onto one event loop; dependencies must outlive app.
    TradingApp(Broker& broker, Strategy& strategy, Portfolio& portfolio, OrderManager& orders, FillProcessor& fills, RiskManager& risk, EventJournal& journal);
    // Recover journal, connect read-only, reconcile account/positions/orders/executions before enabling decisions.
    void start();
    // Poll broker, detect stale data/disconnects, and serialize all state mutations; initial mode is paper only.
    void run();
    // After synchronization and freshness gates, evaluate strategy, risk-check and journal approved intents before submission.
    void on_bar(const Bar& bar);
    // Compare broker truth with recovered state; require completed snapshot and resolve discrepancies before resuming.
    void reconcile(const BrokerSnapshot& snapshot);
    // Halt new intents; request cancellations per policy, drain acknowledgements, persist unresolved orders, disconnect and flush.
    void stop();

private:
    Broker& broker_;
    Strategy& strategy_;
    Portfolio& portfolio_;
    OrderManager& orders_;
    FillProcessor& fills_;
    RiskManager& risk_;
    EventJournal& journal_;
    bool running_{};
    bool reconciled_{};
};
