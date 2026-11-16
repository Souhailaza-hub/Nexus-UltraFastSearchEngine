#pragma once

#include "nexus/usn_record_parser.h"

#include <cstdint>
#include <string>

namespace nexus {

enum class JournalReadState {
    Ok,
    ReconciliationRequired,
    Unsupported,
    AccessDenied,
    Error
};

JournalReadState ValidateJournalCheckpoint(std::uint64_t expected_journal_id,
                                           std::uint64_t actual_journal_id,
                                           std::int64_t start_usn,
                                           std::int64_t lowest_valid_usn,
                                           std::int64_t next_usn);
JournalReadState ClassifyJournalReadError(std::uint32_t win32_error);

struct JournalReadResult {
    JournalReadState state{JournalReadState::Error};
    std::uint64_t journal_id{};
    std::int64_t next_usn{};
    std::int64_t lowest_valid_usn{};
    std::uint64_t applied_changes{};
    bool applied{};
    std::vector<UsnFileEntry> entries;
    std::wstring message;
};

JournalReadResult QueryUsnJournalCheckpoint(const std::wstring& volume_root);

// Reads one non-blocking journal batch. The caller must persist next_usn only
// after all returned entries have been applied successfully.
JournalReadResult ReadUsnJournal(const std::wstring& volume_root,
                                 std::uint64_t expected_journal_id,
                                 std::int64_t start_usn,
                                 std::uint32_t buffer_bytes = 1024 * 1024);

} // namespace nexus
