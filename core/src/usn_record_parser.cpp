#include "nexus/usn_record_parser.h"

#include <windows.h>
#include <winioctl.h>

#include <cstddef>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace nexus {
namespace {
void SetError(std::wstring* out, const wchar_t* message) {
    if (out) *out = message;
}
}

bool ParseUsnEnumBuffer(std::span<const std::byte> bytes, UsnEnumBatch& out, std::wstring* error) {
    static_assert(sizeof(wchar_t) == sizeof(WORD));
    constexpr std::size_t prefix_size = sizeof(ULONGLONG);
    constexpr std::size_t name_offset_min = offsetof(USN_RECORD_V2, FileName);
    if (bytes.size() < prefix_size) {
        SetError(error, L"USN enumeration buffer is shorter than its continuation reference");
        return false;
    }

    UsnEnumBatch parsed;
    ULONGLONG next_reference{};
    std::memcpy(&next_reference, bytes.data(), sizeof(next_reference));
    parsed.continuation_value = next_reference;

    std::size_t offset = prefix_size;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        if (remaining < name_offset_min) {
            SetError(error, L"Truncated USN record header");
            return false;
        }

        USN_RECORD_V2 record{};
        std::memcpy(&record, bytes.data() + offset, name_offset_min);
        if (record.RecordLength < name_offset_min || record.RecordLength > remaining) {
            SetError(error, L"Invalid USN record length");
            return false;
        }
        if (record.MajorVersion != 2) {
            SetError(error, L"Unsupported USN record version");
            return false;
        }
        if ((record.FileNameLength % sizeof(WCHAR)) != 0 ||
            record.FileNameOffset < name_offset_min ||
            record.FileNameOffset > record.RecordLength ||
            record.FileNameLength > record.RecordLength - record.FileNameOffset) {
            SetError(error, L"Invalid USN filename range");
            return false;
        }

        UsnFileEntry entry;
        entry.file_reference = record.FileReferenceNumber;
        entry.parent_reference = record.ParentFileReferenceNumber;
        entry.usn = record.Usn;
        entry.timestamp_filetime = record.TimeStamp.QuadPart;
        entry.reason = record.Reason;
        entry.attributes = record.FileAttributes;
        entry.name.resize(record.FileNameLength / sizeof(WCHAR));
        if (!entry.name.empty()) {
            std::memcpy(entry.name.data(), bytes.data() + offset + record.FileNameOffset, record.FileNameLength);
        }
        parsed.entries.push_back(std::move(entry));
        offset += record.RecordLength;
    }

    out = std::move(parsed);
    if (error) error->clear();
    return true;
}

std::vector<UsnFileEntry> CoalesceUsnEvents(const std::vector<UsnFileEntry>& entries) {
    std::vector<UsnFileEntry> combined;
    combined.reserve(entries.size());
    std::unordered_map<std::uint64_t, std::size_t> positions;
    positions.reserve(entries.size());
    for (const auto& entry : entries) {
        const auto [it, inserted] = positions.emplace(entry.file_reference, combined.size());
        if (inserted) {
            combined.push_back(entry);
            continue;
        }
        auto& current = combined[it->second];
        const auto reasons = current.reason | entry.reason;
        if (entry.usn >= current.usn) current = entry;
        current.reason |= reasons;
    }
    return combined;
}

} // namespace nexus
