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

    // Get the skinned position buffer (for BLAS rebuild input)
    VkBuffer getSkinnedPositionBuffer(uint32_t meshHandle) const;
    VkBuffer getSkinnedNormalBuffer(uint32_t meshHandle) const;
    uint32_t getVertexCount(uint32_t meshHandle) const;

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

    struct SkinPushConstants {
        uint32_t vertexCount;
        uint32_t vertexStride;  // in floats
        uint32_t vertexOffset;  // offset in vertices into the source buffer
    };
};

} // namespace ohao
