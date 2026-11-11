#pragma once
// The skin and skin.bounds passes (sub-project 10, shaders/skin/skin.slang): a skinned instance's
// pose into its own mesh, and its meshlet spheres. RenderScene owns one; recordSkinning drives it.
// Palettes are host-written into a buffer per frame slot (the slot's fence guarantees the GPU is
// done with it before the host rewrites it).
#include "gpu/vulkan/gpu_allocator.hpp"
#include "render/frame/frame_resources.hpp"
#include "render/graph/pass.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ohao::render {

class SkinPass {
public:
    struct Job {
        uint32_t baseVertex{0}, vertex{0}, vertexCount{0};
        uint32_t firstMeshlet{0}, meshletCount{0};
        std::span<const glm::mat4> current, previous;   // the same length (the mesh's bones)
    };

    [[nodiscard]] bool initialize(VkDevice device, GpuAllocator& allocator);
    void shutdown();
    [[nodiscard]] bool ready() const noexcept { return m_skin != VK_NULL_HANDLE && m_bounds != VK_NULL_HANDLE; }

    // Writes the jobs' palettes into the slot's buffer (grown if needed: the slot's fence was
    // waited) and binds the arenas. False when nothing can be recorded.
    [[nodiscard]] bool prepare(uint32_t slot, std::span<const Job> jobs, GpuBuffer& vertexArena, GpuBuffer& meshletArena,
                               GpuBuffer& meshletVertexArena);
    // The skin dispatches, one a job: each reads its base's vertices and writes the instance's own range of the
    // arena (skin). Then the meshlets' spheres over the deformed vertices (skin.bounds).
    void recordSkin(const Frame& frame, std::span<const Job> jobs);
    void recordBounds(const Frame& frame, std::span<const Job> jobs);

private:
    struct Slot {
        GpuBuffer palettes;
        VkDeviceSize capacity{0};   // bytes
        VkDescriptorSet set{VK_NULL_HANDLE};
        std::vector<uint32_t> offsets;   // per job: its current palette's first matrix, then its previous
    };

    VkDevice m_device{VK_NULL_HANDLE};
    GpuAllocator* m_allocator{nullptr};
    GpuBuffer* m_vertices{nullptr};          // prepare()'s arenas, for the frame's passes
    GpuBuffer* m_meshlets{nullptr};
    GpuBuffer* m_meshletVertices{nullptr};
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    VkPipelineLayout m_skinLayout{VK_NULL_HANDLE}, m_boundsLayout{VK_NULL_HANDLE};
    VkPipeline m_skin{VK_NULL_HANDLE}, m_bounds{VK_NULL_HANDLE};
    std::array<Slot, MAX_FRAMES_IN_FLIGHT> m_slots{};
};

} // namespace ohao::render
