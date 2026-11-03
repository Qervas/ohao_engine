#pragma once

// Caustics' light pass on the GPU (sub-project 24, docs/specs/2026-11-03-caustics-design.md): the
// casters' table (render/rt/caustic_casters.hpp, packed as rt/caustics.slang reads it), the splat
// buffer the light pass adds to and raygenCaustics reads and clears (3 words a pixel, fixed point at
// kCausticSplatScale: integer sums, so the order the GPU adds in never changes a bit), and a count of
// splats that did not fit (clamped; 0 on every test scene).
//
// A frame with caustics: setScene (the host's casters, every frame; uploaded only when they change),
// recordBeginFrame (that upload, a pending clear), the tracer dispatches causticTrace, recordAfterTrace
// (its splats ordered before raygen reads them), then raygenCaustics.
//
// Until first used (ensure), the splat buffer is a placeholder of one element; the table and the
// counter are small and always there.

#include <vulkan/vulkan.h>

#include "render/rt/caustic_casters.hpp"

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace ohao::render {

class RenderScene;

// The light pass's counters, once a frame has run it (render.stats `caustics`): the casters and the
// splats clamped since the last reset.
struct CausticsStats {
    uint32_t casters{0};
    uint32_t clamped{0};
};

class CausticPass {
public:
    bool create(VkDevice device, VkPhysicalDevice physicalDevice);
    void destroy();

    // This frame's casters and directional lights from the scene: smooth-capable draws (a material
    // whose lobe can be under `cut`), every directional light, the scene's bounding sphere.
    void setScene(const RenderScene& scene, float cut);
    // Something to trace: a directional light and a caster.
    [[nodiscard]] bool active() const noexcept { return m_casters.totalPower > 0.0f; }
    [[nodiscard]] uint32_t casterCount() const noexcept { return m_casterCount; }

    // The splat buffer for `pixels` pixels (idles the device when it (re)allocates). False: the pass
    // stays off.
    bool ensure(uint32_t pixels);

    void recordBeginFrame(VkCommandBuffer cmd);
    void recordAfterTrace(VkCommandBuffer cmd);

    // Splats that did not fit their word since the last clear (a reset).
    [[nodiscard]] uint32_t clamped() const;
    void clearCounts() noexcept { m_clearCounts = true; }

    [[nodiscard]] VkBuffer table() const noexcept { return m_table.buffer; }
    [[nodiscard]] VkBuffer splat() const noexcept { return m_splat.buffer; }
    [[nodiscard]] VkBuffer stats() const noexcept { return m_stats.buffer; }

    // The packed table (float4 rows), as rt/caustics.slang reads it; at most kRows rows.
    static constexpr uint32_t kRows = 4096;
    [[nodiscard]] static std::vector<glm::vec4> pack(const CausticCasters& c, glm::vec3 sceneCenter, float sceneRadius);

private:
    struct Buffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkDeviceSize size{0};
        void* mapped{nullptr};
    };
    bool allocate(Buffer& b, VkDeviceSize size, bool hostVisible);
    void release(Buffer& b);

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    Buffer m_table, m_splat, m_stats;
    uint32_t m_pixels{0};
    bool m_splatFull{false};
    bool m_clearSplat{true};
    bool m_clearCounts{true};
    CausticCasters m_casters;
    uint32_t m_casterCount{0};
    std::vector<glm::vec4> m_packed, m_uploaded;
};

} // namespace ohao::render
