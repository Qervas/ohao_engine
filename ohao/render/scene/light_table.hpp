#pragma once
#include "render/scene/render_scene_types.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"

#include <vector>

namespace ohao::render {

// Light-space matrix of a raster light: ortho for directional (type 0), perspective
// for spot (type 2), identity otherwise. Shared with VulkanRenderer's fallback sun.
[[nodiscard]] glm::mat4 lightSpaceMatrix(const LightData& light);

// Light table: the light list resolved once per setLights into both consumers'
// layouts -- the raster LightData array (host copy, <= MAX_LIGHTS, for the light
// UBO) and the RT GPULight buffer (device-local, 16-byte header + <= 64 lights).
class LightTable {
public:
    static constexpr VkDeviceSize kHeaderBytes = 16;

    [[nodiscard]] bool initialize(GpuAllocator& allocator);
    void shutdown();

    void set(std::span<const LightDesc> lights);
    void setEnvMapTextureIndex(uint32_t texIdx);
    void setEnvIntensity(float scale);   // header word 2, read by pt_miss (<= 0 means 1)

    [[nodiscard]] std::span<const LightData> raster() const noexcept { return m_raster; }
    [[nodiscard]] uint32_t rtCount() const noexcept { return static_cast<uint32_t>(m_rt.size()); }
    [[nodiscard]] VkBuffer rtBuffer() const noexcept { return m_rtBuffer.buffer; }

    [[nodiscard]] VkDeviceSize pendingBytes() const noexcept;
    std::size_t recordUploads(detail::StagingBatch& batch);   // returns bytes uploaded

private:
    GpuAllocator* m_allocator{nullptr};
    GpuBuffer m_rtBuffer;
    std::vector<LightData> m_raster;
    std::vector<GPULight> m_rt;
    uint32_t m_envMapTexIdx{kNoTexture};
    float m_envIntensity{1.0f};
    bool m_shadowsEnabled{true};
    bool m_dirty{true};
};

} // namespace ohao::render
