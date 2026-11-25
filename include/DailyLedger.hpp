#pragma once
#include <string>
#include <vector>

namespace algo {

// Append-only record of every daily decision. One row per run, plus an INTENT row
// written before any order is sent, so a crashed execution is never repeated blindly.
struct LedgerEntry {
    std::string decided_at; // UTC timestamp
    std::string session;    // decision session (YYYY-MM-DD)
    std::string symbol;
    std::string state;      // signal state or "unavailable"
    std::string target;     // target shares or "retain"
    std::string broker_shares;
    std::string open_orders;
    std::string status;     // proposal status, or "EXEC:..." for an execution attempt
    std::string side;
    std::string quantity;
    std::string limit_price;
    std::string report_dir;
};

// Writes the header when the file does not exist yet, then appends exactly one row.
void append_ledger(const std::string &path, const LedgerEntry &entry);

// True when an execution row ("EXEC:...") already exists for this instrument and
// decision session. Report-only rows do not block a session. Throws when the ledger
// cannot be parsed: an unreadable ledger fails closed.
bool ledger_has_session(const std::string &path, const std::string &symbol, const std::string &session);

std::vector<LedgerEntry> read_ledger(const std::string &path);
} // namespace algo
