#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <array>
#include <vector>
#include <string>
#include <string_view>
#include <memory>
#include <concepts>

namespace ohao {

class GpuAllocator;

// Forward declarations
class Scene;
struct FrameResources;

// Base class for all render passes
class RenderPassBase {
public:
    virtual ~RenderPassBase() = default;

    [[nodiscard]] bool isInitialized() const noexcept { return m_device != VK_NULL_HANDLE; }

    // Before initialize(): where the pass's images and buffers come from (step 4a).
    void setAllocator(GpuAllocator& allocator) noexcept { m_allocator = &allocator; }

    // Initialize pass resources (pipelines, descriptors, etc.)
    [[nodiscard]] virtual bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) = 0;

    // Cleanup pass resources
    virtual void cleanup() = 0;

    // Record commands for this pass
    virtual void execute(VkCommandBuffer cmd, uint32_t frameIndex) = 0;

    // Resize handling
    virtual void onResize(uint32_t width, uint32_t height) {}

    // Get pass name for debugging
    [[nodiscard]] virtual const char* getName() const = 0;

protected:
    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    GpuAllocator* m_allocator{nullptr};

    // Safe Vulkan handle cleanup (null-check + destroy + reset)
    void safeDestroy(VkPipeline& handle) {
        if (handle != VK_NULL_HANDLE) vkDestroyPipeline(m_device, handle, nullptr);
        handle = VK_NULL_HANDLE;
    }
    void safeDestroy(VkPipelineLayout& handle) {
        if (handle != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, handle, nullptr);
        handle = VK_NULL_HANDLE;
    }
    void safeDestroy(VkDescriptorPool& handle) {
        if (handle != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, handle, nullptr);
        handle = VK_NULL_HANDLE;
    }
    void safeDestroy(VkDescriptorSetLayout& handle) {
        if (handle != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, handle, nullptr);
        handle = VK_NULL_HANDLE;
    }
    void safeDestroy(VkSampler& handle) {
        if (handle != VK_NULL_HANDLE) vkDestroySampler(m_device, handle, nullptr);
        handle = VK_NULL_HANDLE;
    }
};

// Cascade shadow map data
struct CascadeData {
    glm::mat4 viewProj[4];
    glm::vec4 splitDepths;
    float cascadeBlendWidth{0.1f};
    float shadowBias{0.012f};
    float normalBias{0.02f};
    uint32_t cascadeCount{4};
};

} // namespace ohao
