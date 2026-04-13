#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace ohao {

class RTAccelerationStructure;

// GPU compute skinning — transforms bind-pose vertices with bone matrices
// and rebuilds BLAS for animated meshes each frame.
class GPUSkinning {
public:
    GPUSkinning() = default;
    ~GPUSkinning();

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    VkCommandPool commandPool, VkQueue queue);
    void cleanup();

    // Register an animated mesh: creates skinned output buffer + compute descriptors.
    // srcVertexBuffer: the original bind-pose vertices (SSBO-compatible)
    // vertexCount: number of vertices
    // Returns a handle for update/query.
    uint32_t registerMesh(VkBuffer srcVertexBuffer, uint32_t vertexCount,
                          uint32_t vertexOffset = 0);

    // Run compute skinning for a registered mesh with the given bone matrices.
    // Outputs deformed positions to the skinned position buffer.
    void skin(VkCommandBuffer cmd, uint32_t meshHandle,
              const std::vector<glm::mat4>& boneMatrices);

    // Get the GLOBAL skinned position buffer (all meshes write to their offset)
    // BLAS builds reference this with per-mesh byte offsets, just like the original RT vertex buffer.
    VkBuffer getGlobalSkinnedPositionBuffer() const { return m_globalPosBuffer; }

    // Create the global skinned position buffer (call once after all meshes registered)
    void createGlobalBuffer(uint32_t totalVertexCount);

    // Get per-mesh info
    uint32_t getVertexCount(uint32_t meshHandle) const;
    uint32_t getVertexOffset(uint32_t meshHandle) const;

private:
    bool createComputePipeline();

    struct SkinEntry {
        VkBuffer srcVertexBuffer{VK_NULL_HANDLE};     // original vertices (read-only)
        VkBuffer skinnedPosBuffer{VK_NULL_HANDLE};    // output positions (3 floats/vert)
        VkDeviceMemory skinnedPosMem{VK_NULL_HANDLE};
        VkBuffer skinnedNormBuffer{VK_NULL_HANDLE};   // output normals (3 floats/vert)
        VkDeviceMemory skinnedNormMem{VK_NULL_HANDLE};
        VkBuffer boneUBO{VK_NULL_HANDLE};             // per-mesh bone matrices
        VkDeviceMemory boneUBOMem{VK_NULL_HANDLE};
        void* boneUBOMapped{nullptr};
        VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
        uint32_t vertexCount{0};
        uint32_t vertexOffset{0};
    };

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    VkCommandPool m_commandPool{VK_NULL_HANDLE};
    VkQueue m_queue{VK_NULL_HANDLE};

    // Compute pipeline
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};

    std::vector<SkinEntry> m_entries;

    // Global skinned position buffer (all meshes packed together)
    VkBuffer m_globalPosBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_globalPosMem{VK_NULL_HANDLE};
    uint32_t m_totalVertexCount{0};

    struct SkinPushConstants {
        uint32_t vertexCount;
        uint32_t vertexStride;
        uint32_t vertexOffset;  // input offset in vertices
        uint32_t outputOffset;  // output offset in vertices (into global buffer)
    };
};

} // namespace ohao
