#pragma once
#include "Portfolio.hpp"
#include <string>
#include <vector>

struct EquityPoint { std::string timestamp; double equity{}; };

// Declaration-only scaffold; implementations are intentionally deferred.
class PerformanceReport {
public:
    // Append finite timestamp-ordered marked equity after fees; include initial equity.
    void record(std::string timestamp, double equity);
    // Compute final/initial minus one; define empty/zero initial equity error behavior.
    double total_return() const;
    // Track running peaks and largest fractional decline; return positive magnitude.
    double max_drawdown() const;
    // Export timestamp/equity with reproducible precision; report I/O failures.
    void write_csv(const std::string& path) const;
    // Clear all samples between runs.
    void reset();

private:
    std::vector<EquityPoint> equity_;
};
