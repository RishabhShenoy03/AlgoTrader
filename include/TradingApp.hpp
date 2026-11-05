#pragma once
#include "Broker.hpp"
#include "EventJournal.hpp"
#include "FillProcessor.hpp"
#include "OrderManager.hpp"
#include "RiskManager.hpp"
#include "Strategy.hpp"
#include <atomic>
#include <memory>

namespace algo {
class TradingApp {
  public:
    TradingApp(Broker &, Strategy &, Portfolio &, OrderManager &, FillProcessor &, RiskManager &,
               EventJournal &, double max_session_loss = 0.05);
    ~TradingApp();
    void start();
    void run();
    void on_bar(const Bar &bar);
    void reconcile(const BrokerSnapshot &snapshot);
    void stop();
    void request_stop() noexcept;

  private:
    Broker &broker_;
    Strategy &strategy_;
    Portfolio &portfolio_;
    OrderManager &orders_;
    FillProcessor &fills_;
    RiskManager &risk_;
    EventJournal &journal_;
    std::atomic<bool> running_{false};
    bool reconciled_{};
    std::string last_bar_;
    double initial_equity_{};
    double max_session_loss_;
};
} // namespace algo
