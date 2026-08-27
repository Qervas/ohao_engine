#pragma once

#include "diff/grad/arena_layout.hpp"

#include <cstdint>

namespace ohao::diff {

/// One scalar field per enumerator. Deliberately scalar rather than vec3:
/// a wavefront stage that only needs throughput should read one dense run,
/// not stride across interleaved path structs.
enum class PathStateField : std::uint32_t {
    OriginX, OriginY, OriginZ,
    DirX, DirY, DirZ,
    ThroughputR, ThroughputG, ThroughputB,
    RadianceR, RadianceG, RadianceB,
    PixelIndex,    // bit-cast uint
    SampleIndex,   // bit-cast uint
    Bounce,        // bit-cast uint
    Alive,         // bit-cast uint, 0 or 1
    HitT,          // intersect-stage output: hit distance, -1 on miss (Task 5)
    Count
};

/// SoA offsets for `capacity` in-flight paths. Pure -- no Vulkan.
///
/// shaders/includes/diff/path_state.glsl mirrors this layout. The two are a
/// matched pair like PathRng and rng.glsl: change both or neither, and the
/// probe's field round-trip check is what proves they agree.
class PathStateLayout {
public:
    explicit PathStateLayout(std::uint32_t capacity);

    [[nodiscard]] std::size_t block(PathStateField field) const;
    [[nodiscard]] std::uint32_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] const ArenaLayout& arena() const noexcept { return m_arena; }

    static constexpr std::uint32_t kFieldCount =
        static_cast<std::uint32_t>(PathStateField::Count);

private:
    std::uint32_t m_capacity{0};
    ArenaLayout m_arena;
    std::size_t m_blocks[kFieldCount];
};

}  // namespace ohao::diff
