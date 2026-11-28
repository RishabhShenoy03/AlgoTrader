#pragma once
#include "Broker.hpp"
#include "Instrument.hpp"
#include <memory>
#include <optional>
namespace algo {
// Watchlist quote only: never treated as a completed bar or executable live price.
struct IbkrQuote {
    std::uint64_t ticker{};
    int market_data_type{}; // 0 unknown, 1 live, 2 frozen, 3 delayed, 4 delayed-frozen.
    std::optional<double> bid, ask, last;
};
// Result of a read-only contract lookup. Only what a research run needs to identify a
// tradable contract and its economics; nothing here is used to place an order.
struct ContractInfo {
    long con_id{};
    std::string symbol, sec_type, exchange, currency, expiry, multiplier, trading_class;
    double min_tick{};
    std::string long_name, time_zone, trading_hours;
};

struct IbkrConfig {
    std::string host{"127.0.0.1"};
    int port{};
    int client_id{17};
    std::string paper_account;
    bool read_only{true};
    std::string currency{"USD"};
    int timeout_seconds{15};
    int stale_seconds{30};
    // Data-only sessions (historical pulls) must not fail because the account holds an
    // instrument that is not in this run's catalogue.
    bool data_only{false};
};
// Built with official SDK when ALGO_WITH_IBKR=ON. No SDK types escape this header.
class IbkrBroker : public Broker {
  public:
    explicit IbkrBroker(IbkrConfig config);
    ~IbkrBroker() override;
    void add_instrument(const Instrument &instrument);
    void subscribe(std::uint64_t ticker);
    void unsubscribe(std::uint64_t ticker);
    // Read-only diagnostic. Waits for the configured timeout and cancels its subscription.
    IbkrQuote quote(std::uint64_t ticker, int market_data_type = 3);
    // Read-only symbol -> contract resolution (no con_id needed). Returns every match;
    // the caller decides which expiry it wants.
    std::vector<ContractInfo> resolve_contracts(const std::string &symbol, const std::string &sec_type,
                                                const std::string &exchange,
                                                const std::string &expiry = "",
                                                const std::string &currency = "USD");
    std::vector<Bar> historical_bars(std::uint64_t ticker, const std::string &duration,
                                     const std::string &bar_size, const std::string &end_time = "",
                                     bool regular_hours = true);
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
