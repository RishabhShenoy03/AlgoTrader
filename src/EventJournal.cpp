#include "EventJournal.hpp"
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <unistd.h>

namespace algo {
namespace {
std::uint64_t checksum(const std::string &text) {
    std::uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}
} // namespace
EventJournal::EventJournal(std::string path) : path_(std::move(path)) {}
EventJournal::~EventJournal() {
    if (fd_ >= 0)
        ::close(fd_);
}
void EventJournal::open() {
    if (fd_ >= 0)
        return;
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd_ < 0)
        throw std::runtime_error("cannot open journal: " + path_);
    if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("journal already in use");
    }
    try {
        auto parent = std::filesystem::path(path_).parent_path();
        if (parent.empty())
            parent = ".";
        const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0)
            throw std::runtime_error("cannot open journal directory");
        const int synced = ::fsync(directory);
        ::close(directory);
        if (synced != 0)
            throw std::runtime_error("cannot persist journal directory entry");
        const auto records = replay();
        next_sequence_ = records.size() + 1;
    } catch (...) {
        ::close(fd_);
        fd_ = -1;
        throw;
    }
}
std::vector<JournalRecord> EventJournal::replay() const {
    std::ifstream in(path_, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read journal");
    std::vector<JournalRecord> records;
    std::string line;
    while (std::getline(in, line)) {
        if (in.eof())
            throw std::runtime_error("truncated journal; manual reconciliation required");
        const auto tab = line.rfind('\t');
        if (tab == std::string::npos)
            throw std::runtime_error("invalid journal record");
        const auto body = line.substr(0, tab);
        std::size_t used;
        std::uint64_t expected;
        try {
            expected = std::stoull(line.substr(tab + 1), &used, 16);
        } catch (...) {
            throw std::runtime_error("invalid journal checksum");
        }
        if (used != line.size() - tab - 1 || checksum(body) != expected)
            throw std::runtime_error("journal checksum mismatch");
        std::istringstream fields(body);
        std::string version;
        JournalRecord r;
        if (!(fields >> version >> r.sequence >> std::quoted(r.timestamp) >> std::quoted(r.type) >>
              std::quoted(r.payload)) ||
            version != "v1" || r.sequence != records.size() + 1)
            throw std::runtime_error("invalid journal sequence/schema");
        fields >> std::ws;
        if (!fields.eof())
            throw std::runtime_error("journal trailing fields");
        records.push_back(r);
    }
    if (in.bad())
        throw std::runtime_error("journal I/O failure");
    return records;
}
void EventJournal::append(const JournalRecord &r) {
    if (fd_ < 0)
        throw std::logic_error("journal not open");
    for (const auto &s : {r.timestamp, r.type, r.payload})
        if (s.find_first_of("\r\n") != std::string::npos)
            throw std::invalid_argument("journal fields cannot contain newlines");
    std::ostringstream body;
    body << "v1 " << next_sequence_ << ' ' << std::quoted(r.timestamp) << ' ' << std::quoted(r.type) << ' '
         << std::quoted(r.payload);
    std::ostringstream line;
    line << body.str() << '\t' << std::hex << checksum(body.str()) << '\n';
    const auto text = line.str();
    std::size_t done = 0;
    while (done < text.size()) {
        auto written = ::write(fd_, text.data() + done, text.size() - done);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            throw std::runtime_error("journal write failed");
        done += static_cast<std::size_t>(written);
    }
    flush();
    ++next_sequence_;
}
void EventJournal::flush() {
    if (fd_ < 0 || ::fdatasync(fd_) != 0)
        throw std::runtime_error("journal synchronization failed");
}
} // namespace algo
