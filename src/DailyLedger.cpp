#include "DailyLedger.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace algo {
namespace {
const char *kHeader =
    "decided_at,session,symbol,state,target,broker_shares,open_orders,status,side,quantity,"
    "limit_price,report_dir";

// The ledger is plain unquoted CSV, like the rest of the project. Refuse fields
// that would corrupt it rather than guessing at an escaping scheme.
void validate_field(const std::string &value, const std::string &name) {
    if (value.find_first_of(",\"\n\r") != std::string::npos)
        throw std::invalid_argument("ledger field " + name + " may not contain a comma, quote or newline");
}

std::vector<std::string> split(const std::string &line, std::size_t expected, std::size_t row) {
    std::vector<std::string> fields;
    std::istringstream in(line);
    std::string field;
    while (std::getline(in, field, ','))
        fields.push_back(field);
    if (fields.size() != expected)
        throw std::runtime_error("ledger row " + std::to_string(row) + " has " +
                                 std::to_string(fields.size()) + " columns, expected " +
                                 std::to_string(expected));
    return fields;
}

std::vector<std::vector<std::string>> load(const std::string &path, std::size_t expected) {
    std::vector<std::vector<std::string>> rows;
    std::ifstream in(path);
    if (!in)
        return rows; // no ledger yet
    std::string line;
    if (!std::getline(in, line))
        return rows;
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line != kHeader)
        throw std::runtime_error("ledger " + path + " has an unexpected header");
    std::size_t number = 1;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        rows.push_back(split(line, expected, number));
    }
    return rows;
}
} // namespace

void append_ledger(const std::string &path, const LedgerEntry &entry) {
    for (const auto &field :
         {entry.decided_at, entry.session, entry.symbol, entry.state, entry.target,
          entry.broker_shares, entry.open_orders, entry.status, entry.side, entry.quantity,
          entry.limit_price, entry.report_dir})
        validate_field(field, "value");
    if (entry.session.size() != 10 || entry.symbol.empty())
        throw std::invalid_argument("ledger entry requires a session date and symbol");

    const std::filesystem::path file(path);
    if (file.has_parent_path())
        std::filesystem::create_directories(file.parent_path());
    const bool fresh = !std::filesystem::exists(file);
    std::ofstream out(file, std::ios::app);
    if (!out)
        throw std::runtime_error("cannot open ledger " + path);
    if (fresh)
        out << kHeader << '\n';
    out << entry.decided_at << ',' << entry.session << ',' << entry.symbol << ',' << entry.state << ','
        << entry.target << ',' << entry.broker_shares << ',' << entry.open_orders << ',' << entry.status
        << ',' << entry.side << ',' << entry.quantity << ',' << entry.limit_price << ','
        << entry.report_dir << '\n';
    out.flush();
    if (!out)
        throw std::runtime_error("cannot write ledger " + path);
}

bool ledger_has_session(const std::string &path, const std::string &symbol, const std::string &session) {
    // Only execution rows block a session: a plain report row records the decision
    // without claiming to have traded it.
    for (const auto &row : load(path, 12))
        if (row[1] == session && row[2] == symbol && row[7].rfind("EXEC:", 0) == 0)
            return true;
    return false;
}

std::vector<LedgerEntry> read_ledger(const std::string &path) {
    std::vector<LedgerEntry> entries;
    for (const auto &row : load(path, 12)) {
        LedgerEntry entry;
        entry.decided_at = row[0];
        entry.session = row[1];
        entry.symbol = row[2];
        entry.state = row[3];
        entry.target = row[4];
        entry.broker_shares = row[5];
        entry.open_orders = row[6];
        entry.status = row[7];
        entry.side = row[8];
        entry.quantity = row[9];
        entry.limit_price = row[10];
        entry.report_dir = row[11];
        entries.push_back(entry);
    }
    return entries;
}
} // namespace algo
