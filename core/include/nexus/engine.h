#pragma once

#include "nexus/file_record.h"
#include "nexus/volume.h"
#include "nexus/usn_journal_reader.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nexus {

struct IndexStats {
    std::uint64_t files{};
    std::uint64_t directories{};
    std::uint64_t errors{};
    std::uint64_t estimated_memory_bytes{};
    std::uint64_t mft_fallbacks{};
    bool indexing{};
};

struct JournalCheckpoint {
    std::uint64_t journal_id{};
    std::int64_t next_usn{};
};

struct SearchFilter {
    std::wstring extension;
    std::wstring path_prefix;
    std::wstring volume_id;
    bool files_only{};
    bool folders_only{};
    std::optional<std::uint64_t> min_size_bytes;
    std::optional<std::uint64_t> max_size_bytes;
    std::optional<std::int64_t> modified_after;
    std::optional<std::int64_t> modified_before;
};

class Engine {
public:
    Engine() = default;
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    std::vector<VolumeInfo> DiscoverVolumes() const;
    void IndexPath(const std::filesystem::path& root);
    void StartIndexing(const std::vector<std::filesystem::path>& roots);
    void StopIndexing();
    JournalReadResult SyncUsnJournal(const std::wstring& volume_root,
                                     std::uint64_t expected_journal_id,
                                     std::int64_t start_usn);
    bool SetJournalCheckpoint(const std::wstring& volume_root, std::uint64_t journal_id, std::int64_t next_usn);
    std::optional<JournalCheckpoint> GetJournalCheckpoint(const std::wstring& volume_root) const;
    bool SaveSnapshot(const std::filesystem::path& path) const;
    bool LoadSnapshot(const std::filesystem::path& path);
    std::vector<FileRecord> SearchExact(const std::wstring& query, std::size_t limit = 100) const;
    std::vector<FileRecord> SearchPrefix(const std::wstring& query, std::size_t limit = 100) const;
    std::vector<FileRecord> SearchSubstring(const std::wstring& query, std::size_t limit = 100) const;
    std::vector<FileRecord> SearchFiltered(const std::wstring& query, const std::wstring& mode,
                                           const SearchFilter& filter, std::size_t limit = 100) const;
    IndexStats Stats() const;
    void Clear();

private:
    friend struct EngineJournalTestAccess;
    struct FileKey {
        std::wstring volume;
        std::uint64_t id{};
        bool operator==(const FileKey&) const = default;
    };
    struct FileKeyHash {
        std::size_t operator()(const FileKey& key) const noexcept;
    };
    void ScanPath(const std::filesystem::path& root);
    void AddRecords(std::vector<FileRecord>&& records);
    void AddRecordLocked(FileRecord&& record);
    void CompactIndexesLocked();
    void StopIndexingUnlocked();
    bool ScanNtfsMft(const std::filesystem::path& volume_root);
    JournalReadResult ApplyUsnBatch(const std::wstring& volume,
                                    const std::vector<UsnFileEntry>& entries,
                                    JournalReadResult result);

    std::mutex operation_mutex_;
    mutable std::mutex mutex_;
    std::vector<FileRecord> records_;
    std::vector<std::wstring> normalized_names_;
    std::vector<std::uint8_t> record_active_;
    std::unordered_map<std::wstring, std::vector<std::size_t>> exact_index_;
    std::map<std::wstring, std::vector<std::size_t>> ordered_index_;
    std::unordered_map<std::wstring, std::vector<std::size_t>> trigram_index_;
    std::unordered_map<std::wstring, std::vector<std::size_t>> short_gram_index_;
    std::unordered_map<std::wstring, std::uint64_t> directory_ids_;
    std::unordered_map<FileKey, std::vector<std::size_t>, FileKeyHash> records_by_file_id_;
    std::unordered_map<FileKey, std::wstring, FileKeyHash> directory_paths_by_file_id_;
    std::unordered_map<std::wstring, std::size_t> record_by_path_;
    std::unordered_set<std::wstring> mft_indexed_volumes_;
    std::unordered_map<std::wstring, JournalCheckpoint> journal_checkpoints_;
    std::uint64_t next_id_{1};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> indexing_{false};
    std::atomic<std::uint64_t> errors_{0};
    std::atomic<std::uint64_t> mft_fallbacks_{0};
    std::thread worker_;
};

std::wstring NormalizeName(const std::wstring& value);

} // namespace nexus
