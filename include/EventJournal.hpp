#pragma once
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

struct JournalRecord {
    std::uint64_t sequence{};
    std::string timestamp;
    std::string type;
    std::string payload; // Versioned structured serialization, not arbitrary log prose.
};

// Declaration-only scaffold; implementations are intentionally deferred.
class EventJournal {
public:
    // Configure versioned append-only journal with account/run identity.
    explicit EventJournal(std::string path);
    // Validate previous records and recover sequence; detect corruption and truncated final writes.
    void open();
    // Serialize structured payload; ensure durable write before external side effects; fail closed on disk errors.
    void append(const JournalRecord& record);
    // Read and validate ordered records; never resubmit orders during replay.
    std::vector<JournalRecord> replay() const;
    // Define OS durability guarantees; stream flush alone is not crash durability.
    void flush();

private:
    std::string path_;
    std::ofstream output_;
    std::uint64_t next_sequence_{1};
};
