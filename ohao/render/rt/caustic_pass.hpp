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
    [[nodiscard]] uint32_t lightCount() const noexcept { return uint32_t(m_casters.lightIrradiance.size()); }

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
    // The manifold rounds' words since the last clear (words 12-22): items lost past the last pass, fixed-point words
    // clamped, items carried to a later pass, the passes that held an item (the most), then the peak counts in a batch
    // and pass of the records and of the records' items, groups, clusters, sub-clusters, candidates and roots
    // (render/rt/manifold_rounds.hpp).
    [[nodiscard]] std::array<uint32_t, 11> manifoldRoundCounts() const;
    // Step 2c's (words 23-29): the peak pyramid nodes and cells in a batch and pass, cells reached, pyramid tests,
    // mapped triangles queued, records on a mapped draw, mapped triangles with degenerate uvs.
    [[nodiscard]] std::array<uint32_t, 7> manifoldMappedCounts() const;
    // The solve's (words 2112-2117): tests run, items still open after the last pass, undecided boxes, roots in cells,
    // roots past those an item keeps, items started over for want of a state (rt/path_tracer.slang, manifoldSolveRound).
    [[nodiscard]] std::array<uint32_t, 6> manifoldSolveCounts() const;
    // Step 2c's dump (OHAO_MANIFOLD_DUMP=x,y, written at destroy to OHAO_MANIFOLD_DUMP_FILE): the stats grown to hold
    // it (rt/path_tracer.slang, kManifoldDump).
    [[nodiscard]] bool dumping() const noexcept { return m_dumping; }
    void clearCounts() noexcept { m_clearCounts = true; }

    [[nodiscard]] VkBuffer table() const noexcept { return m_table.buffer; }
    [[nodiscard]] VkBuffer splat() const noexcept { return m_splat.buffer; }
    [[nodiscard]] VkBuffer stats() const noexcept { return m_stats.buffer; }

    // The packed table (float4 rows), as rt/caustics.slang reads it; at most kRows rows.
    static constexpr uint32_t kRows = 4096;
    // The stats: word 0 the splats clamped, 1-11 the manifold pass's counters, 12-24 its rounds', 25-29 step 2c's,
    // from word 32 the cell statistics' histograms (rt/path_tracer.slang, kManifoldHist), 2112-2117 the solve's; from
    // word 4096 the dump.
    static constexpr uint32_t kStatsBytes = 4096 * 4;
    // rt/path_tracer.slang's kManifoldDumpSolved + 32 x kManifoldDumpSolvedCap
    static constexpr VkDeviceSize kDumpBytes = (19927168ull + 32ull * 16384ull) * 4;
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
    bool m_dumping{false};
    CausticCasters m_casters;
    uint32_t m_casterCount{0};
    std::vector<glm::vec4> m_packed, m_uploaded;
};

} // namespace ohao::render
