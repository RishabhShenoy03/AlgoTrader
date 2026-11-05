#include "PerformanceReport.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace algo {
void PerformanceReport::record(std::string timestamp, double equity) {
    if (!std::isfinite(equity))
        throw std::invalid_argument("nonfinite equity");
    if (!equity_.empty() && equity_.back().timestamp != "initial" && timestamp <= equity_.back().timestamp)
        throw std::invalid_argument("equity timestamps must increase");
    equity_.push_back({std::move(timestamp), equity});
}
double PerformanceReport::total_return() const {
    if (equity_.empty() || equity_.front().equity <= 0)
        throw std::runtime_error("return requires positive starting equity");
    return equity_.back().equity / equity_.front().equity - 1;
}
double PerformanceReport::max_drawdown() const {
    double peak = 0, dd = 0;
    for (const auto &p : equity_) {
        peak = std::max(peak, p.equity);
        if (peak > 0)
            dd = std::max(dd, (peak - p.equity) / peak);
    }
    return dd;
}
void PerformanceReport::write_csv(const std::string &path) const {
    std::ofstream out(path);
    out << std::setprecision(17) << "timestamp,equity\n";
    for (const auto &p : equity_)
        out << p.timestamp << ',' << p.equity << '\n';
    out.flush();
    if (!out)
        throw std::runtime_error("cannot write equity CSV: " + path);
}
void PerformanceReport::write_trades(const std::string &path, const FillProcessor &fills) const {
    std::ofstream out(path);
    out << std::setprecision(17) << "execution_id,order_id,timestamp,ticker,side,quantity,price\n";
    for (const auto &f : fills.executions())
        out << f.execution_id << ',' << f.order_id << ',' << f.executed_at << ',' << f.ticker << ','
            << (f.side == Side::Buy ? "buy" : "sell") << ',' << f.quantity << ',' << f.price << '\n';
    out.flush();
    if (!out)
        throw std::runtime_error("cannot write trades CSV");
}
const std::vector<EquityPoint> &PerformanceReport::points() const noexcept {
    return equity_;
}
void PerformanceReport::reset() {
    equity_.clear();
}
} // namespace algo
