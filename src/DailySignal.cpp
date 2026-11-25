#include "DailySignal.hpp"
#include "Instrument.hpp"
#include "MovingAverageStrategy.hpp"
#include "Portfolio.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace algo {
namespace {

// A US regular session is closed by 21:00 UTC at the latest (16:00 ET in winter);
// 22:00 UTC leaves an hour of slack for DST and end-of-day bar settling.
constexpr int kSessionClosedUtcHour = 22;
constexpr double kShareTolerance = 1e-9;

bool leap_year(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

unsigned month_length(int y, unsigned m) {
    static const unsigned lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && leap_year(y)) ? 29 : lengths[m - 1];
}

// Howard Hinnant's civil-calendar algorithms; no timezone database needed.
std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= static_cast<int>(m <= 2);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - static_cast<int>(era) * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::string format_number(double value, int precision = 10) {
    std::ostringstream out;
    out << std::setprecision(precision) << value;
    return out.str();
}

std::string json_string(const std::string &value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                std::ostringstream hex;
                hex << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                out += hex.str();
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
    return out;
}

std::string window_dates_text(const SignalEvaluation &signal) {
    if (signal.window_first.empty())
        return "n/a";
    return signal.window_first + " to " + signal.window_last;
}

std::string share_word(double quantity) {
    return std::abs(quantity - 1.0) < kShareTolerance ? " share" : " shares";
}

std::string broker_position_text(const BrokerView &broker) {
    if (!broker.confirmed)
        return "unknown — " + (broker.error.empty() ? "not confirmed by the broker" : broker.error);
    return format_number(broker.shares, 6) + share_word(broker.shares);
}

std::string target_text(const SignalEvaluation &signal) {
    if (!signal.target_shares)
        return "retain current position (close equals the prior average)";
    return format_number(*signal.target_shares, 6) + share_word(*signal.target_shares);
}

} // namespace

bool parse_daily_date(const std::string &date, std::int64_t &days_since_epoch) {
    if (date.size() != 10 || date[4] != '-' || date[7] != '-' || !valid_timestamp(date))
        return false;
    const int year = std::stoi(date.substr(0, 4));
    const auto month = static_cast<unsigned>(std::stoi(date.substr(5, 2)));
    const auto day = static_cast<unsigned>(std::stoi(date.substr(8, 2)));
    if (month < 1 || month > 12 || day < 1 || day > month_length(year, month))
        return false;
    days_since_epoch = days_from_civil(year, month, day);
    return true;
}

std::string utc_date(std::int64_t unix_seconds) {
    const std::time_t time = static_cast<std::time_t>(unix_seconds);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%d");
    return out.str();
}

std::string iso_date(int year, unsigned month, unsigned day) {
    if (month < 1 || month > 12 || day < 1 || day > month_length(year, month))
        throw std::invalid_argument("iso_date: not a real calendar date");
    return utc_date(days_from_civil(year, month, day) * 86400);
}

std::string utc_timestamp(std::int64_t unix_seconds) {
    const std::time_t time = static_cast<std::time_t>(unix_seconds);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::int64_t days_between(const std::string &from, const std::string &to) {
    std::int64_t a = 0, b = 0;
    if (!parse_daily_date(from, a) || !parse_daily_date(to, b))
        throw std::invalid_argument("days_between requires ISO YYYY-MM-DD dates");
    return b - a;
}

bool session_completed(const std::string &date, std::int64_t now_unix) {
    std::int64_t session = 0, today = 0;
    if (!parse_daily_date(date, session) || !parse_daily_date(utc_date(now_unix), today))
        return false;
    if (session < today)
        return true;
    if (session > today)
        return false;
    const int hour = static_cast<int>((now_unix % 86400) / 3600);
    return hour >= kSessionClosedUtcHour;
}

SeriesReport validate_daily_series(const std::vector<Bar> &bars) {
    SeriesReport report;
    report.bars = bars.size();
    if (bars.empty()) {
        report.error = "no bars supplied";
        return report;
    }
    report.ticker = bars.front().ticker;
    if (report.ticker == 0) {
        report.error = "bar ticker must be a positive instrument id";
        return report;
    }
    std::string previous;
    for (const auto &bar : bars) {
        if (bar.ticker != report.ticker) {
            report.error = "series mixes instruments (" + std::to_string(report.ticker) + " and " +
                           std::to_string(bar.ticker) + ")";
            return report;
        }
        if (bar.date.size() != 10) {
            report.error = "daily bars require YYYY-MM-DD dates; intraday timestamps are not "
                           "accepted for a daily decision (" +
                           bar.date + ")";
            return report;
        }
        std::int64_t days = 0;
        if (!parse_daily_date(bar.date, days)) {
            report.error = "invalid or non-existent calendar date: " + bar.date;
            return report;
        }
        if (!previous.empty() && bar.date <= previous) {
            report.error = "bars must be strictly chronological and unique; " + bar.date +
                           " follows " + previous;
            return report;
        }
        if (!std::isfinite(bar.open) || bar.open <= 0 || !std::isfinite(bar.close) || bar.close <= 0) {
            report.error = "prices must be finite and positive on " + bar.date;
            return report;
        }
        previous = bar.date;
    }
    report.ok = true;
    report.first_date = bars.front().date;
    report.last_date = bars.back().date;
    return report;
}

SignalEvaluation evaluate_signal(const std::vector<Bar> &bars, const SignalRequest &request) {
    SignalEvaluation out;
    out.window = request.window;
    out.quantity = request.quantity;
    if (request.window < 2 || request.window > 1000) {
        out.error = "window must be between 2 and 1000 sessions";
        return out;
    }
    if (!std::isfinite(request.quantity) || request.quantity <= 0) {
        out.error = "quantity must be finite and positive";
        return out;
    }
    const auto series = validate_daily_series(bars);
    if (!series.ok) {
        out.error = series.error;
        return out;
    }

    std::size_t index = bars.size() - 1;
    if (!request.as_of.empty()) {
        const auto it = std::find_if(bars.begin(), bars.end(),
                                     [&](const Bar &b) { return b.date == request.as_of; });
        if (it == bars.end()) {
            out.error = "as-of date " + request.as_of + " is not in the supplied series";
            return out;
        }
        index = static_cast<std::size_t>(std::distance(bars.begin(), it));
    }
    const std::string &decision_date = bars[index].date;

    if (!session_completed(decision_date, request.now_unix)) {
        out.error = "decision bar " + decision_date +
                    " is not a completed session yet; refusing to treat a still-forming or "
                    "future bar as the current session";
        return out;
    }
    if (request.as_of.empty()) {
        const auto age = days_between(decision_date, utc_date(request.now_unix));
        if (age > request.max_stale_days) {
            out.error = "stale data: last completed session " + decision_date + " is " +
                        std::to_string(age) + " calendar days old (limit " +
                        std::to_string(request.max_stale_days) + ")";
            return out;
        }
    } else {
        out.freshness_waived = true;
    }

    if (index < request.window) {
        out.error = "insufficient warmup: " + std::to_string(request.window) +
                    " prior completed closes are required, " + std::to_string(index) +
                    " are available before " + decision_date;
        return out;
    }

    // Replay the prior sessions through the strategy itself so the average is
    // computed by exactly the code the backtester runs.
    MovingAverageStrategy strategy(series.ticker, request.window, request.quantity);
    Portfolio portfolio(100000.0, "USD");
    for (std::size_t i = 0; i < index; ++i)
        strategy.on_bar(bars[i], portfolio);
    const auto average = strategy.prior_average();
    if (!average) {
        out.error = "strategy warmup did not fill after replaying " + std::to_string(index) +
                    " prior bars";
        return out;
    }

    out.decision_date = decision_date;
    out.decision_close = bars[index].close;
    out.prior_average = *average;
    out.prior_bars_available = strategy.observed();
    out.window_first = bars[index - request.window].date;
    out.window_last = bars[index - 1].date;
    if (out.decision_close > out.prior_average)
        out.state = SignalState::AboveAverage;
    else if (out.decision_close < out.prior_average)
        out.state = SignalState::BelowAverage;
    else
        out.state = SignalState::EqualToAverage;
    out.target_shares = out.state == SignalState::AboveAverage ? std::optional<double>(request.quantity)
                       : out.state == SignalState::BelowAverage ? std::optional<double>(0.0)
                                                                 : std::nullopt;
    out.ok = true;
    return out;
}

bool snapshot_matches_decision(double expected_shares, double expected_cash,
                               double actual_shares, double actual_cash) {
    return std::isfinite(expected_shares) && std::isfinite(expected_cash) &&
           std::isfinite(actual_shares) && std::isfinite(actual_cash) &&
           std::abs(expected_shares - actual_shares) <= 1e-9 &&
           std::abs(expected_cash - actual_cash) <= 1e-6;
}

Proposal propose_action(const SignalEvaluation &signal, const BrokerView &broker) {
    Proposal proposal;
    if (!signal.ok) {
        proposal.status = ProposalStatus::BlockedNoSignal;
        proposal.reason = "no signal: " + signal.error;
        return proposal;
    }
    if (!broker.confirmed) {
        proposal.status = ProposalStatus::BlockedUnknownBroker;
        proposal.reason = "broker position/open-order state is unknown (" +
                          (broker.error.empty() ? std::string("not confirmed") : broker.error) +
                          "); no order is proposed until it is reconciled";
        return proposal;
    }
    if (!broker.open_orders.empty()) {
        proposal.status = ProposalStatus::BlockedOpenOrders;
        proposal.reason = "the paper account holds " + std::to_string(broker.open_orders.size()) +
                          " open order(s) on this instrument; reconcile them before acting on a "
                          "new signal";
        return proposal;
    }
    const double held = broker.shares;
    const bool integral = std::isfinite(held) && std::abs(held - std::round(held)) < kShareTolerance;
    const bool expected =
        integral && (std::abs(held) < kShareTolerance ||
                     std::abs(held - signal.quantity) < kShareTolerance);
    if (!expected) {
        proposal.status = ProposalStatus::BlockedUnexpectedPosition;
        proposal.reason = "broker position " + format_number(held, 6) +
                          " is neither flat nor the strategy quantity " +
                          format_number(signal.quantity, 6) +
                          "; reconcile the account before trading this rule";
        return proposal;
    }
    const double target = signal.target_shares.value_or(held);
    proposal.resulting_position = target;
    if (std::abs(target - held) < kShareTolerance) {
        proposal.status = ProposalStatus::NoActionRequired;
        proposal.reason = signal.state == SignalState::EqualToAverage
                              ? "close equals the prior average, so the strategy retains the "
                                "current position"
                              : "the broker position already matches the strategy target";
        return proposal;
    }
    proposal.status = ProposalStatus::Actionable;
    proposal.side = target > held ? Side::Buy : Side::Sell;
    proposal.quantity = std::abs(target - held);
    proposal.reason = "strategy target " + format_number(target, 6) + " shares versus broker " +
                      format_number(held, 6) + " shares";
    return proposal;
}

ExecutionPlan plan_execution(const SignalEvaluation &signal, const Proposal &proposal,
                             double tick_size, int band_bps) {
    ExecutionPlan plan;
    plan.band_bps = band_bps;
    if (!std::isfinite(tick_size) || tick_size <= 0) {
        plan.error = "invalid tick size";
        return plan;
    }
    if (band_bps < 0 || band_bps > 500) {
        plan.error = "limit band must be 0..500 basis points";
        return plan;
    }
    if (!signal.ok) {
        plan.error = "no signal to execute";
        return plan;
    }
    if (proposal.status != ProposalStatus::Actionable || !proposal.side) {
        plan.error = "proposal is not actionable";
        return plan;
    }
    const double close = signal.decision_close;
    if (!std::isfinite(close) || close <= 0) {
        plan.error = "invalid reference close";
        return plan;
    }
    plan.side = *proposal.side;
    plan.quantity = proposal.quantity;
    plan.reference_close = close;
    const double band = static_cast<double>(band_bps) / 10000.0;
    const double raw = plan.side == Side::Buy ? close * (1.0 + band) : close * (1.0 - band);
    // Round adversely: a buy limit may only move up, a sell only down.
    const double ticks = raw / tick_size;
    const double rounded = plan.side == Side::Buy ? std::ceil(ticks - 1e-9) : std::floor(ticks + 1e-9);
    plan.limit_price = rounded * tick_size;
    if (!std::isfinite(plan.limit_price) || plan.limit_price <= 0) {
        plan.error = "derived limit price is not positive";
        return plan;
    }
    plan.ok = true;
    return plan;
}

std::string signal_state_name(SignalState state) {
    switch (state) {
    case SignalState::AboveAverage: return "above-average";
    case SignalState::BelowAverage: return "below-average";
    case SignalState::EqualToAverage: return "equal-to-average";
    }
    return "unknown";
}

std::string proposal_status_name(ProposalStatus status) {
    switch (status) {
    case ProposalStatus::Actionable: return "ACTIONABLE";
    case ProposalStatus::NoActionRequired: return "NO ACTION";
    case ProposalStatus::BlockedNoSignal: return "BLOCKED - no signal";
    case ProposalStatus::BlockedUnknownBroker: return "BLOCKED - broker state unknown";
    case ProposalStatus::BlockedOpenOrders: return "BLOCKED - open orders";
    case ProposalStatus::BlockedUnexpectedPosition: return "BLOCKED - unexpected position";
    }
    return "UNKNOWN";
}

std::string side_name(Side side) {
    return side == Side::Buy ? "BUY" : "SELL";
}

ReportFiles write_signal_report(const std::string &output_root, const SignalEvaluation &signal,
                                const BrokerView &broker, const Proposal &proposal,
                                const ReportContext &context, const std::vector<Bar> &bars_used) {
    namespace fs = std::filesystem;
    const fs::path root(output_root);
    fs::create_directories(root);
    std::string stamp = utc_timestamp(context.now_unix);
    std::replace(stamp.begin(), stamp.end(), ':', '-');
    // Never reuse a directory: a second run in the same second gets a numeric
    // suffix so earlier evidence cannot be overwritten.
    const std::string prefix = signal.ok && !signal.decision_date.empty() ? signal.decision_date
                                                                          : std::string("no-signal");
    fs::path directory;
    bool created = false;
    for (int attempt = 1; attempt <= 99 && !created; ++attempt) {
        const std::string suffix = attempt == 1 ? "" : "-" + std::to_string(attempt);
        directory = root / (prefix + "_" + stamp + suffix);
        std::error_code ec;
        created = fs::create_directory(directory, ec);
        if (!created && ec)
            throw std::runtime_error("cannot create report directory " + directory.string() + ": " +
                                     ec.message());
    }
    if (!created)
        throw std::runtime_error("cannot create a unique report directory under " + output_root);

    ReportFiles files;
    files.directory = directory.string();
    files.markdown = (directory / "signal.md").string();
    files.json = (directory / "signal.json").string();
    files.bars_csv = (directory / "bars-used.csv").string();
    files.command = (directory / "command.txt").string();

    {
        std::ofstream out(files.bars_csv);
        out << std::setprecision(17) << "date,ticker,open,close\n";
        for (const auto &bar : bars_used)
            out << bar.date << ',' << bar.ticker << ',' << bar.open << ',' << bar.close << '\n';
        if (!out)
            throw std::runtime_error("failed to write " + files.bars_csv);
    }
    {
        std::ofstream out(files.command);
        out << context.command << '\n';
        if (!out)
            throw std::runtime_error("failed to write " + files.command);
    }

    std::string action;
    if (proposal.status == ProposalStatus::Actionable && proposal.side)
        action = std::string(side_name(*proposal.side)) + " " +
                 format_number(proposal.quantity, 6) + " share(s) — NOT SUBMITTED";
    else
        action = "none";

    std::ostringstream md;
    md << "# Daily signal report — " << context.symbol << "\n\n";
    md << "- Run (UTC): " << utc_timestamp(context.now_unix) << "\n";
    md << "- Mode: read-only signal report. No order was submitted, modified or cancelled.\n";
    md << "- Status: **" << proposal_status_name(proposal.status) << "**\n";
    md << "- Source: " << context.source << "\n\n";

    md << "## Decision\n\n";
    md << "| Item | Value |\n| --- | --- |\n";
    md << "| Decision session (completed) | " << (signal.decision_date.empty() ? "n/a" : signal.decision_date)
       << " |\n";
    md << "| Close | " << (signal.ok ? format_number(signal.decision_close) : "n/a") << " "
       << context.currency << " |\n";
    md << "| Prior " << signal.window << "-session average | "
       << (signal.ok ? format_number(signal.prior_average) : "n/a") << " " << context.currency
       << " |\n";
    md << "| Averaged window | " << window_dates_text(signal) << " |\n";
    md << "| Signal | " << (signal.ok ? signal_state_name(signal.state) : "n/a") << " |\n";
    md << "| Strategy target | " << (signal.ok ? target_text(signal) : "n/a") << " |\n";
    md << "| Broker position | " << broker_position_text(broker) << " |\n";
    md << "| Open orders | "
       << (broker.confirmed ? (broker.open_orders.empty() ? "none" : std::to_string(broker.open_orders.size())) : "unknown")
       << " |\n";
    md << "| Proposed action | " << action << " |\n";
    md << "| Reason | " << proposal.reason << " |\n\n";

    if (!broker.confirmed || !broker.open_orders.empty()) {
        md << "> This report is not actionable. Reconcile the broker state before acting on it.\n\n";
    } else if (proposal.status == ProposalStatus::Actionable) {
        md << "> The signal implies a trade, but this command never sends one. Decide manually.\n\n";
    }

    md << "## Data quality\n\n";
    md << "- Bars supplied for this decision: " << bars_used.size()
       << " (1 decision session + " << signal.window << " prior sessions).\n";
    md << "- Ticker id: " << (bars_used.empty() ? 0 : bars_used.front().ticker) << "\n";
    md << "- Series span: "
       << (bars_used.empty() ? "n/a" : bars_used.front().date + " to " + bars_used.back().date) << "\n";
    md << "- Checks applied: daily (YYYY-MM-DD) dates only, one instrument, strictly increasing "
          "sessions (no duplicates), finite positive prices, full warmup, completed session, "
          "freshness.\n\n";

    md << "## Policy\n\n";
    md << "- Prior window: the average covers the " << signal.window
       << " completed closes *before* the decision session; the decision close is excluded.\n";
    md << "- Signal: close above the average targets " << format_number(signal.quantity, 6)
       << " share; below targets 0 shares; **equal retains the current position** "
          "(no target is asserted, so no order is proposed from the signal alone).\n";
    md << "- Session completion: a bar dated today is only accepted after "
       << kSessionClosedUtcHour << ":00 UTC (US regular close plus buffer). Otherwise the latest "
          "strictly earlier session is used, and a still-forming bar is never the decision bar.\n";
    md << "- Freshness: the latest completed session must be within " << 5
       << " calendar days when the decision comes from the newest bar. An explicit --as-of replay "
          "waives only the freshness check, never the completion check.\n\n";

    md << "## Data limitations\n\n";
    md << "- IBKR TRADES daily bars are split-adjusted but not dividend-adjusted; this is a "
          "price-only study, not a total-return series.\n";
    md << "- Splits, dividends, corporate actions and exchange holidays are not modelled.\n";
    md << "- The session-completion rule assumes US equity regular hours; futures and extended "
          "sessions need their own calendar.\n";
    md << "- No slippage, commission or fill assumption is applied: this report states intent, "
          "not execution economics.\n";
    md << "- Broker position and open orders are only as good as the snapshot this run received.\n\n";

    md << "## Files\n\n";
    md << "- `signal.json` — same content, machine-readable\n";
    md << "- `bars-used.csv` — exact bars behind the average\n";
    md << "- `command.txt` — the command that produced this report\n";

    {
        std::ofstream out(files.markdown);
        out << md.str();
        if (!out)
            throw std::runtime_error("failed to write " + files.markdown);
    }

    std::ostringstream json;
    json << "{\n";
    json << "  \"generated_at_utc\": " << json_string(utc_timestamp(context.now_unix)) << ",\n";
    json << "  \"mode\": \"read-only daily signal report\",\n";
    json << "  \"symbol\": " << json_string(context.symbol) << ",\n";
    json << "  \"source\": " << json_string(context.source) << ",\n";
    json << "  \"strategy\": {\"kind\": \"moving-average\", \"window\": " << signal.window
         << ", \"quantity\": " << format_number(signal.quantity) << "},\n";
    json << "  \"decision\": {\n";
    json << "    \"session\": " << json_string(signal.decision_date) << ",\n";
    json << "    \"close\": " << (signal.ok ? format_number(signal.decision_close) : "null") << ",\n";
    json << "    \"prior_average\": " << (signal.ok ? format_number(signal.prior_average) : "null") << ",\n";
    json << "    \"window_first\": " << json_string(signal.window_first) << ",\n";
    json << "    \"window_last\": " << json_string(signal.window_last) << ",\n";
    json << "    \"signal\": " << json_string(signal.ok ? signal_state_name(signal.state) : "unavailable") << ",\n";
    json << "    \"target_shares\": ";
    if (!signal.ok) json << "null";
    else if (!signal.target_shares) json << "null";
    else json << format_number(*signal.target_shares);
    json << ",\n";
    json << "    \"freshness_waived\": " << (signal.freshness_waived ? "true" : "false") << ",\n";
    json << "    \"bars_used\": " << bars_used.size() << "\n";
    json << "  },\n";
    json << "  \"broker\": {\n";
    json << "    \"confirmed\": " << (broker.confirmed ? "true" : "false") << ",\n";
    json << "    \"account\": " << json_string(broker.account) << ",\n";
    json << "    \"currency\": " << json_string(broker.currency) << ",\n";
    json << "    \"cash\": " << (broker.confirmed ? format_number(broker.cash) : "null") << ",\n";
    json << "    \"shares\": " << (broker.confirmed ? format_number(broker.shares) : "null") << ",\n";
    json << "    \"error\": " << json_string(broker.error) << ",\n";
    json << "    \"open_orders\": [";
    for (std::size_t i = 0; i < broker.open_orders.size(); ++i) {
        const auto &order = broker.open_orders[i];
        if (i) json << ", ";
        json << "{\"id\": " << order.order.id << ", \"side\": "
             << json_string(side_name(order.order.side)) << ", \"quantity\": "
             << format_number(order.order.quantity) << ", \"client_key\": "
             << json_string(order.order.client_key) << "}";
    }
    json << "]\n";
    json << "  },\n";
    json << "  \"proposal\": {\n";
    json << "    \"status\": " << json_string(proposal_status_name(proposal.status)) << ",\n";
    json << "    \"side\": " << (proposal.side ? json_string(side_name(*proposal.side)) : "null") << ",\n";
    json << "    \"quantity\": " << (proposal.side ? format_number(proposal.quantity) : "null") << ",\n";
    json << "    \"resulting_position\": " << format_number(proposal.resulting_position) << ",\n";
    json << "    \"submitted\": false,\n";
    json << "    \"reason\": " << json_string(proposal.reason) << "\n";
    json << "  },\n";
    json << "  \"command\": " << json_string(context.command) << "\n";
    json << "}\n";
    {
        std::ofstream out(files.json);
        out << json.str();
        if (!out)
            throw std::runtime_error("failed to write " + files.json);
    }
    return files;
}
} // namespace algo
