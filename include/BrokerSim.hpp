#pragma once
#include "Broker.hpp"
#include "Portfolio.hpp"
#include <unordered_set>

namespace algo {
class BrokerSim : public Broker {
  public:
    BrokerSim(double fee_per_fill, double slippage_bps, double max_fill_quantity = 0);
    void configure_account(const Portfolio &portfolio);
    void on_bar_open(const Bar &bar);
    void settle(const std::unordered_map<std::uint64_t, double> &prices);
    void connect() override;
    void disconnect() override;
    bool ready() const override;
    void submit(const Order &order) override;
    void cancel(OrderId id) override;
    void request_snapshot() override;
    void poll() override;
    void set_callbacks(BrokerCallbacks callbacks) override;

  private:
    BrokerCallbacks callbacks_;
    std::vector<OrderState> pending_;
    std::vector<Fill> executions_;
    std::unordered_set<OrderId> ids_;
    Portfolio account_{1000};
    double fee_per_fill_;
    double slippage_bps_;
    double max_fill_quantity_;
    std::uint64_t next_execution_{1};
    bool connected_{};
};
} // namespace algo
