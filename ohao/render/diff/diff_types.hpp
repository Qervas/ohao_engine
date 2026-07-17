#pragma once

// Diff-IR POD types — shared by pipeline / session / inverse backend.

#include <cstdint>

namespace ohao::diff {

struct DiffMapDesc {
    std::uint32_t width{64};
    std::uint32_t height{64};
    std::uint32_t channels{3}; // RGB albedo v0
};

struct DiffRenderDesc {
    std::uint32_t width{640};
    std::uint32_t height{360};
    std::uint32_t seed{1};
};

enum class DiffStatus : std::uint8_t {
    Ok = 0,
    NotInitialized,
    Unsupported,
    Failed,
};

[[nodiscard]] inline const char* diffStatusName(DiffStatus s) noexcept {
    switch (s) {
    case DiffStatus::Ok:
        return "ok";
    case DiffStatus::NotInitialized:
        return "not_initialized";
    case DiffStatus::Unsupported:
        return "unsupported";
    case DiffStatus::Failed:
        return "failed";
    }
    return "unknown";
}

} // namespace ohao::diff
