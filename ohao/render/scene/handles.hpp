#pragma once
#include <cstdint>

namespace ohao::render {

inline constexpr uint32_t kInvalidIndex = UINT32_MAX;

// Generational handles: a slot index plus the generation it was issued at. A handle
// whose generation no longer matches its slot refers to something that was released.
struct MeshHandle {
    uint32_t index{kInvalidIndex};
    uint32_t generation{0};
    [[nodiscard]] constexpr bool valid() const noexcept { return index != kInvalidIndex; }
    constexpr bool operator==(const MeshHandle&) const = default;
};

struct MaterialHandle {
    uint32_t index{kInvalidIndex};
    uint32_t generation{0};
    [[nodiscard]] constexpr bool valid() const noexcept { return index != kInvalidIndex; }
    constexpr bool operator==(const MaterialHandle&) const = default;
};

} // namespace ohao::render
