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
//
// A material is owned when a lobe of it can be smooth, it has no normal map (step 2c: the light pass carries a mapped
// reflector's glints; docs/plans/2026-11-08-caustics-step2c.md, task 7) and every draw using it is gathered (static, not
// an emitter or a first-person gun). The camera side subtracts the core of a smooth reflection only at an owned
// material, and the pass connects only through owned draws: the split stays exact (ruling M7).

#include <vulkan/vulkan.h>

#include "gpu/vulkan/gpu_allocator.hpp"
#include "render/frame/frame_resources.hpp"
#include "render/graph/pass.hpp"
#include "render/rt/caustic_casters.hpp"

#include <array>
#include <cstdint>

namespace ohao::render {

class RenderScene;

class ManifoldCasters {
public:
    bool create(VkDevice device, GpuAllocator& allocator);
    void destroy();

    // This frame's owned draws from the scene, into this frame slot's draws buffer (grows the device buffers if
    // needed: the ones they replace are retired).
    void prepare(uint32_t frameSlot, const RenderScene& scene, float cut);
    [[nodiscard]] bool active() const noexcept { return m_triangles > 0; }
    [[nodiscard]] uint32_t triangles() const noexcept { return m_triangles; }
    [[nodiscard]] uint32_t drawCount() const noexcept { return m_drawCount; }

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

private:
    bool allocate(GpuBuffer& b, VkDeviceSize size);

    VkDevice m_device{VK_NULL_HANDLE};
    GpuAllocator* m_allocator{nullptr};
    GpuBuffer m_tris, m_nodes, m_requests;
    VkDeviceSize m_trisBytes{0}, m_nodesBytes{0};   // what the two were asked to hold
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

} // namespace ohao::render
