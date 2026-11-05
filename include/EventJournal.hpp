#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace algo {
struct JournalRecord {
    std::uint64_t sequence{};
    std::string timestamp;
    std::string type;
    std::string payload;
};
// Linux/POSIX: exclusive process lock, checksummed records, fdatasync per append.
class EventJournal {
  public:
    explicit EventJournal(std::string path);
    ~EventJournal();
    EventJournal(const EventJournal &) = delete;
    EventJournal &operator=(const EventJournal &) = delete;
    void open();
    void append(const JournalRecord &record);
    std::vector<JournalRecord> replay() const;
    void flush();

  private:
    std::string path_;
    int fd_{-1};
    std::uint64_t next_sequence_{1};
};
} // namespace algo
