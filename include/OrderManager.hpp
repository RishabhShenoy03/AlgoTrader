#pragma once
#include "Broker.hpp"
#include <unordered_map>
#include <unordered_set>

namespace algo {
class OrderManager {
  public:
    explicit OrderManager(Broker &broker);
    OrderId submit(Order intent, double reserved_cash = 0, double reserved_shares = 0);
    void cancel(OrderId id);
    void on_update(const OrderUpdate &update);
    void on_fill(const Fill &fill);
    std::vector<OrderState> pending() const;
    std::vector<OrderState> all() const;
    void reconcile(const BrokerSnapshot &snapshot);

  private:
    Broker &broker_;
    OrderId next_id_{1};
    std::unordered_map<OrderId, OrderState> orders_;
    std::unordered_set<std::string> executions_;
    std::unordered_set<std::string> keys_;
    std::unordered_map<OrderId, double> reported_fills_;
};
} // namespace algo
