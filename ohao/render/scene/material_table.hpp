#pragma once
#include "render/scene/render_scene_types.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"

#include <array>
#include <set>
#include <vector>

namespace ohao::render {

// Material table: a fixed-capacity device-local array of 192-byte packs (the
// MaterialGpuPack layout, gpu::shared::kMaterialRows rows: emissive strength in row 2 .y,
// subsurface in row 2 .z, alpha in row 3, OpenPBR's specular and base in rows 4-6, its coat, fuzz
// and anisotropy in rows 7-11, transmission and the thin film in rows 12-14). Index 0 is the default material. Writes are
// staged through the frame slot's staging buffer.
class MaterialTable {
public:
    static constexpr uint32_t kCapacity = 4096;
    using Pack = std::array<float, 60>;
    static_assert(sizeof(Pack) == layout::MaterialGpuPack::kBytes);

    [[nodiscard]] bool initialize(GpuAllocator& allocator);   // registers MaterialDesc{} as index 0
    void shutdown();

    // Full table: logs and returns the default material.
    MaterialHandle add(const MaterialDesc& desc);
    [[nodiscard]] uint32_t count() const noexcept { return static_cast<uint32_t>(m_packs.size()); }
    [[nodiscard]] bool doubleSided(uint32_t index) const noexcept {
        return index >= m_doubleSided.size() || m_doubleSided[index] != 0;
    }
    // Host copy of a material's alpha mode (the draw table's kDrawMask / kDrawTransparent, the
    // TLAS instances' masks and flags); a transmissive opaque material reads as Blend.
    [[nodiscard]] AlphaMode alphaMode(uint32_t index) const noexcept {
        return index < m_alphaMode.size() ? static_cast<AlphaMode>(m_alphaMode[index]) : AlphaMode::Opaque;
    }
    // Host copy of whether a material emits (emission colour x luminance above 0): its triangles
    // are the path tracer's emissive lights (render/rt/emissive_lights.hpp).
    [[nodiscard]] bool emissive(uint32_t index) const noexcept {
        return index < m_emissive.size() && m_emissive[index] != 0;
    }
    // Whether any material has OpenPBR's layers (a coat, a fuzz, specular anisotropy, a thin film or
    // transmission). A frame without them runs RT direct light's shading without the layers' code
    // (its registers).
    [[nodiscard]] bool anyLayered() const noexcept { return m_layered > 0; }
    [[nodiscard]] static bool layered(const MaterialDesc& desc) noexcept {
        return desc.coatWeight > 0.0f || desc.fuzzWeight > 0.0f || desc.specularAnisotropy > 0.0f ||
               desc.thinFilmWeight > 0.0f || desc.transmissionWeight > 0.0f;
    }
    [[nodiscard]] static Pack pack(const MaterialDesc& desc);

    [[nodiscard]] VkDeviceSize pendingBytes() const noexcept { return m_dirty.size() * sizeof(Pack); }
    void recordUploads(detail::StagingBatch& batch);   // one copy per contiguous dirty run

    [[nodiscard]] VkBuffer buffer() const noexcept { return m_buffer.buffer; }

private:
    GpuAllocator* m_allocator{nullptr};
    GpuBuffer m_buffer;
    std::vector<Pack> m_packs;
    std::vector<uint8_t> m_doubleSided;   // host only: the draw table's face-culling flag
    std::vector<uint8_t> m_alphaMode;     // host only: AlphaMode
    std::vector<uint8_t> m_emissive;      // host only: emits
    uint32_t m_layered{0};                // materials with layers (layered())
    std::set<uint32_t> m_dirty;
};

} // namespace ohao::render
