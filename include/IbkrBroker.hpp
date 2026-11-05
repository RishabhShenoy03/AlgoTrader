#pragma once
#include "Broker.hpp"
#include <memory>

struct IbkrConfig {
    std::string host;
    int port{}; // Explicit value from the user's paper TWS/Gateway settings.
    int client_id{};
    std::string paper_account;
    bool read_only{true};
};

// Declaration-only scaffold; implementations are intentionally deferred.
class IbkrBroker : public Broker {
public:
    // Store explicit paper connection/account configuration; no SDK dependency in public header.
    explicit IbkrBroker(IbkrConfig config);
    // Disconnect and join reader thread before releasing SDK objects.
    ~IbkrBroker() override;
    // Request contract details and reject ambiguous matches; initial scope is one stock/ETF.
    void resolve_instrument(std::uint64_t ticker, std::string symbol, std::string exchange, std::string currency);
    // Map request ID to resolved contract; retain timestamps and market-data type; detect stale data.
    void subscribe(std::uint64_t ticker);
    // Cancel subscription and retire request mapping safely.
    void unsubscribe(std::uint64_t ticker);
    // Begin connection; readiness requires completed synchronization. Map SDK callbacks through the serialized event queue.
    void connect() override;
    // Stop callbacks and join owned threads before destruction. Map SDK callbacks through the serialized event queue.
    void disconnect() override;
    // False until account, open orders and executions have reconciled. Map SDK callbacks through the serialized event queue.
    bool ready() const override;
    // Send an order; success here does not mean a fill. Map SDK callbacks through the serialized event queue.
    void submit(const Order& order) override;
    // Request cancellation; await broker acknowledgement. Map SDK callbacks through the serialized event queue.
    void cancel(OrderId id) override;
    // Request cash, positions, open orders and recent executions. Map SDK callbacks through the serialized event queue.
    void request_snapshot() override;
    // Dispatch queued callbacks on the application thread. Map SDK callbacks through the serialized event queue.
    void poll() override;
    // Install callbacks before connecting; callbacks must not outlive owners. Map SDK callbacks through the serialized event queue.
    void set_callbacks(BrokerCallbacks callbacks) override;

private:
    struct Impl; // Own SDK client, wrapper, reader and callback queue here.
    std::unique_ptr<Impl> impl_;
};
