#pragma once
#include "Bar.hpp"
#include "Fill.hpp"
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct BrokerSnapshot {
    std::string account;
    std::string currency;
    double cash{};
    std::unordered_map<std::uint64_t, double> positions;
    std::vector<OrderState> open_orders;
    std::vector<Fill> executions;
    bool complete{}; // True only after all snapshot end markers arrive.
};
struct BrokerCallbacks {
    std::function<void(const OrderUpdate&)> on_order;
    std::function<void(const Fill&)> on_fill;
    std::function<void(const Commission&)> on_commission;
    std::function<void(const BrokerSnapshot&)> on_snapshot;
    std::function<void(const Bar&)> on_bar;
    std::function<void(const std::string&)> on_error;
};

// Declaration-only scaffold; implementations are intentionally deferred.
class Broker {
public:
    virtual ~Broker() = default;
    // Begin connection; readiness requires completed synchronization.
    virtual void connect() = 0;
    // Stop callbacks and join owned threads before destruction.
    virtual void disconnect() = 0;
    // False until account, open orders and executions have reconciled.
    virtual bool ready() const = 0;
    // Send an order; success here does not mean a fill.
    virtual void submit(const Order& order) = 0;
    // Request cancellation; await broker acknowledgement.
    virtual void cancel(OrderId id) = 0;
    // Request cash, positions, open orders and recent executions.
    virtual void request_snapshot() = 0;
    // Dispatch queued callbacks on the application thread.
    virtual void poll() = 0;
    // Install callbacks before connecting; callbacks must not outlive owners.
    virtual void set_callbacks(BrokerCallbacks callbacks) = 0;
};
