#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace ohao {

// Indirect draw command for indexed drawing (VkDrawIndexedIndirectCommand)
struct IndirectDrawCommand {
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t vertexOffset;
    uint32_t firstInstance;
};

// Per-draw instance data (object transforms, material indices, etc.)
struct DrawInstance {
    glm::mat4 modelMatrix;
    glm::mat4 normalMatrix;
    uint32_t materialIndex;
    uint32_t meshIndex;
    uint32_t flags;
    uint32_t padding;
};

// Mesh descriptor for GPU culling
struct MeshDescriptor {
    glm::vec4 boundingSphere;  // xyz = center, w = radius
    glm::vec4 aabbMin;         // xyz = min, w = unused
    glm::vec4 aabbMax;         // xyz = max, w = unused
    uint32_t indexOffset;
    uint32_t indexCount;
    uint32_t vertexOffset;
    uint32_t materialIndex;
};

// GPU-driven draw call batch
struct DrawBatch {
    uint32_t drawCount;
    uint32_t firstDraw;
    uint32_t materialBindGroup;
    uint32_t padding;
};

// GPU Indirect Draw Buffer
// Manages GPU-visible buffers for indirect draw commands and instance data
class IndirectDrawBuffer {
public:
    IndirectDrawBuffer() = default;
    ~IndirectDrawBuffer();

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t maxDraws = 65536);
    void cleanup();

    // Clear all draws for new frame
    void reset();

    // Add a draw call
    uint32_t addDraw(const IndirectDrawCommand& cmd, const DrawInstance& instance);

    // Add multiple draws at once
    void addDraws(const std::vector<IndirectDrawCommand>& cmds,
                  const std::vector<DrawInstance>& instances);

    // Upload CPU data to GPU
    void upload(VkCommandBuffer cmd);

    // Get buffers for binding
    VkBuffer getCommandBuffer() const { return m_commandBuffer; }
    VkBuffer getInstanceBuffer() const { return m_instanceBuffer; }
    VkBuffer getDrawCountBuffer() const { return m_drawCountBuffer; }

    // Get draw count (on CPU)
    uint32_t getDrawCount() const { return m_drawCount; }

    // GPU-side draw count offset (for indirect count)
    VkDeviceSize getDrawCountOffset() const { return 0; }

    // For descriptor binding
    VkDescriptorBufferInfo getCommandBufferInfo() const {
        return {m_commandBuffer, 0, m_commandBufferSize};
    }

    VkDescriptorBufferInfo getInstanceBufferInfo() const {
        return {m_instanceBuffer, 0, m_instanceBufferSize};
    }

private:
    bool createBuffer(VkBuffer& buffer, VkDeviceMemory& memory,
                      VkDeviceSize size, VkBufferUsageFlags usage);
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};

    // GPU buffers
    VkBuffer m_commandBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_commandMemory{VK_NULL_HANDLE};
    VkDeviceSize m_commandBufferSize{0};

    VkBuffer m_instanceBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_instanceMemory{VK_NULL_HANDLE};
    VkDeviceSize m_instanceBufferSize{0};

    VkBuffer m_drawCountBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_drawCountMemory{VK_NULL_HANDLE};

    // Staging buffers for CPU->GPU upload
    VkBuffer m_stagingCommandBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_stagingCommandMemory{VK_NULL_HANDLE};
    void* m_stagingCommandMapped{nullptr};

    VkBuffer m_stagingInstanceBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_stagingInstanceMemory{VK_NULL_HANDLE};
    void* m_stagingInstanceMapped{nullptr};

    VkBuffer m_stagingCountBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_stagingCountMemory{VK_NULL_HANDLE};
    void* m_stagingCountMapped{nullptr};

    // CPU-side data
    std::vector<IndirectDrawCommand> m_cpuCommands;
    std::vector<DrawInstance> m_cpuInstances;
    uint32_t m_drawCount{0};
    uint32_t m_maxDraws{0};
};

// GPU Frustum Culling Pass
// Culls objects on the GPU using compute shaders
class GpuCullPass {
public:
    GpuCullPass() = default;
    ~GpuCullPass();

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice);
    void cleanup();

    // Set input mesh descriptors
    void setMeshDescriptors(VkBuffer descriptorBuffer, uint32_t meshCount);

    // Set camera matrices for frustum culling
    void setCameraData(const glm::mat4& viewProj, const glm::vec3& cameraPos);

    // Execute culling pass
    void execute(VkCommandBuffer cmd, IndirectDrawBuffer& drawBuffer);

    // Configuration
    void setOcclusionCullingEnabled(bool enabled) { m_occlusionCulling = enabled; }
    void setFrustumCullingEnabled(bool enabled) { m_frustumCulling = enabled; }
    void setDistanceCullingEnabled(bool enabled) { m_distanceCulling = enabled; }
    void setMaxDistance(float distance) { m_maxDistance = distance; }

private:
    bool createDescriptors();
    bool createPipeline();

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};

    VkPipeline m_cullPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_descriptorSet{VK_NULL_HANDLE};

    // Culling settings
    bool m_frustumCulling{true};
    bool m_occlusionCulling{false};  // Requires Hi-Z
    bool m_distanceCulling{true};
    float m_maxDistance{1000.0f};

    // Camera data
    glm::mat4 m_viewProj;
    glm::vec3 m_cameraPos;

    // Mesh data
    VkBuffer m_meshDescriptorBuffer{VK_NULL_HANDLE};
    uint32_t m_meshCount{0};

    // Push constants
    struct CullPushConstants {
        glm::mat4 viewProj;
        glm::vec4 cameraPos;     // xyz = position, w = maxDistance
        glm::vec4 frustumPlanes[6];
        uint32_t meshCount;
        uint32_t flags;          // Bit 0: frustum, Bit 1: occlusion, Bit 2: distance
        uint32_t padding[2];
    };
};

} // namespace ohao
