#include "nexus/usn_journal_reader.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace nexus {
namespace {
struct VolumeHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~VolumeHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

JournalReadResult Failure(JournalReadState state, const wchar_t* message) {
    JournalReadResult result;
    result.state = state;
    result.message = message;
    return result;
}
}

JournalReadState ValidateJournalCheckpoint(std::uint64_t expected_journal_id,
                                           std::uint64_t actual_journal_id,
                                           std::int64_t start_usn,
                                           std::int64_t lowest_valid_usn,
                                           std::int64_t next_usn) {
    if ((expected_journal_id != 0 && expected_journal_id != actual_journal_id) ||
        start_usn < lowest_valid_usn || start_usn < 0 || start_usn > next_usn)
        return JournalReadState::ReconciliationRequired;
    return JournalReadState::Ok;
}

JournalReadState ClassifyJournalReadError(std::uint32_t error) {
    if (error == ERROR_JOURNAL_ENTRY_DELETED || error == ERROR_JOURNAL_DELETE_IN_PROGRESS ||
        error == ERROR_JOURNAL_NOT_ACTIVE) return JournalReadState::ReconciliationRequired;
    return JournalReadState::Error;
}

JournalReadResult QueryUsnJournalCheckpoint(const std::wstring& volume_root) {
    if (volume_root.size() < 2 || volume_root[1] != L':')
        return Failure(JournalReadState::Error, L"Invalid volume root");
    const auto device = L"\\\\.\\" + volume_root.substr(0, 2);
    VolumeHandle volume{CreateFileW(device.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (volume.value == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD)
            return Failure(JournalReadState::AccessDenied, L"Volume journal access was denied; elevation may be required");
        return Failure(JournalReadState::Unsupported, L"Could not open volume for journal access");
    }
    USN_JOURNAL_DATA_V0 journal{};
    DWORD returned = 0;
    if (!DeviceIoControl(volume.value, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                         &journal, sizeof(journal), &returned, nullptr)) {
        const auto error = GetLastError();
        if (error == ERROR_JOURNAL_NOT_ACTIVE || error == ERROR_INVALID_FUNCTION)
            return Failure(JournalReadState::Unsupported, L"The volume has no readable USN journal");
        return Failure(JournalReadState::Error, L"FSCTL_QUERY_USN_JOURNAL failed");
    }
    JournalReadResult result;
    result.state = JournalReadState::Ok;
    result.journal_id = journal.UsnJournalID;
    result.next_usn = journal.NextUsn;
    result.lowest_valid_usn = journal.LowestValidUsn;
    return result;
}

JournalReadResult ReadUsnJournal(const std::wstring& volume_root,
                                std::uint64_t expected_journal_id,
                                std::int64_t start_usn,
                                std::uint32_t buffer_bytes) {
    if (volume_root.size() < 2 || volume_root[1] != L':' || buffer_bytes < 4096) {
        return Failure(JournalReadState::Error, L"Invalid volume root or output buffer size");
    }
    const auto device = L"\\\\.\\" + volume_root.substr(0, 2);
    VolumeHandle volume{CreateFileW(device.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (volume.value == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD)
            return Failure(JournalReadState::AccessDenied, L"Volume journal access was denied; elevation may be required");
        return Failure(JournalReadState::Unsupported, L"Could not open volume for journal access");
    }

    USN_JOURNAL_DATA_V0 journal{};
    DWORD returned = 0;
    if (!DeviceIoControl(volume.value, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                         &journal, sizeof(journal), &returned, nullptr)) {
        const auto error = GetLastError();
        if (error == ERROR_JOURNAL_NOT_ACTIVE || error == ERROR_INVALID_FUNCTION)
            return Failure(JournalReadState::Unsupported, L"The volume has no readable USN journal");
        return Failure(JournalReadState::Error, L"FSCTL_QUERY_USN_JOURNAL failed");
    }

    JournalReadResult result;
    result.journal_id = journal.UsnJournalID;
    result.next_usn = start_usn;
    result.lowest_valid_usn = journal.LowestValidUsn;
    result.state = ValidateJournalCheckpoint(expected_journal_id, journal.UsnJournalID, start_usn,
                                             journal.LowestValidUsn, journal.NextUsn);
    if (result.state != JournalReadState::Ok) {
        result.message = L"Journal identity changed or the checkpoint is outside the retained USN range";
        return result;
    }

    READ_USN_JOURNAL_DATA_V0 request{};
    request.StartUsn = start_usn;
    request.ReasonMask = MAXDWORD;
    request.ReturnOnlyOnClose = FALSE;
    request.Timeout = 0;
    request.BytesToWaitFor = 0;
    request.UsnJournalID = journal.UsnJournalID;
    std::vector<std::byte> buffer(buffer_bytes);
    returned = 0;
    if (!DeviceIoControl(volume.value, FSCTL_READ_USN_JOURNAL, &request, sizeof(request),
                         buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
        const auto error = GetLastError();
        result.state = ClassifyJournalReadError(error);
        if (result.state == JournalReadState::ReconciliationRequired) {
            result.message = L"The USN journal changed while reading; reconciliation is required";
            return result;
        }
        result.message = L"FSCTL_READ_USN_JOURNAL failed";
        return result;
    }

    UsnEnumBatch batch;
    if (!ParseUsnEnumBuffer(std::span<const std::byte>(buffer.data(), returned), batch, &result.message)) {
        result.state = JournalReadState::ReconciliationRequired;
        return result;
    }
    if (batch.continuation_value < static_cast<std::uint64_t>(start_usn) ||
        batch.continuation_value > static_cast<std::uint64_t>(journal.NextUsn)) {
        result.state = JournalReadState::ReconciliationRequired;
        result.message = L"The journal returned an invalid next-USN checkpoint";
        return result;
    }
    result.state = JournalReadState::Ok;
    result.next_usn = static_cast<std::int64_t>(batch.continuation_value);
    result.entries = std::move(batch.entries);
    return result;
}

} // namespace nexus
