#pragma once

#include <vulkan/vulkan.h>
#include <memory>
#include <vector>
#include <functional>
#include <glm/glm.hpp>

namespace ohao {

class ShaderManager;
class ComputeShader;
class OhaoVkDevice;
class OhaoVkBuffer;
struct ShaderDefines;

// Compute dispatch information
struct ComputeDispatchInfo {
    glm::uvec3 groupCount{1, 1, 1};
    glm::uvec3 workGroupSize{1, 1, 1};
    bool useIndirect{false};
    VkBuffer indirectBuffer{VK_NULL_HANDLE};
    VkDeviceSize indirectOffset{0};
};

// Compute resource binding
struct ComputeResourceBinding {
    uint32_t set{0};
    uint32_t binding{0};
    VkDescriptorType type{VK_DESCRIPTOR_TYPE_MAX_ENUM};
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceSize offset{0};
    VkDeviceSize range{VK_WHOLE_SIZE};
    VkImageView imageView{VK_NULL_HANDLE};
    VkSampler sampler{VK_NULL_HANDLE};
    VkDescriptorBufferInfo bufferInfo{};
    VkDescriptorImageInfo imageInfo{};
};

// Compute pipeline state
class ComputePipelineState {
public:
    ComputePipelineState(std::shared_ptr<ComputeShader> shader);
    ~ComputePipelineState();
    
    // Resource binding
    void bindBuffer(uint32_t set, uint32_t binding, VkBuffer buffer, 
                   VkDeviceSize offset = 0, VkDeviceSize range = VK_WHOLE_SIZE);
    void bindStorageBuffer(uint32_t set, uint32_t binding, VkBuffer buffer,
                          VkDeviceSize offset = 0, VkDeviceSize range = VK_WHOLE_SIZE);
    void bindUniformBuffer(uint32_t set, uint32_t binding, VkBuffer buffer,
                          VkDeviceSize offset = 0, VkDeviceSize range = VK_WHOLE_SIZE);
    void bindImage(uint32_t set, uint32_t binding, VkImageView imageView, 
                  VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL);
    void bindSampler(uint32_t set, uint32_t binding, VkSampler sampler);
    void bindCombinedImageSampler(uint32_t set, uint32_t binding, 
                                 VkImageView imageView, VkSampler sampler,
                                 VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    
    // Push constants
    template<typename T>
    void setPushConstants(const T& data, uint32_t offset = 0) {
        pushConstantData.resize(sizeof(T));
        std::memcpy(pushConstantData.data(), &data, sizeof(T));
        pushConstantOffset = offset;
        pushConstantSize = sizeof(T);
    }
    
    // Shader variant selection
    void setShaderDefines(const ShaderDefines& defines);
    const ShaderDefines& getShaderDefines() const { return *shaderDefines; }
    
    // Dispatch configuration
    void setDispatchSize(uint32_t groupCountX, uint32_t groupCountY = 1, uint32_t groupCountZ = 1);
    void setDispatchSize(const glm::uvec3& groupCount);
    void setIndirectDispatch(VkBuffer buffer, VkDeviceSize offset = 0);
    
    // Execution
    bool dispatch(VkCommandBuffer cmd, OhaoVkDevice* device);
    
    // State validation
    bool isValid() const;
    
private:
    std::shared_ptr<ComputeShader> computeShader;
    std::unique_ptr<ShaderDefines> shaderDefines;
    
    // Resource bindings
    std::vector<ComputeResourceBinding> resourceBindings;
    
    // Push constants
    std::vector<uint8_t> pushConstantData;
    uint32_t pushConstantOffset{0};
    uint32_t pushConstantSize{0};
    
    // Dispatch info
    ComputeDispatchInfo dispatchInfo;
    
    // Vulkan objects
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
    bool descriptorSetDirty{true};
    
    // Helper functions
    bool updateDescriptorSet(OhaoVkDevice* device);
    void clearResourceBindings();
};

// Compute command encoder for batch compute operations
class ComputeCommandEncoder {
public:
    ComputeCommandEncoder(VkCommandBuffer commandBuffer, OhaoVkDevice* device);
    ~ComputeCommandEncoder();
    
    // Pipeline state management
    void setPipelineState(std::shared_ptr<ComputePipelineState> state);
    
    // Memory barriers
    void memoryBarrier(VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                      VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    
    void bufferBarrier(VkBuffer buffer, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                      VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE,
                      VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    
    void imageBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                     VkImageSubresourceRange subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                     VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    
    // Dispatch operations
    void dispatch(uint32_t groupCountX, uint32_t groupCountY = 1, uint32_t groupCountZ = 1);
    void dispatchIndirect(VkBuffer buffer, VkDeviceSize offset = 0);
    
    // Debug utilities
    void insertDebugLabel(const std::string& label);
    void beginDebugRegion(const std::string& name);
    void endDebugRegion();
    
private:
    VkCommandBuffer commandBuffer;
    OhaoVkDevice* device;
    std::shared_ptr<ComputePipelineState> currentPipelineState;
};

// High-level compute manager for common operations
class ComputeManager {
public:
    ComputeManager();
    ~ComputeManager();
    
    bool initialize(OhaoVkDevice* device, ShaderManager* shaderManager);
    void cleanup();
    
    // Predefined compute operations
    bool dispatchParticleSimulation(VkCommandBuffer cmd,
                                   VkBuffer particleBuffer,
                                   VkBuffer forceBuffer,
                                   uint32_t particleCount,
                                   float deltaTime);
    
    bool dispatchFrustumCulling(VkCommandBuffer cmd,
                               VkBuffer objectBuffer,
                               VkBuffer visibleBuffer,
                               uint32_t objectCount,
                               const glm::mat4& viewMatrix,
                               const glm::mat4& projMatrix);
    
    bool dispatchShadowGeneration(VkCommandBuffer cmd,
                                 VkImageView shadowMap,
                                 VkImageView depthTexture,
                                 const glm::mat4& lightViewProjMatrix,
                                 const glm::uvec2& shadowMapSize);
    
    // Generic compute dispatch
    std::shared_ptr<ComputePipelineState> createPipelineState(const std::string& computeShaderName);
    bool dispatch(VkCommandBuffer cmd, std::shared_ptr<ComputePipelineState> state);
    
    // Utility functions
    glm::uvec3 calculateOptimalWorkGroups(const glm::uvec3& totalWork, const glm::uvec3& workGroupSize);
    uint32_t calculateOptimalWorkGroupSize(uint32_t totalWork, uint32_t maxWorkGroupSize = 64);
    
private:
    OhaoVkDevice* device{nullptr};
    ShaderManager* shaderManager{nullptr};
    
    // Cached compute shaders for common operations
    std::shared_ptr<ComputeShader> particleSimulationShader;
    std::shared_ptr<ComputeShader> frustumCullingShader;
    std::shared_ptr<ComputeShader> shadowGenerationShader;
    
    // Pipeline states
    std::shared_ptr<ComputePipelineState> particleSimulationState;
    std::shared_ptr<ComputePipelineState> frustumCullingState;
    std::shared_ptr<ComputePipelineState> shadowGenerationState;
    
    // Initialization helpers
    bool initializeCommonShaders();
    bool createCommonPipelineStates();
};

} // namespace ohao