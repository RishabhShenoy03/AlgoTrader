#pragma once
#include "Broker.hpp"
#include "Instrument.hpp"
#include <memory>
namespace algo {
struct IbkrConfig {
    std::string host{"127.0.0.1"};
    int port{};
    int client_id{17};
    std::string paper_account;
    bool read_only{true};
    std::string currency{"USD"};
    int timeout_seconds{15};
    int stale_seconds{30};
};
// Built with official SDK when ALGO_WITH_IBKR=ON. No SDK types escape this header.
class IbkrBroker : public Broker {
  public:
    explicit IbkrBroker(IbkrConfig config);
    ~IbkrBroker() override;
    void add_instrument(const Instrument &instrument);
    void subscribe(std::uint64_t ticker);
    void unsubscribe(std::uint64_t ticker);
    std::vector<Bar> historical_bars(std::uint64_t ticker, const std::string &duration,
                                     const std::string &bar_size, const std::string &end_time = "");
    void connect() override;
    void disconnect() override;
    bool ready() const override;
    void submit(const Order &) override;
    void cancel(OrderId) override;
    void request_snapshot() override;
    void poll() override;
    void set_callbacks(BrokerCallbacks) override;
    static bool available() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace algo
