#pragma once
#include "render/scene/render_scene_types.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"

#include <optional>
#include <vector>

namespace ohao::render {


// What the camera sees of a light: RenderScene draws it as an emitter instance
// (kInstanceEmitter), pure emission that only the G-buffer, camera rays and a near-mirror's
// first specular ray see.
struct LightEmitter {
    bool quad{false};          // the unit quad ([0,1]^2 at z = 0, facing +z); else the unit sphere
    glm::mat4 world{1.0f};
    glm::vec3 radiance{0.0f};  // emitted, on the front side
};
// A point or spot light is a sphere of its radius with radiance I / (pi r^2): seen from afar it
// is a source of intensity I, the I / d^2 both renderers light with. A spot light's sphere
// shines toward `viewer` as the light does, times its cone factor there (the path tracer's
// profile: linear from the outer cone, 0, to the inner, 1); without a viewer, on-axis. An area light is its
// rectangle (position + u edge1 + v edge2, emitting towards edge1 x edge2) at the path tracer's
// radiance for it, I / (4 pi r^2). Radii below the path tracer's 0.01 m are raised to it.
// Directional lights and emissive proxies (whose mesh already emits) have none, nor a point or
// spot light of radius 0 or a degenerate rectangle.
[[nodiscard]] std::optional<LightEmitter> lightEmitter(const LightDesc& light,
                                                       const std::optional<glm::vec3>& viewer = std::nullopt);

// Light table: the light list resolved once per setLights into both consumers'
// layouts -- the raster LightData array (host copy, <= MAX_LIGHTS = 65,536, for clustered
// lighting's light buffers) and the RT GPULight buffer (device-local, 16-byte header + <= 64
// lights).
class LightTable {
public:
    static constexpr VkDeviceSize kHeaderBytes = 16;

    [[nodiscard]] bool initialize(GpuAllocator& allocator);
    void shutdown();

    void set(std::span<const LightDesc> lights);
    void setEnvMapTextureIndex(uint32_t texIdx);
    // Whether a TLAS instance is non-opaque (an alpha material): the path tracer's rays then stop
    // forcing opacity (header word 3).
    void setAlphaRays(bool on);
    // The RT lights (indices in the RT buffer) that have an emitter mesh a bounce ray can meet:
    // rt/path_tracer.slang weighs their light sampling against BSDF sampling, and samples the rest
    // at full weight. Flagged at upload (GPULight extra2.z for a point or spot, extra.w for a
    // rectangle).
    void setEmitterLights(std::vector<uint32_t> rtIndices);
    void setEnvIntensity(float scale);   // header word 2, read by pt_miss (<= 0 means 1)

    [[nodiscard]] std::span<const LightData> raster() const noexcept { return m_raster; }
    // Changes whenever raster() does (from 1: 0 means "always upload" to LightClusters).
    [[nodiscard]] uint64_t rasterVersion() const noexcept { return m_rasterVersion; }
    // Shadow-casting raster lights past the kMaxShadowLights channels: lit unshadowed.
    [[nodiscard]] uint32_t shadowCastersWithoutChannel() const noexcept { return m_shadowCastersWithoutChannel; }
    // The last set() list, sorted by order (host copy).
    [[nodiscard]] std::span<const LightDesc> descs() const noexcept { return m_descs; }
    // The lights RT shadows trace, <= kMaxShadowLights: element i is shadow-mask channel i.
    [[nodiscard]] std::span<const LightDesc> shadowLights() const noexcept { return m_shadowLights; }
    [[nodiscard]] uint32_t rtCount() const noexcept { return static_cast<uint32_t>(m_rt.size()); }
    [[nodiscard]] VkBuffer rtBuffer() const noexcept { return m_rtBuffer.buffer; }

    [[nodiscard]] VkDeviceSize pendingBytes() const noexcept;
    std::size_t recordUploads(detail::StagingBatch& batch);   // returns bytes uploaded

private:
    GpuAllocator* m_allocator{nullptr};
    GpuBuffer m_rtBuffer;
    std::vector<LightDesc> m_descs;
    std::vector<LightData> m_raster;
    uint64_t m_rasterVersion{1};
    uint32_t m_shadowCastersWithoutChannel{0};
    bool m_capLogged{false};
    std::vector<LightDesc> m_shadowLights;
    std::vector<GPULight> m_rt;
    uint32_t m_envMapTexIdx{kNoTexture};
    float m_envIntensity{1.0f};
    bool m_alphaRays{false};
    std::vector<uint32_t> m_emitterLights;
    bool m_dirty{true};
};

} // namespace ohao::render
