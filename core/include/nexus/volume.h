#pragma once

#include <string>
#include <vector>

namespace nexus {

struct VolumeInfo {
    std::wstring root_path;
    std::wstring filesystem;
    std::wstring label;
    unsigned drive_type{};
    bool ready{};
};

std::vector<VolumeInfo> DiscoverVolumes();

} // namespace nexus
