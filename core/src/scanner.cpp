#include "nexus/engine.h"

#include <windows.h>
#include <algorithm>
#include <vector>
#include <system_error>

namespace nexus {
namespace {
std::wstring VolumeKey(const std::filesystem::path& p) {
    const auto root = p.root_path().wstring();
    return root.empty() ? p.root_name().wstring() : root;
}
}

void Engine::ScanPath(const std::filesystem::path& root) {
    std::error_code ec;
    const auto absolute_root = std::filesystem::absolute(root, ec);
    if (ec || !std::filesystem::exists(absolute_root, ec) || ec || !std::filesystem::is_directory(absolute_root, ec) || ec) {
        ++errors_;
        return;
    }

    if (absolute_root == absolute_root.root_path()) {
        wchar_t fs_name[MAX_PATH + 1]{};
        if (GetVolumeInformationW(absolute_root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fs_name, MAX_PATH) &&
            _wcsicmp(fs_name, L"NTFS") == 0) {
            if (ScanNtfsMft(absolute_root)) return;
            if (stop_requested_) return;
            ++mft_fallbacks_;
        }
    }

    std::vector<FileRecord> batch;
    batch.reserve(1024);
    const auto volume = VolumeKey(absolute_root);
    auto add = [&](const std::filesystem::directory_entry& entry) {
        std::error_code e;
        FileRecord r;
        r.path = entry.path().lexically_normal().wstring();
        r.name = entry.path().filename().wstring();
        r.extension = entry.path().extension().wstring();
        r.volume_id = volume;
        bool recurse = false;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(r.path.c_str(), GetFileExInfoStandard, &data)) {
            r.attributes = data.dwFileAttributes;
            r.is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            recurse = r.is_directory && (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
            if (!r.is_directory) {
                r.size_bytes = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            }
            ULARGE_INTEGER write_time{};
            write_time.LowPart = data.ftLastWriteTime.dwLowDateTime;
            write_time.HighPart = data.ftLastWriteTime.dwHighDateTime;
            r.modified_time = static_cast<std::int64_t>(write_time.QuadPart);
        } else {
            ++errors_;
            r.is_directory = entry.is_directory(e);
            if (e) { ++errors_; e.clear(); }
            if (r.is_directory) {
                const auto symlink = entry.is_symlink(e);
                if (e) { ++errors_; e.clear(); }
                recurse = !symlink;
            }
        }
        {
            std::lock_guard lock(mutex_);
            r.file_id = next_id_++;
            const auto parent = entry.path().parent_path().lexically_normal().wstring();
            const auto it = directory_ids_.find(parent);
            if (it != directory_ids_.end()) r.parent_id = it->second;
            if (r.is_directory) directory_ids_[r.path] = r.file_id;
        }
        batch.push_back(std::move(r));
        if (batch.size() >= 1024) { AddRecords(std::move(batch)); batch.clear(); batch.reserve(1024); }
        return recurse;
    };

    const std::filesystem::directory_entry root_entry(absolute_root, ec);
    if (ec) { ++errors_; return; }
    add(root_entry);

    // Enumerate one directory at a time. A failed child-directory open then
    // increments the error count without aborting enumeration of its siblings.
    std::vector<std::filesystem::path> pending_directories{absolute_root};
    while (!pending_directories.empty() && !stop_requested_) {
        auto directory = std::move(pending_directories.back());
        pending_directories.pop_back();
        std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::none, ec), end;
        if (ec) { ++errors_; ec.clear(); continue; }
        for (; it != end && !stop_requested_;) {
            const auto entry = *it;
            if (add(entry)) pending_directories.push_back(entry.path());
            it.increment(ec);
            if (ec) { ++errors_; ec.clear(); break; }
        }
    }
    AddRecords(std::move(batch));
}

} // namespace nexus
