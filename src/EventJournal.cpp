#include "EventJournal.hpp"

// Intentionally unimplemented. Uncomment one template when implementing its milestone.
// Until then, using these declarations requires implementations and will fail to link.

// EventJournal::EventJournal(std::string path) {
//     TODO: Configure versioned append-only journal with account/run identity.
// }

// void EventJournal::open() {
//     TODO: Validate previous records and recover sequence; detect corruption and truncated final writes.
// }

// void EventJournal::append(const JournalRecord& record) {
//     TODO: Serialize structured payload; ensure durable write before external side effects; fail closed on disk errors.
// }

// std::vector<JournalRecord> EventJournal::replay() const {
//     TODO: Read and validate ordered records; never resubmit orders during replay.
// }

// void EventJournal::flush() {
//     TODO: Define OS durability guarantees; stream flush alone is not crash durability.
// }
