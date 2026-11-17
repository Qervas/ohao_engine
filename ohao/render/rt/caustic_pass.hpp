#pragma once

// Caustics' light pass on the GPU (sub-project 24, docs/specs/2026-11-03-caustics-design.md): the
// casters' table (render/rt/caustic_casters.hpp, packed as rt/caustics.slang reads it), the splat
// buffer the light pass adds to and raygenCaustics reads and clears (3 words a pixel, fixed point at
// kCausticSplatScale: integer sums, so the order the GPU adds in never changes a bit), and a count of
// splats that did not fit (clamped; 0 on every test scene).
//
// A frame with caustics: setScene (the host's casters, every frame; uploaded only when they change),
// recordBeginFrame (that upload, a pending clear), the tracer dispatches causticTrace (lightPass), then
// raygenCaustics (cameraPass).
//
// Until first used (ensure), the splat buffer is a placeholder of one element; the table and the
// counter are small and always there.

#include <vulkan/vulkan.h>

#include "gpu/vulkan/gpu_allocator.hpp"
#include "render/graph/pass.hpp"
#include "render/rt/caustic_casters.hpp"

#include <array>
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
    bool create(VkDevice device, GpuAllocator& allocator);
    void destroy();

    // This frame's casters and directional lights from the scene: smooth-capable draws (a material
    // whose lobe can be under `cut`), every directional light, the scene's bounding sphere.
    void setScene(const RenderScene& scene, float cut);
    // Something to trace: a directional light and a caster.
    [[nodiscard]] bool active() const noexcept { return m_casters.totalPower > 0.0f; }
    [[nodiscard]] uint32_t casterCount() const noexcept { return m_casterCount; }

    // The splat buffer for `pixels` pixels (the one it replaces is retired). False: the pass stays off.
    bool ensure(uint32_t pixels);

    // The table's upload when the casters changed, and the pending clears.
    void recordBeginFrame(const Frame& frame);
    // What the traces declare: the light pass (causticTrace) reads the table, adds to the splats and counts the
    // clamped ones; the camera pass (raygenCaustics) reads and clears the splats; the manifold pass
    // (its rounds, render/rt/manifold_rounds.hpp) reads the table and counts.
    void lightPass(Pass& pass);
    void cameraPass(Pass& pass);
    void manifoldPass(Pass& pass);

    // Splats that did not fit their word since the last clear (a reset).
    [[nodiscard]] uint32_t clamped() const;
    // The manifold pass's counters since the last clear (words 1-11 of the stats): requests, triangles solved, roots,
    // connections that carried light, the pruning's cluster, sub-cluster and triangle tests, and step 2b's walks, paths
    // found, connections through solids and paths at the re-find trials' ceiling.
    [[nodiscard]] std::array<uint32_t, 11> manifoldCounts() const;
    // The manifold rounds' words since the last clear (words 12-21): items that did not fit their queue, fixed-point
    // words clamped, two unused, then the peak counts of the records, groups, clusters, sub-clusters, candidates and
    // roots in a frame (render/rt/manifold_rounds.hpp).
    [[nodiscard]] std::array<uint32_t, 10> manifoldRoundCounts() const;
    void clearCounts() noexcept { m_clearCounts = true; }

    [[nodiscard]] VkBuffer table() const noexcept { return m_table.buffer; }
    [[nodiscard]] VkBuffer splat() const noexcept { return m_splat.buffer; }
    [[nodiscard]] VkBuffer stats() const noexcept { return m_stats.buffer; }

    // The packed table (float4 rows), as rt/caustics.slang reads it; at most kRows rows.
    static constexpr uint32_t kRows = 4096;
    // The stats: word 0 the splats clamped, 1-11 the manifold pass's counters, 12-21 its rounds'.
    static constexpr uint32_t kStatsBytes = 128;
    [[nodiscard]] static std::vector<glm::vec4> pack(const CausticCasters& c, glm::vec3 sceneCenter, float sceneRadius);

private:
    bool allocate(GpuBuffer& b, VkDeviceSize size, bool hostVisible);
    // The counters as the GPU last wrote them, into `out` (false without the buffer).
    bool readStats(void* out, VkDeviceSize offset, VkDeviceSize bytes) const;

    VkDevice m_device{VK_NULL_HANDLE};
    GpuAllocator* m_allocator{nullptr};
    GpuBuffer m_table, m_splat, m_stats;
    uint32_t m_pixels{0};
    bool m_splatFull{false};
    bool m_clearSplat{true};
    bool m_clearCounts{true};
    CausticCasters m_casters;
    uint32_t m_casterCount{0};
    std::vector<glm::vec4> m_packed, m_uploaded;
};

} // namespace ohao::render
