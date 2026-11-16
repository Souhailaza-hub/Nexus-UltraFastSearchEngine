#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nexus {

struct UsnFileEntry {
    std::uint64_t file_reference{};
    std::uint64_t parent_reference{};
    std::int64_t usn{};
    std::int64_t timestamp_filetime{};
    std::uint32_t reason{};
    std::uint32_t attributes{};
    std::wstring name;
};

struct UsnEnumBatch {
    std::uint64_t continuation_value{};
    std::vector<UsnFileEntry> entries;
};

// Parses the leading 64-bit continuation value and V2 records returned by
// FSCTL_ENUM_USN_DATA or FSCTL_READ_USN_JOURNAL. Leaves `out` unchanged on failure.
bool ParseUsnEnumBuffer(std::span<const std::byte> bytes, UsnEnumBatch& out, std::wstring* error = nullptr);
std::vector<UsnFileEntry> CoalesceUsnEvents(const std::vector<UsnFileEntry>& entries);

} // namespace nexus
