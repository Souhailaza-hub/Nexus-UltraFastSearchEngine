#include "nexus/engine.h"

#include <windows.h>

namespace nexus {

std::wstring NormalizeName(const std::wstring& value) {
    if (value.empty()) return {};
    const int needed = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
    if (needed <= 0) return value;
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    const int written = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), static_cast<int>(value.size()), out.data(), needed, nullptr, nullptr, 0);
    if (written <= 0) return value;
    out.resize(static_cast<std::size_t>(written));
    return out;
}

std::vector<FileRecord> Engine::SearchExact(const std::wstring& query, std::size_t limit) const {
    return SearchFiltered(query, L"exact", {}, limit);
}

std::vector<FileRecord> Engine::SearchPrefix(const std::wstring& query, std::size_t limit) const {
    return SearchFiltered(query, L"prefix", {}, limit);
}

std::vector<FileRecord> Engine::SearchSubstring(const std::wstring& query, std::size_t limit) const {
    return SearchFiltered(query, L"substring", {}, limit);
}

std::vector<FileRecord> Engine::SearchFiltered(const std::wstring& query, const std::wstring& mode,
                                               const SearchFilter& filter, std::size_t limit) const {
    std::vector<FileRecord> out;
    const auto key = NormalizeName(query);
    if (key.empty() || limit == 0 || (filter.files_only && filter.folders_only)) return out;

    const auto extension = NormalizeName(filter.extension);
    const auto path_prefix = NormalizeName(filter.path_prefix);
    const auto volume = NormalizeName(filter.volume_id);
    if (filter.min_size_bytes && filter.max_size_bytes && *filter.min_size_bytes > *filter.max_size_bytes) return out;
    if (filter.modified_after && filter.modified_before && *filter.modified_after > *filter.modified_before) return out;

    std::lock_guard lock(mutex_);

    auto matches_filter = [&](std::size_t pos) {
        const auto& record = records_[pos];
        if (filter.files_only && record.is_directory) return false;
        if (filter.folders_only && !record.is_directory) return false;
        if (!extension.empty() && NormalizeName(record.extension) != extension) return false;
        if (!path_prefix.empty() && NormalizeName(record.path).compare(0, path_prefix.size(), path_prefix) != 0) return false;
        if (!volume.empty() && NormalizeName(record.volume_id) != volume) return false;
        if (filter.min_size_bytes || filter.max_size_bytes) {
            if (record.is_directory) return false;
            if (filter.min_size_bytes && record.size_bytes < *filter.min_size_bytes) return false;
            if (filter.max_size_bytes && record.size_bytes > *filter.max_size_bytes) return false;
        }
        if (filter.modified_after || filter.modified_before) {
            if (filter.modified_after && record.modified_time < *filter.modified_after) return false;
            if (filter.modified_before && record.modified_time > *filter.modified_before) return false;
        }
        return true;
    };
    auto emit = [&](const std::vector<std::size_t>& positions, bool verify_substring) {
        for (const auto pos : positions) {
            if (out.size() >= limit) return true;
            if (!record_active_[pos]) continue;
            if (verify_substring && normalized_names_[pos].find(key) == std::wstring::npos) continue;
            if (matches_filter(pos)) out.push_back(records_[pos]);
        }
        return false;
    };

    if (mode == L"exact") {
        const auto it = exact_index_.find(key);
        if (it != exact_index_.end()) emit(it->second, false);
    } else if (mode == L"prefix") {
        for (auto it = ordered_index_.lower_bound(key); it != ordered_index_.end(); ++it) {
            if (it->first.compare(0, key.size(), key) != 0) break;
            if (emit(it->second, false)) break;
        }
    } else if (mode == L"substring") {
        const std::vector<std::size_t>* candidates = nullptr;
        if (key.size() < 3) {
            const auto it = short_gram_index_.find(key);
            if (it == short_gram_index_.end()) return out;
            candidates = &it->second;
        } else {
            for (std::size_t i = 0; i + 3 <= key.size(); ++i) {
                const auto it = trigram_index_.find(key.substr(i, 3));
                if (it == trigram_index_.end()) return out;
                if (candidates == nullptr || it->second.size() < candidates->size()) candidates = &it->second;
            }
        }
        if (candidates != nullptr) emit(*candidates, true);
    }
    return out;
}

} // namespace nexus
