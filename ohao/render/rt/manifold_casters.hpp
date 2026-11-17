#pragma once

// The manifold pass's casters (sub-project 24 step 2a, docs/specs/2026-11-04-caustics-step2-design.md, "Step 2a as
// designed"): the draws whose material the pass owns, their triangles gathered into world space on the GPU every frame
// it runs (rt/manifold_gather.slang: triangles, clusters and groups of kManifoldCluster with boxes and normal cones),
// a bit word a material, and the camera side's requests (kManifoldDepths a pixel). The path tracer's bindings:
//   54 tris     9 float4 a triangle slot, 128 a cluster (a meshlet; manifold_gather.slang)
//   55 nodes    4 float4 a node: the groups, the clusters, then a record a draw (its lobes' alpha bounds)
//   56 draws    row 0 (D, slots, C, G); three rows a draw (the third its world bounding sphere); then a word a
//               material (kManifoldOwned* bits)
//   57 requests 3 float4 a request: (origin, meta bits), (direction, 0), (throughput, 0); meta 0 = none
//   63 pyramid  step 2c: the normal maps' pyramids of balls (below)
//
// The pyramid (step 2c, docs/plans/2026-11-08-caustics-step2c.md). A normal map's cell is the square between four
// texel centres, where the tracer's normal is its four texels' decoded normals blended bilinearly (rt/pt_hit.slang,
// mapNormalAt): a tangent-space normal there lies in their convex hull. A leaf holds kManifoldPyramidLeaf x
// kManifoldPyramidLeaf cells and a ball around all of their texels' normals; each level up a ball around its 2 x 2
// children's (a tile's leaves padded to a power of two a side; a pad node's radius -1). Built on the GPU from the
// texels as the tracer Loads them (rt/path_tracer.slang, manifoldPyramidBuild), once a map (a view and size), the
// first frame a draw of it is owned. The buffer: kMapRows float4 a texture index (its directory: (W, H, Pw, Ph),
// (Lx | Ly << 8 | Lmax << 16, each level's first node...)), then the nodes.
//
// A material is owned when a lobe of it can be smooth, every draw using it is gathered (static, not an emitter or a
// first-person gun) and, if it has a normal map, the map has a pyramid and its walk is on (stage 2: only for measurement,
// OHAO_MANIFOLD_MAPS; manifold_casters.cpp). The camera side subtracts the core of a smooth
// reflection only at an owned material without a normal map (step 2c, stage 2: a mapped one's cells are walked and
// counted, not yet solved), and the pass connects only through owned draws: the split stays exact (ruling M7).

#include <vulkan/vulkan.h>

#include "gpu/vulkan/gpu_allocator.hpp"
#include "render/frame/frame_resources.hpp"
#include "render/graph/pass.hpp"
#include "render/rt/caustic_casters.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ohao::render {

class RenderScene;

class ManifoldCasters {
public:
    bool create(VkDevice device, GpuAllocator& allocator);
    void destroy();

    // This frame's owned draws from the scene, into this frame slot's draws buffer (grows the device buffers if
    // needed: the ones they replace are retired); `views` the bindless table's (a map's pyramid is its view's).
    void prepare(uint32_t frameSlot, const RenderScene& scene, float cut, std::span<const VkImageView> views);
    [[nodiscard]] bool active() const noexcept { return m_triangles > 0; }
    [[nodiscard]] uint32_t triangles() const noexcept { return m_triangles; }
    [[nodiscard]] uint32_t drawCount() const noexcept { return m_drawCount; }
    // Whether a gathered draw is normal-mapped (step 2c: its walk goes through the pyramid's rounds).
    [[nodiscard]] bool mapped() const noexcept { return m_mappedSlots > 0; }

    // The requests for `pixels` pixels (the ones they replace are retired). False: the pass stays off.
    bool ensureRequests(uint32_t pixels);

    // The gather from the scene's arenas, and the requests cleared on the first frame after they were
    // (re)allocated.
    void record(const Frame& frame, const RenderScene& scene);
    // What the passes declare: the camera pass (raygenCaustics) reads the casters and writes its requests; the
    // manifold pass's setup (manifoldSetup) reads the casters and their triangles, and answers and clears the
    // requests; its later rounds (render/rt/manifold_rounds.hpp) read the casters and their triangles.
    void cameraPass(Pass& pass, uint32_t frameSlot);
    void manifoldPass(Pass& pass, uint32_t frameSlot);
    void roundPass(Pass& pass, uint32_t frameSlot);

    [[nodiscard]] VkBuffer tris() const noexcept { return m_tris.buffer; }
    [[nodiscard]] VkBuffer nodes() const noexcept { return m_nodes.buffer; }
    [[nodiscard]] VkBuffer draws(uint32_t frameSlot) const noexcept { return m_draws[frameSlot].buffer; }
    [[nodiscard]] VkBuffer requests() const noexcept { return m_requests.buffer; }

    // Step 2c's pyramids: whether a map's waits to be built, and its build: the directory rows of every waiting map,
    // then `dispatch(tex, level, nodes)` a level each (the caller's pipeline bound: rt/path_tracer.slang's
    // manifoldPyramidBuild), a barrier between.
    [[nodiscard]] bool pyramidsPending() const noexcept;
    template <class Dispatch> void recordPyramids(const Frame& frame, Dispatch&& dispatch);
    [[nodiscard]] VkBuffer pyramid() const noexcept { return m_pyramid.buffer; }
    void pyramidPass(Pass& pass) { pass.read(m_pyramid, BufferUse::Storage); }

    static constexpr uint32_t kMapRows = 8;         // rt/path_tracer.slang's kManifoldMapRows
    static constexpr uint32_t kMapIndices = 4096;   // texture indices the directory holds
    static constexpr uint32_t kMaxLevels = 27;

private:
    struct PyramidMap {
        uint32_t tex{0}, width{0}, height{0};
        VkImageView view{VK_NULL_HANDLE};
        uint32_t pw{1}, ph{1}, lx{0}, ly{0}, lmax{0};
        std::array<uint32_t, kMaxLevels> offsets{};   // a level's first node, a float4 row of the buffer
        uint32_t nodes{0};
        bool built{false};
    };
    // Whether a map has a pyramid (laid out now when it had none; built at the next recordPyramids).
    bool ensurePyramid(uint32_t tex, uint32_t width, uint32_t height, VkImageView view);
    void recordPyramidDirectory(const Frame& frame);

    bool allocate(GpuBuffer& b, VkDeviceSize size);

    VkDevice m_device{VK_NULL_HANDLE};
    GpuAllocator* m_allocator{nullptr};
    GpuBuffer m_tris, m_nodes, m_requests;
    VkDeviceSize m_trisBytes{0}, m_nodesBytes{0};   // what the two were asked to hold
    GpuBuffer m_pyramid;
    VkDeviceSize m_pyramidBytes{0};
    std::vector<PyramidMap> m_maps;
    bool m_pyramidFailed{false};
    bool m_pyramidZero{true};   // a new buffer: its directory cleared before the first map is written
    uint32_t m_mappedSlots{0};   // the side table's slots (logged when it changes)
    std::array<GpuBuffer, MAX_FRAMES_IN_FLIGHT> m_draws{};
    std::array<VkDeviceSize, MAX_FRAMES_IN_FLIGHT> m_drawsBytes{};
    uint32_t m_triangles{0}, m_clusters{0}, m_groups{0}, m_drawCount{0};
    uint32_t m_requestPixels{0};
    bool m_requestsFull{false};
    bool m_growthFailed{false};   // the triangles or nodes could not grow: the pass stays off
    bool m_clearRequests{true};

    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkPipelineLayout m_layout{VK_NULL_HANDLE};
    // The groups pipeline's own layout (the same set as m_layout's): kept until destroy(), since NVIDIA's Linux driver
    // reads a pipeline's layout when it is bound and dispatched (destroyed early, the dispatch faulted or wrote nothing).
    VkPipelineLayout m_groupLayout{VK_NULL_HANDLE};
    VkPipeline m_clusterPipe{VK_NULL_HANDLE}, m_groupPipe{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_sets{};
};

template <class Dispatch> void ManifoldCasters::recordPyramids(const Frame& frame, Dispatch&& dispatch) {
    if (!pyramidsPending()) return;
    recordPyramidDirectory(frame);
    for (PyramidMap& m : m_maps) {
        if (m.built) continue;
        for (uint32_t l = 0; l <= m.lmax; ++l) {
            Pass pass(frame, nullptr, Pass::Compute);
            pass.readWrite(m_pyramid, BufferUse::Storage);
            pass.begin();
            dispatch(m.tex, l, std::max(m.pw >> l, 1u) * std::max(m.ph >> l, 1u));
        }
        m.built = true;
    }
}

} // namespace ohao::render
