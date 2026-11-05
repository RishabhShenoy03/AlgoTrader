#pragma once
#include "Fill.hpp"
#include "Portfolio.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Declaration-only scaffold; implementations are intentionally deferred.
class FillProcessor {
public:
    // Restore deduplication state with portfolio on recovery.
    explicit FillProcessor(Portfolio& portfolio);
    // Apply confirmed execution once; support commission-before-fill; surface accounting failure and halt reconciliation.
    void on_fill(const Fill& fill);
    // Apply only difference from previously charged amount; validate currency; persist correction atomically with accounting.
    void on_commission(const Commission& commission);
    // Restore execution dedup state from the same checkpoint as portfolio and commissions.
    void restore(const std::vector<std::string>& execution_ids);

private:
    Portfolio& portfolio_;
    std::unordered_set<std::string> applied_executions_;
    std::unordered_map<std::string, Commission> commissions_;
};
