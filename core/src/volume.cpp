#include "nexus/volume.h"

#include <windows.h>
#include <utility>

namespace nexus {

std::vector<VolumeInfo> DiscoverVolumes() {
    std::vector<VolumeInfo> volumes;
    const DWORD mask = GetLogicalDrives();
    if (mask == 0) return volumes;

    for (unsigned i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        std::wstring root{static_cast<wchar_t>(L'A' + i), L':', L'\\'};
        const UINT type = GetDriveTypeW(root.c_str());
        wchar_t fs[MAX_PATH + 1]{};
        wchar_t label[MAX_PATH + 1]{};
        const bool ready = GetVolumeInformationW(root.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, fs, MAX_PATH) != 0;
        volumes.push_back({std::move(root), ready ? fs : L"", ready ? label : L"", type, ready});
    }
    return volumes;
}

} // namespace nexus
