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

inline constexpr std::size_t kLightDataBytes       = 128;
inline constexpr std::size_t kGPULightBytes        = 80;  // 5 * vec4
inline constexpr std::size_t kPTPushConstantsBytes = 256;
inline constexpr std::size_t kMaterialVec4s        = 15;  // gpu::shared::kMaterialRows
inline constexpr std::size_t kMaterialBytes        = kMaterialVec4s * sizeof(glm::vec4);

// ─── Material SSBO packing (render::MaterialTable; core/gbuffer.slang, rt/path_tracer.slang) ─
//
// matColors[matID * 15 + 0]  = vec4(baseColor.rgb, uintBitsToFloat(diffuseTexIdx))
// matColors[matID * 15 + 1]  = vec4(roughness, metallic, normalTexIdx, emissiveTexIdx)
// matColors[matID * 15 + 2]  = vec4(roughMetalTexIdx, emissiveStrength, subsurface, occlusionTexIdx)
// matColors[matID * 15 + 3]  = vec4(alphaMode, alphaCutoff, opacity, baseColorAlpha)
// matColors[matID * 15 + 4]  = vec4(specularColor.rgb, specularWeight)                 (OpenPBR)
// matColors[matID * 15 + 5]  = vec4(specularIor, diffuseRoughness, baseWeight, specularTexIdx)
// matColors[matID * 15 + 6]  = vec4(emissionColor.rgb, specularColorTexIdx)
// matColors[matID * 15 + 7]  = vec4(coatColor.rgb, coatWeight)                         (OpenPBR step 2)
// matColors[matID * 15 + 8]  = vec4(coatRoughness, coatAnisotropy, coatIor, coatDarkening)
// matColors[matID * 15 + 9]  = vec4(fuzzColor.rgb, fuzzWeight)
// matColors[matID * 15 + 10] = vec4(fuzzRoughness, specularAnisotropy, tangentRotation, 0)
// matColors[matID * 15 + 11] = vec4(coatTex, coatRoughnessTex, fuzzColorTex, fuzzRoughnessTex)
// matColors[matID * 15 + 12] = vec4(transmissionColor.rgb, transmissionWeight)       (OpenPBR step 3)
// matColors[matID * 15 + 13] = vec4(transmissionDepth, thinWalled, dispersionScale, dispersionAbbe)
// matColors[matID * 15 + 14] = vec4(thinFilmWeight, thinFilmThickness, thinFilmIor, 0)

struct MaterialGpuPack {
    static constexpr std::size_t kSlots     = kMaterialVec4s;
    static constexpr std::size_t kBytes     = kMaterialBytes;
    static constexpr uint32_t    kNoTexture = 0xFFFFFFFFu;

    static constexpr int kBaseColorSlot = 0;
    static constexpr int kPbrSlot       = 1;
    static constexpr int kExtraSlot     = 2;
    static constexpr int kAlphaSlot     = 3;
    static constexpr int kSpecularSlot  = 4;
    static constexpr int kOpenPBRSlot   = 5;
    static constexpr int kEmissionSlot  = 6;
    static constexpr int kCoatSlot      = 7;
    static constexpr int kCoatRoughSlot = 8;
    static constexpr int kFuzzSlot      = 9;
    static constexpr int kLayerSlot     = 10;
    static constexpr int kLayerTexSlot  = 11;

    /// Byte offset of material `id` in the SSBO.
    [[nodiscard]] static constexpr std::size_t byteOffset(uint32_t materialId) noexcept {
        return static_cast<std::size_t>(materialId) * kBytes;
    }

    /// Number of vec4 slots for `count` materials.
    [[nodiscard]] static constexpr std::size_t vec4Count(uint32_t materialCount) noexcept {
        return static_cast<std::size_t>(materialCount) * kSlots;
    }
};

static_assert(MaterialGpuPack::kBytes == 240, "material pack is 15 * 16 bytes");
static_assert(MaterialGpuPack::byteOffset(2) == 480);
static_assert(MaterialGpuPack::vec4Count(4) == 60);

template<GpuPod T>
[[nodiscard]] consteval std::size_t gpu_sizeof() noexcept {
    return sizeof(T);
}

} // namespace layout
} // namespace ohao
