#pragma once
#include "Bar.hpp"
#include "Order.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace algo {

// Read-only daily signal research. Nothing in this header submits, modifies or
// cancels broker orders: it turns completed daily bars plus a broker snapshot
// into a report a human can inspect.

enum class SignalState { AboveAverage, BelowAverage, EqualToAverage };

struct SeriesReport {
    bool ok{};
    std::string error;
    std::size_t bars{};
    std::uint64_t ticker{};
    std::string first_date, last_date;
};

// Daily-only validation: ISO YYYY-MM-DD dates, one ticker, strictly increasing
// (so duplicates and unsorted rows fail), finite positive prices.
SeriesReport validate_daily_series(const std::vector<Bar> &bars);

// Calendar helpers. Days are counted from the Unix epoch in UTC.
bool parse_daily_date(const std::string &date, std::int64_t &days_since_epoch);
std::string utc_date(std::int64_t unix_seconds);
std::string iso_date(int year, unsigned month, unsigned day);
std::string utc_timestamp(std::int64_t unix_seconds);
std::int64_t days_between(const std::string &from, const std::string &to);

// A daily bar dated D is a completed session once D is in the past, or once the
// US regular session has certainly closed on D (22:00 UTC). A bar dated today
// before that hour may still be forming and is refused.
bool session_completed(const std::string &date, std::int64_t now_unix);

struct SignalRequest {
    std::size_t window{100};
    double quantity{1};
    std::string as_of;     // decision date; empty selects the latest completed bar
    int max_stale_days{5};
    std::int64_t now_unix{}; // injected clock; tests pass a fixed value
};

struct SignalEvaluation {
    bool ok{};
    std::string error;
    std::string decision_date;
    double decision_close{};
    double prior_average{};
    std::size_t window{};
    std::size_t prior_bars_available{};
    std::string window_first, window_last;
    SignalState state{SignalState::EqualToAverage};
    std::optional<double> target_shares; // nullopt when the close equals the average
    double quantity{1};
    bool freshness_waived{}; // true when --as-of was used to replay a past session
};

// Computes the prior-window average with MovingAverageStrategy itself, so the
// report cannot drift from the backtester's definition.
SignalEvaluation evaluate_signal(const std::vector<Bar> &bars, const SignalRequest &request);

struct BrokerView {
    bool confirmed{};
    std::string error;
    std::string account;
    std::string currency;
    double cash{};
    double shares{};
    std::vector<OrderState> open_orders;
};

enum class ProposalStatus {
    Actionable,
    NoActionRequired,
    BlockedNoSignal,
    BlockedUnknownBroker,
    BlockedOpenOrders,
    BlockedUnexpectedPosition
};

struct Proposal {
    ProposalStatus status{ProposalStatus::BlockedNoSignal};
    std::optional<Side> side;
    double quantity{};
    double resulting_position{};
    std::string reason;
};

Proposal propose_action(const SignalEvaluation &signal, const BrokerView &broker);
// Reject changes between the decision and the final broker snapshot.
bool snapshot_matches_decision(double expected_shares, double expected_cash,
                               double actual_shares, double actual_cash);

// A bounded limit order derived from the completed close, rounded adversely to the
// tick. The backtester fills at the next open, so this is the closest executable
// approximation of that assumption: it never chases a gap.
struct ExecutionPlan {
    bool ok{};
    std::string error;
    Side side{Side::Buy};
    double quantity{};
    double reference_close{};
    double limit_price{};
    int band_bps{};
};

ExecutionPlan plan_execution(const SignalEvaluation &signal, const Proposal &proposal,
                             double tick_size, int band_bps);

std::string signal_state_name(SignalState state);
std::string proposal_status_name(ProposalStatus status);
std::string side_name(Side side);

struct ReportContext {
    std::string symbol;
    std::string currency;
    std::string source;
    std::string command;
    std::int64_t now_unix{};
};

struct ReportFiles {
    std::string directory, markdown, json, bars_csv, command;
};

// Writes signal.md, signal.json, bars-used.csv and command.txt into a new
// directory under output_root. Refuses to overwrite an existing directory.
ReportFiles write_signal_report(const std::string &output_root, const SignalEvaluation &signal,
                                const BrokerView &broker, const Proposal &proposal,
                                const ReportContext &context, const std::vector<Bar> &bars_used);
} // namespace algo
