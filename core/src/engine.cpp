#include "nexus/engine.h"

#include <windows.h>
#include <exception>

namespace nexus {

std::size_t Engine::FileKeyHash::operator()(const FileKey& key) const noexcept {
    const auto a = std::hash<std::wstring>{}(key.volume);
    const auto b = std::hash<std::uint64_t>{}(key.id);
    return a ^ (b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2));
}

Engine::~Engine() { StopIndexing(); }

std::vector<VolumeInfo> Engine::DiscoverVolumes() const { return nexus::DiscoverVolumes(); }

void Engine::IndexPath(const std::filesystem::path& root) {
    std::lock_guard operation_lock(operation_mutex_);
    StopIndexingUnlocked();
    stop_requested_ = false;
    indexing_ = true;
    try {
        ScanPath(root);
    } catch (...) {
        ++errors_;
        indexing_ = false;
        throw;
    }
    indexing_ = false;
}

void Engine::StartIndexing(const std::vector<std::filesystem::path>& roots) {
    std::lock_guard operation_lock(operation_mutex_);
    StopIndexingUnlocked();
    stop_requested_ = false;
    indexing_ = true;
    try {
        worker_ = std::thread([this, roots] {
            try {
                for (const auto& root : roots) {
                    if (stop_requested_) break;
                    ScanPath(root);
                }
            } catch (...) { ++errors_; }
            indexing_ = false;
        });
    } catch (...) { indexing_ = false; throw; }
}

void Engine::StopIndexing() {
    // Set the cancellation flag before waiting for the operation lock so an
    // active synchronous scan can observe it and exit promptly.
    stop_requested_ = true;
    std::lock_guard operation_lock(operation_mutex_);
    StopIndexingUnlocked();
}

void Engine::StopIndexingUnlocked() {
    stop_requested_ = true;
    if (worker_.joinable()) worker_.join();
    indexing_ = false;
}

void Engine::AddRecords(std::vector<FileRecord>&& batch) {
    std::lock_guard lock(mutex_);
    for (auto& record : batch) AddRecordLocked(std::move(record));
}

void Engine::AddRecordLocked(FileRecord&& record) {
    const auto key = NormalizeName(record.name);
    const auto path_key = NormalizeName(record.path);
    const auto pos = records_.size();
    records_.push_back(std::move(record));
    normalized_names_.push_back(key);
    record_active_.push_back(1);
    exact_index_[key].push_back(pos);
    ordered_index_[key].push_back(pos);
    for (std::size_t n = 1; n <= 2 && n <= key.size(); ++n) {
        for (std::size_t i = 0; i + n <= key.size(); ++i) {
            const auto gram = key.substr(i, n);
            if (key.find(gram) == i) short_gram_index_[gram].push_back(pos);
        }
    }
    for (std::size_t i = 0; i + 3 <= key.size(); ++i) {
        const auto gram = key.substr(i, 3);
        if (key.find(gram) == i) trigram_index_[gram].push_back(pos);
    }
    const auto& stored = records_.back();
    const FileKey file_key{stored.volume_id, stored.file_id};
    records_by_file_id_[file_key].push_back(pos);
    record_by_path_[path_key] = pos;
    if (stored.is_directory) {
        directory_paths_by_file_id_[file_key] = stored.path;
        directory_ids_[stored.path] = stored.file_id;
    }
}

void Engine::CompactIndexesLocked() {
    std::vector<FileRecord> live;
    live.reserve(records_.size());
    for (std::size_t i = 0; i < records_.size(); ++i)
        if (record_active_[i]) live.push_back(std::move(records_[i]));
    records_.swap(live);
    normalized_names_.clear();
    record_active_.assign(records_.size(), 1);
    exact_index_.clear(); ordered_index_.clear(); trigram_index_.clear(); short_gram_index_.clear();
    records_by_file_id_.clear(); directory_paths_by_file_id_.clear(); record_by_path_.clear(); directory_ids_.clear();
    normalized_names_.reserve(records_.size());
    for (std::size_t pos = 0; pos < records_.size(); ++pos) {
        const auto key = NormalizeName(records_[pos].name);
        normalized_names_.push_back(key);
        exact_index_[key].push_back(pos);
        ordered_index_[key].push_back(pos);
        for (std::size_t n = 1; n <= 2 && n <= key.size(); ++n) {
            for (std::size_t i = 0; i + n <= key.size(); ++i) {
                const auto gram = key.substr(i, n);
                if (key.find(gram) == i) short_gram_index_[gram].push_back(pos);
            }
        }
        for (std::size_t i = 0; i + 3 <= key.size(); ++i) {
            const auto gram = key.substr(i, 3);
            if (key.find(gram) == i) trigram_index_[gram].push_back(pos);
        }
        const auto& record = records_[pos];
        const FileKey file_key{record.volume_id, record.file_id};
        records_by_file_id_[file_key].push_back(pos);
        record_by_path_[NormalizeName(record.path)] = pos;
        if (record.is_directory) {
            directory_paths_by_file_id_[file_key] = record.path;
            directory_ids_[record.path] = record.file_id;
        }
    }
}

IndexStats Engine::Stats() const {
    std::lock_guard lock(mutex_);
    IndexStats out{};
    out.errors = errors_.load();
    out.mft_fallbacks = mft_fallbacks_.load();
    out.indexing = indexing_.load();
    out.estimated_memory_bytes = records_.capacity() * sizeof(FileRecord);
    out.estimated_memory_bytes += normalized_names_.capacity() * sizeof(std::wstring);
    for (const auto& name : normalized_names_) out.estimated_memory_bytes += name.capacity() * sizeof(wchar_t);
    for (std::size_t i = 0; i < records_.size(); ++i) {
        const auto& r = records_[i];
        out.estimated_memory_bytes += (r.volume_id.capacity() + r.name.capacity() + r.extension.capacity() + r.path.capacity()) * sizeof(wchar_t);
        if (!record_active_[i]) continue;
        r.is_directory ? ++out.directories : ++out.files;
    }
    for (const auto& [key, values] : exact_index_) out.estimated_memory_bytes += key.capacity() * sizeof(wchar_t) + values.capacity() * sizeof(std::size_t);
    for (const auto& [key, values] : ordered_index_) out.estimated_memory_bytes += key.capacity() * sizeof(wchar_t) + values.capacity() * sizeof(std::size_t);
    for (const auto& [key, values] : trigram_index_) out.estimated_memory_bytes += key.capacity() * sizeof(wchar_t) + values.capacity() * sizeof(std::size_t);
    for (const auto& [key, values] : short_gram_index_) out.estimated_memory_bytes += key.capacity() * sizeof(wchar_t) + values.capacity() * sizeof(std::size_t);
    return out;
}

void Engine::Clear() {
    std::lock_guard operation_lock(operation_mutex_);
    StopIndexingUnlocked();
    std::lock_guard lock(mutex_);
    records_.clear(); normalized_names_.clear(); record_active_.clear(); exact_index_.clear(); ordered_index_.clear(); trigram_index_.clear(); short_gram_index_.clear(); directory_ids_.clear();
    records_by_file_id_.clear(); directory_paths_by_file_id_.clear(); record_by_path_.clear(); mft_indexed_volumes_.clear(); journal_checkpoints_.clear();
    next_id_ = 1; errors_ = 0; mft_fallbacks_ = 0;
}

bool Engine::SetJournalCheckpoint(const std::wstring& volume_root, std::uint64_t journal_id, std::int64_t next_usn) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(std::filesystem::path(volume_root), ec);
    if (ec || absolute.root_path().empty() || next_usn < 0) return false;
    const auto volume = absolute.root_path().lexically_normal().wstring();
    std::lock_guard lock(mutex_);
    if (!mft_indexed_volumes_.contains(volume)) return false;
    journal_checkpoints_[volume] = {journal_id, next_usn};
    return true;
}

std::optional<JournalCheckpoint> Engine::GetJournalCheckpoint(const std::wstring& volume_root) const {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(std::filesystem::path(volume_root), ec);
    if (ec || absolute.root_path().empty()) return std::nullopt;
    const auto volume = absolute.root_path().lexically_normal().wstring();
    std::lock_guard lock(mutex_);
    const auto it = journal_checkpoints_.find(volume);
    return it == journal_checkpoints_.end() ? std::nullopt : std::optional<JournalCheckpoint>(it->second);
}

} // namespace nexus
