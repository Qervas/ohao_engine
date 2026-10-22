#pragma once
#include <string>

namespace ohao::reflect {

// A reference to an asset file. `path` is project-relative with '/' separators.
// `guid` stays empty until the asset database exists; scene files write it only when set.
struct AssetRef {
    std::string path;
    std::string guid;
    bool operator==(const AssetRef&) const = default;
};

} // namespace ohao::reflect
