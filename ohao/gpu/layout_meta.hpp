#pragma once

/**
 * Compile-time GPU layout contracts (sizes + material pack).
 *
 * Owning headers also call OHAO_ASSERT_GPU_LAYOUT on their structs.
 * Include this when you need MaterialGpuPack or canonical byte sizes
 * without pulling the full renderer.
 */

#include "core/concepts.hpp"

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>

namespace ohao {
namespace layout {

// ─── Canonical sizes (std140/std430 / push-constant contracts) ──────────────

inline constexpr std::size_t kObjectPushConstantsBytes =
    3 * sizeof(glm::mat4) + 3 * sizeof(glm::vec4); // 240
inline constexpr std::size_t kLightDataBytes       = 128;
inline constexpr std::size_t kGPULightBytes        = 80;  // 5 * vec4
inline constexpr std::size_t kPTPushConstantsBytes = 256;
inline constexpr std::size_t kMaterialVec4s        = 3;
inline constexpr std::size_t kMaterialBytes        = kMaterialVec4s * sizeof(glm::vec4);

// ─── Material SSBO packing (must match pt_closesthit / rt_build) ────────────
//
// matColors[matID * 3 + 0] = vec4(baseColor.rgb, uintBitsToFloat(diffuseTexIdx))
// matColors[matID * 3 + 1] = vec4(roughness, metallic, normalTexIdx, emissiveTexIdx)
// matColors[matID * 3 + 2] = vec4(roughMetalTexIdx, unused, unused, unused)

struct MaterialGpuPack {
    static constexpr std::size_t kSlots     = kMaterialVec4s;
    static constexpr std::size_t kBytes     = kMaterialBytes;
    static constexpr uint32_t    kNoTexture = 0xFFFFFFFFu;

    static constexpr int kBaseColorSlot = 0;
    static constexpr int kPbrSlot       = 1;
    static constexpr int kExtraSlot     = 2;

    /// Byte offset of material `id` in the SSBO.
    [[nodiscard]] static constexpr std::size_t byteOffset(uint32_t materialId) noexcept {
        return static_cast<std::size_t>(materialId) * kBytes;
    }

    /// Number of vec4 slots for `count` materials.
    [[nodiscard]] static constexpr std::size_t vec4Count(uint32_t materialCount) noexcept {
        return static_cast<std::size_t>(materialCount) * kSlots;
    }
};

static_assert(MaterialGpuPack::kBytes == 48, "material pack is 3 * 16 bytes");
static_assert(MaterialGpuPack::byteOffset(2) == 96);
static_assert(MaterialGpuPack::vec4Count(4) == 12);

template<GpuPod T>
[[nodiscard]] consteval std::size_t gpu_sizeof() noexcept {
    return sizeof(T);
}

} // namespace layout
} // namespace ohao
