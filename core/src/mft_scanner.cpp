#include "nexus/engine.h"
#include "nexus/usn_record_parser.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

namespace nexus {
namespace {
struct VolumeHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~VolumeHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct RawEntry {
    UsnFileEntry usn;
    std::wstring path;
    bool is_directory{};
};

bool ResolveDirectoryPath(const std::unordered_map<std::uint64_t, std::size_t>& directories,
                          const std::vector<RawEntry>& entries,
                          std::uint64_t directory_id,
                          std::uint64_t root_id,
                          const std::wstring& root,
                          std::wstring& result) {
    if (directory_id == root_id) { result = root; return true; }
    std::vector<const RawEntry*> components;
    std::unordered_set<std::uint64_t> visited;
    auto current = directory_id;
    while (current != root_id) {
        if (!visited.insert(current).second) return false;
        const auto it = directories.find(current);
        if (it == directories.end()) return false;
        const auto& entry = entries[it->second];
        if (!entry.is_directory || entry.usn.name.empty()) return false;
        components.push_back(&entry);
        current = entry.usn.parent_reference;
        if (components.size() > directories.size()) return false;
    }
    std::filesystem::path path(root);
    for (auto it = components.rbegin(); it != components.rend(); ++it) path /= (*it)->usn.name;
    result = path.lexically_normal().wstring();
    return true;
}
}

bool Engine::ScanNtfsMft(const std::filesystem::path& volume_root) {
    const auto root = volume_root.wstring();
    if (root.size() < 2 || root[1] != L':') return false;
    const std::wstring device = L"\\\\.\\" + root.substr(0, 2);
    VolumeHandle volume{CreateFileW(device.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (volume.value == INVALID_HANDLE_VALUE) return false;

    std::vector<UsnFileEntry> raw;
    std::vector<std::byte> buffer(1024 * 1024);
    MFT_ENUM_DATA_V0 request{};
    request.StartFileReferenceNumber = 0;
    request.LowUsn = 0;
    request.HighUsn = MAXLONGLONG;

    for (;;) {
        DWORD returned = 0;
        if (!DeviceIoControl(volume.value, FSCTL_ENUM_USN_DATA, &request, sizeof(request),
                             buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return false;
        }
        if (returned == 0) break;
        UsnEnumBatch batch;
        if (!ParseUsnEnumBuffer(std::span<const std::byte>(buffer.data(), returned), batch)) return false;
        if (batch.entries.empty()) break;
        const auto next = batch.continuation_value;
        if (next <= request.StartFileReferenceNumber) return false;
        raw.insert(raw.end(), std::make_move_iterator(batch.entries.begin()), std::make_move_iterator(batch.entries.end()));
        request.StartFileReferenceNumber = next;
        if (stop_requested_) return false;
    }

    std::unordered_map<std::uint64_t, std::size_t> directories;
    std::vector<RawEntry> entries;
    entries.reserve(raw.size());
    std::uint64_t root_id = 0;
    for (auto& item : raw) {
        RawEntry entry;
        entry.is_directory = (item.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.usn = std::move(item);
        if (entry.is_directory) {
            // The NTFS root MFT record number is 5; the upper bits are its sequence number.
            if ((entry.usn.file_reference & 0x0000FFFFFFFFFFFFull) == 5) root_id = entry.usn.file_reference;
            directories.emplace(entry.usn.file_reference, entries.size());
        }
        entries.push_back(std::move(entry));
    }
    if (root_id == 0) return false;

    std::vector<FileRecord> completed;
    completed.reserve(entries.size());
    for (auto& entry : entries) {
        FileRecord record;
        record.file_id = entry.usn.file_reference;
        record.parent_id = entry.usn.parent_reference;
        record.volume_id = root;
        record.name = entry.usn.name;
        record.extension = std::filesystem::path(record.name).extension().wstring();
        record.is_directory = entry.is_directory;
        if (record.file_id == root_id) {
            record.path = root;
            record.name = L"";
            record.extension.clear();
        } else {
            std::wstring parent_path;
            if (!ResolveDirectoryPath(directories, entries, record.parent_id, root_id, root, parent_path)) return false;
            record.path = (std::filesystem::path(parent_path) / record.name).lexically_normal().wstring();
        }

        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(record.path.c_str(), GetFileExInfoStandard, &data)) {
            record.attributes = data.dwFileAttributes;
            record.is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!record.is_directory) record.size_bytes = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            ULARGE_INTEGER timestamp{};
            timestamp.LowPart = data.ftLastWriteTime.dwLowDateTime;
            timestamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
            record.modified_time = static_cast<std::int64_t>(timestamp.QuadPart);
        } else {
            ++errors_;
            record.attributes = entry.usn.attributes;
        }
        completed.push_back(std::move(record));
    }
    for (std::size_t offset = 0; offset < completed.size(); offset += 1024) {
        const auto count = (std::min)(std::size_t{1024}, completed.size() - offset);
        std::vector<FileRecord> batch;
        batch.reserve(count);
        for (std::size_t i = 0; i < count; ++i) batch.push_back(std::move(completed[offset + i]));
        AddRecords(std::move(batch));
    }
    {
        std::lock_guard lock(mutex_);
        mft_indexed_volumes_.insert(root);
    }
    return true;
}

} // namespace nexus
