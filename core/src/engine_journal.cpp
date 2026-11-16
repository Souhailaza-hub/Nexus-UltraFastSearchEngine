#include "nexus/engine.h"

#include <windows.h>
#include <winioctl.h>

#include <filesystem>
#include <unordered_map>

namespace nexus {
namespace {
JournalReadResult Reconcile(JournalReadResult result, const wchar_t* reason) {
    result.state = JournalReadState::ReconciliationRequired;
    result.applied = false;
    result.message = reason;
    return result;
}

struct PlannedChange {
    std::uint64_t file_id{};
    bool deleted{};
    std::size_t existing_position{static_cast<std::size_t>(-1)};
    FileRecord record;
};
}

JournalReadResult Engine::SyncUsnJournal(const std::wstring& volume_root,
                                        std::uint64_t expected_journal_id,
                                        std::int64_t start_usn) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(std::filesystem::path(volume_root), ec);
    if (ec || absolute.root_path().empty()) {
        JournalReadResult result;
        result.state = JournalReadState::Error;
        result.message = L"Invalid volume root";
        return result;
    }
    const auto volume = absolute.root_path().lexically_normal().wstring();
    std::lock_guard operation_lock(operation_mutex_);
    if (indexing_) {
        JournalReadResult result;
        result.state = JournalReadState::ReconciliationRequired;
        result.message = L"Indexing is active; read the journal after the initial scan completes";
        return result;
    }
    {
        std::lock_guard lock(mutex_);
        if (!mft_indexed_volumes_.contains(volume)) {
            JournalReadResult result;
            result.state = JournalReadState::ReconciliationRequired;
            result.message = L"This volume lacks an MFT-backed identity map; a full reconciliation is required";
            return result;
        }
    }

    auto result = ReadUsnJournal(volume, expected_journal_id, start_usn);
    if (result.state != JournalReadState::Ok) return result;
    return ApplyUsnBatch(volume, result.entries, std::move(result));
}

JournalReadResult Engine::ApplyUsnBatch(const std::wstring& volume,
                                        const std::vector<UsnFileEntry>& entries,
                                        JournalReadResult result) {
    constexpr std::uint32_t supported = USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE |
        USN_REASON_DATA_OVERWRITE | USN_REASON_DATA_EXTEND | USN_REASON_DATA_TRUNCATION |
        USN_REASON_BASIC_INFO_CHANGE | USN_REASON_CLOSE;
    auto changes = CoalesceUsnEvents(entries);

    std::vector<PlannedChange> plan;
    plan.reserve(changes.size());
    for (const auto& change : changes) {
        if ((change.reason & ~supported) != 0 || (change.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return Reconcile(std::move(result), L"Rename, directory, hard-link, or unsupported change requires reconciliation");
        }
        if ((change.reason & (USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE | USN_REASON_DATA_OVERWRITE |
                              USN_REASON_DATA_EXTEND | USN_REASON_DATA_TRUNCATION | USN_REASON_BASIC_INFO_CHANGE)) == 0) {
            continue;
        }

        const FileKey key{volume, change.file_reference};
        std::size_t existing = static_cast<std::size_t>(-1);
        std::wstring parent_path;
        {
            std::lock_guard lock(mutex_);
            const auto found = records_by_file_id_.find(key);
            if (found != records_by_file_id_.end()) {
                for (const auto position : found->second) {
                    if (record_active_[position]) {
                        if (existing != static_cast<std::size_t>(-1))
                            return Reconcile(std::move(result), L"Multiple active links for one file ID require reconciliation");
                        existing = position;
                    }
                }
            }
            if ((change.reason & USN_REASON_FILE_DELETE) == 0) {
                const auto parent = directory_paths_by_file_id_.find(FileKey{volume, change.parent_reference});
                if (parent == directory_paths_by_file_id_.end())
                    return Reconcile(std::move(result), L"Parent directory identity is unknown; reconciliation is required");
                parent_path = parent->second;
            }
        }

        PlannedChange item;
        item.file_id = change.file_reference;
        item.deleted = (change.reason & USN_REASON_FILE_DELETE) != 0;
        item.existing_position = existing;
        if (!item.deleted) {
            if (existing == static_cast<std::size_t>(-1) && (change.reason & USN_REASON_FILE_CREATE) == 0)
                return Reconcile(std::move(result), L"Changed file ID is absent from the index; reconciliation is required");
            item.record.file_id = change.file_reference;
            item.record.parent_id = change.parent_reference;
            item.record.volume_id = volume;
            item.record.name = change.name;
            item.record.extension = std::filesystem::path(change.name).extension().wstring();
            item.record.path = (std::filesystem::path(parent_path) / change.name).lexically_normal().wstring();
            item.record.is_directory = false;
            if (existing != static_cast<std::size_t>(-1)) {
                std::lock_guard lock(mutex_);
                if (NormalizeName(records_[existing].path) != NormalizeName(item.record.path))
                    return Reconcile(std::move(result), L"File path changed without a supported rename pair; reconciliation is required");
            }
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(item.record.path.c_str(), GetFileExInfoStandard, &data) ||
                (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                return Reconcile(std::move(result), L"File metadata could not be read consistently; reconciliation is required");
            item.record.attributes = data.dwFileAttributes;
            item.record.size_bytes = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            ULARGE_INTEGER timestamp{};
            timestamp.LowPart = data.ftLastWriteTime.dwLowDateTime;
            timestamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
            item.record.modified_time = static_cast<std::int64_t>(timestamp.QuadPart);
        } else if (existing == static_cast<std::size_t>(-1)) {
            continue; // Duplicate delete event; already absent.
        }
        plan.push_back(std::move(item));
    }

    {
        std::lock_guard lock(mutex_);
        for (auto& item : plan) {
            const FileKey key{volume, item.file_id};
            if (item.deleted) {
                const auto position = item.existing_position;
                record_active_[position] = 0;
                const auto path_key = NormalizeName(records_[position].path);
                const auto path = record_by_path_.find(path_key);
                if (path != record_by_path_.end() && path->second == position) record_by_path_.erase(path);
            } else if (item.existing_position != static_cast<std::size_t>(-1)) {
                records_[item.existing_position] = std::move(item.record);
            } else {
                AddRecordLocked(std::move(item.record));
            }
            ++result.applied_changes;
        }
        if (records_.size() >= 4096) {
            std::size_t dead = 0;
            for (const auto active : record_active_) if (!active) ++dead;
            if (dead * 4 > records_.size()) CompactIndexesLocked();
        }
        journal_checkpoints_[volume] = {result.journal_id, result.next_usn};
    }
    result.applied = true;
    return result;
}

} // namespace nexus
