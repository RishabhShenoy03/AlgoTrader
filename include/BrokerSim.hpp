#pragma once
#include "Broker.hpp"
#include <vector>

// Declaration-only scaffold; implementations are intentionally deferred.
class BrokerSim : public Broker {
public:
    // Validate nonnegative fees/slippage; initially support market orders only.
    BrokerSim(double fee_per_fill, double slippage_bps);
    // Fill only matching orders queued on earlier bars, at open plus adverse slippage; emit execution then commission; never fill new close signals here.
    void on_bar_open(const Bar& bar);
    // Begin connection; readiness requires completed synchronization. Implement simulator events.
    void connect() override;
    // Stop callbacks and join owned threads before destruction. Implement simulator events.
    void disconnect() override;
    // False until account, open orders and executions have reconciled. Implement simulator events.
    bool ready() const override;
    // Send an order; success here does not mean a fill. Implement simulator events.
    void submit(const Order& order) override;
    // Request cancellation; await broker acknowledgement. Implement simulator events.
    void cancel(OrderId id) override;
    // Request cash, positions, open orders and recent executions. Implement simulator events.
    void request_snapshot() override;
    // Dispatch queued callbacks on the application thread. Implement simulator events.
    void poll() override;
    // Install callbacks before connecting; callbacks must not outlive owners. Implement simulator events.
    void set_callbacks(BrokerCallbacks callbacks) override;

private:
    BrokerCallbacks callbacks_;
    std::vector<Order> pending_;
    double fee_per_fill_;
    double slippage_bps_;
    bool connected_{};
};
