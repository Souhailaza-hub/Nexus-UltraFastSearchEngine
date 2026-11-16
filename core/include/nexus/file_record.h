#pragma once

#include <cstdint>
#include <string>

namespace nexus {

struct FileRecord {
    std::uint64_t file_id{};
    std::uint64_t parent_id{};
    std::wstring volume_id;
    std::wstring name;
    std::wstring extension;
    std::wstring path;
    std::uint64_t size_bytes{};
    std::int64_t modified_time{};
    std::uint32_t attributes{};
    bool is_directory{};
};

} // namespace nexus
