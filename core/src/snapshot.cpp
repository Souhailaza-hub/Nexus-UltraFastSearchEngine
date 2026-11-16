#include "nexus/engine.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <utility>

namespace nexus {
namespace {
constexpr std::array<char, 8> kMagic{'N','E','X','U','S','I','D','X'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint32_t kEngineVersion = 1;
constexpr std::uint32_t kMaxStringChars = 32760;
constexpr std::uint64_t kMaxRecords = 50'000'000;
constexpr std::uint32_t kMaxVolumes = 1024;
constexpr std::uint64_t kMaxPayloadBytes = 16ull * 1024 * 1024 * 1024;

#pragma pack(push, 1)
struct SnapshotHeader {
    char magic[8];
    std::uint32_t format_version;
    std::uint32_t engine_version;
    std::uint64_t record_count;
    std::uint64_t payload_bytes;
    std::uint32_t checksum;
    std::uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(SnapshotHeader) == 40);
static_assert(sizeof(wchar_t) == 2);

void CrcUpdate(std::uint32_t& crc, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
}

bool WritePayload(std::ofstream& out, const void* data, std::size_t size, std::uint32_t& crc, std::uint64_t& bytes) {
    if (size > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)())) return false;
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!out) return false;
    CrcUpdate(crc, data, size);
    bytes += size;
    return true;
}

template<class T>
bool WriteValue(std::ofstream& out, const T& value, std::uint32_t& crc, std::uint64_t& bytes) {
    return WritePayload(out, &value, sizeof(T), crc, bytes);
}

bool WriteString(std::ofstream& out, const std::wstring& value, std::uint32_t& crc, std::uint64_t& bytes) {
    if (value.size() > kMaxStringChars) return false;
    const auto length = static_cast<std::uint32_t>(value.size());
    return WriteValue(out, length, crc, bytes) &&
        WritePayload(out, value.data(), value.size() * sizeof(wchar_t), crc, bytes);
}

bool ReadPayload(std::ifstream& in, void* data, std::size_t size, std::uint32_t& crc, std::uint64_t& bytes_left) {
    if (size > bytes_left || size > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)())) return false;
    in.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    if (!in) return false;
    CrcUpdate(crc, data, size);
    bytes_left -= size;
    return true;
}

template<class T>
bool ReadValue(std::ifstream& in, T& value, std::uint32_t& crc, std::uint64_t& bytes_left) {
    return ReadPayload(in, &value, sizeof(T), crc, bytes_left);
}

bool ReadString(std::ifstream& in, std::wstring& value, std::uint32_t& crc, std::uint64_t& bytes_left) {
    std::uint32_t length = 0;
    if (!ReadValue(in, length, crc, bytes_left) || length > kMaxStringChars) return false;
    value.resize(length);
    return ReadPayload(in, value.data(), static_cast<std::size_t>(length) * sizeof(wchar_t), crc, bytes_left);
}

bool WriteRecord(std::ofstream& out, const FileRecord& r, std::uint32_t& crc, std::uint64_t& bytes) {
    const std::uint8_t directory = r.is_directory ? 1 : 0;
    return WriteValue(out, r.file_id, crc, bytes) && WriteValue(out, r.parent_id, crc, bytes) &&
        WriteValue(out, r.size_bytes, crc, bytes) && WriteValue(out, r.modified_time, crc, bytes) &&
        WriteValue(out, r.attributes, crc, bytes) && WriteValue(out, directory, crc, bytes) &&
        WriteString(out, r.volume_id, crc, bytes) && WriteString(out, r.name, crc, bytes) &&
        WriteString(out, r.extension, crc, bytes) && WriteString(out, r.path, crc, bytes);
}

bool ReadRecord(std::ifstream& in, FileRecord& r, std::uint32_t& crc, std::uint64_t& bytes_left) {
    std::uint8_t directory = 0;
    if (!ReadValue(in, r.file_id, crc, bytes_left) || !ReadValue(in, r.parent_id, crc, bytes_left) ||
        !ReadValue(in, r.size_bytes, crc, bytes_left) || !ReadValue(in, r.modified_time, crc, bytes_left) ||
        !ReadValue(in, r.attributes, crc, bytes_left) || !ReadValue(in, directory, crc, bytes_left) || directory > 1 ||
        !ReadString(in, r.volume_id, crc, bytes_left) || !ReadString(in, r.name, crc, bytes_left) ||
        !ReadString(in, r.extension, crc, bytes_left) || !ReadString(in, r.path, crc, bytes_left)) return false;
    r.is_directory = directory != 0;
    return !r.volume_id.empty() && !r.path.empty();
}
}

bool Engine::SaveSnapshot(const std::filesystem::path& path) const {
    if (path.empty()) return false;
    std::vector<FileRecord> records;
    std::vector<std::wstring> mft_volumes;
    std::vector<std::pair<std::wstring, JournalCheckpoint>> checkpoints;
    {
        std::lock_guard lock(mutex_);
        records.reserve(records_.size());
        for (std::size_t i = 0; i < records_.size(); ++i)
            if (record_active_[i]) records.push_back(records_[i]);
        mft_volumes.assign(mft_indexed_volumes_.begin(), mft_indexed_volumes_.end());
        checkpoints.reserve(journal_checkpoints_.size());
        for (const auto& item : journal_checkpoints_) checkpoints.push_back(item);
    }
    if (mft_volumes.size() > kMaxVolumes || checkpoints.size() > kMaxVolumes || records.size() > kMaxRecords) return false;

    auto temporary = path;
    temporary += L".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    SnapshotHeader header{};
    std::copy(kMagic.begin(), kMagic.end(), header.magic);
    header.format_version = kFormatVersion;
    header.engine_version = kEngineVersion;
    header.record_count = records.size();
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!out) return false;

    std::uint32_t crc = 0xFFFFFFFFu;
    std::uint64_t payload_bytes = 0;
    const auto volume_count = static_cast<std::uint32_t>(mft_volumes.size());
    const auto checkpoint_count = static_cast<std::uint32_t>(checkpoints.size());
    bool ok = WriteValue(out, volume_count, crc, payload_bytes);
    for (const auto& volume : mft_volumes) ok = ok && WriteString(out, volume, crc, payload_bytes);
    ok = ok && WriteValue(out, checkpoint_count, crc, payload_bytes);
    for (const auto& [volume, checkpoint] : checkpoints) {
        ok = ok && WriteString(out, volume, crc, payload_bytes) &&
            WriteValue(out, checkpoint.journal_id, crc, payload_bytes) && WriteValue(out, checkpoint.next_usn, crc, payload_bytes);
    }
    for (const auto& record : records) ok = ok && WriteRecord(out, record, crc, payload_bytes);
    if (!ok) { out.close(); DeleteFileW(temporary.c_str()); return false; }

    header.payload_bytes = payload_bytes;
    header.checksum = ~crc;
    out.seekp(0);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.flush();
    if (!out) { out.close(); DeleteFileW(temporary.c_str()); return false; }
    out.close();
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool Engine::LoadSnapshot(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    SnapshotHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in || !std::equal(kMagic.begin(), kMagic.end(), header.magic) ||
        header.format_version != kFormatVersion || header.engine_version != kEngineVersion ||
        header.record_count > kMaxRecords || header.payload_bytes > kMaxPayloadBytes) return false;
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec || file_size != sizeof(header) + header.payload_bytes) return false;

    std::uint32_t crc = 0xFFFFFFFFu;
    auto bytes_left = header.payload_bytes;
    std::uint32_t volume_count = 0;
    if (!ReadValue(in, volume_count, crc, bytes_left) || volume_count > kMaxVolumes) return false;
    std::unordered_set<std::wstring> mft_volumes;
    for (std::uint32_t i = 0; i < volume_count; ++i) {
        std::wstring volume;
        if (!ReadString(in, volume, crc, bytes_left) || volume.empty()) return false;
        mft_volumes.insert(std::move(volume));
    }
    std::uint32_t checkpoint_count = 0;
    if (!ReadValue(in, checkpoint_count, crc, bytes_left) || checkpoint_count > kMaxVolumes) return false;
    std::unordered_map<std::wstring, JournalCheckpoint> checkpoints;
    for (std::uint32_t i = 0; i < checkpoint_count; ++i) {
        std::wstring volume;
        JournalCheckpoint checkpoint;
        if (!ReadString(in, volume, crc, bytes_left) || volume.empty() ||
            !ReadValue(in, checkpoint.journal_id, crc, bytes_left) ||
            !ReadValue(in, checkpoint.next_usn, crc, bytes_left) || checkpoint.next_usn < 0 ||
            !mft_volumes.contains(volume)) return false;
        checkpoints[std::move(volume)] = checkpoint;
    }
    std::vector<FileRecord> records;
    records.reserve(static_cast<std::size_t>(header.record_count));
    for (std::uint64_t i = 0; i < header.record_count; ++i) {
        FileRecord record;
        if (!ReadRecord(in, record, crc, bytes_left)) return false;
        records.push_back(std::move(record));
    }
    if (bytes_left != 0 || ~crc != header.checksum) return false;

    std::lock_guard operation_lock(operation_mutex_);
    StopIndexingUnlocked();
    std::lock_guard lock(mutex_);
    records_.clear(); normalized_names_.clear(); record_active_.clear();
    exact_index_.clear(); ordered_index_.clear(); trigram_index_.clear(); short_gram_index_.clear();
    directory_ids_.clear(); records_by_file_id_.clear(); directory_paths_by_file_id_.clear(); record_by_path_.clear();
    mft_indexed_volumes_ = std::move(mft_volumes);
    journal_checkpoints_ = std::move(checkpoints);
    next_id_ = 1;
    for (auto& record : records) {
        if (!mft_indexed_volumes_.contains(record.volume_id) && record.file_id >= next_id_ && record.file_id != (std::numeric_limits<std::uint64_t>::max)())
            next_id_ = record.file_id + 1;
        AddRecordLocked(std::move(record));
    }
    errors_ = 0;
    mft_fallbacks_ = 0;
    return true;
}

} // namespace nexus
