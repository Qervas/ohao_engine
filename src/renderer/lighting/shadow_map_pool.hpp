#pragma once

#include "unified_light.hpp"
#include <vulkan/vulkan.h>
#include <memory>
#include <vector>
#include <array>

namespace ohao {

class OhaoVkDevice;
class VulkanContext;
class Scene;

// Individual shadow map resource
struct ShadowMapResource {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView imageView{VK_NULL_HANDLE};
    bool inUse{false};
};

// ShadowMapPool - Pre-allocated pool of shadow map textures
// Avoids dynamic allocation during rendering
class ShadowMapPool {
public:
    static constexpr uint32_t SHADOW_MAP_SIZE = 2048;

    ShadowMapPool() = default;
    ~ShadowMapPool();

    // Initialize pool with device
    bool initialize(VulkanContext* context);
    void cleanup();

    // Allocate a shadow map from the pool
    ShadowMapHandle allocate();

    // Release a shadow map back to the pool
    void release(ShadowMapHandle handle);

    // Get shadow map resources
    VkImageView getImageView(ShadowMapHandle handle) const;
    VkImage getImage(ShadowMapHandle handle) const;

    // Get array of all image views (for descriptor binding)
    std::array<VkImageView, MAX_SHADOW_MAPS> getAllImageViews() const;

    // Get sampler for shadow maps (shared by all)
    VkSampler getSampler() const { return shadowSampler; }

    // Get render pass for shadow map rendering
    VkRenderPass getRenderPass() const { return shadowRenderPass; }

    // Get framebuffer for a specific shadow map
    VkFramebuffer getFramebuffer(ShadowMapHandle handle) const;

    // Render shadow map for a light
    void beginShadowPass(VkCommandBuffer cmd, ShadowMapHandle handle);
    void endShadowPass(VkCommandBuffer cmd);

    // Get dimensions
    uint32_t getWidth() const { return SHADOW_MAP_SIZE; }
    uint32_t getHeight() const { return SHADOW_MAP_SIZE; }

    // Check if initialized
    bool isInitialized() const { return initialized; }

private:
    VulkanContext* context{nullptr};
    OhaoVkDevice* device{nullptr};

    // Pool of shadow maps
    std::array<ShadowMapResource, MAX_SHADOW_MAPS> shadowMaps;
    std::array<VkFramebuffer, MAX_SHADOW_MAPS> framebuffers;

    // Shared resources
    VkSampler shadowSampler{VK_NULL_HANDLE};
    VkRenderPass shadowRenderPass{VK_NULL_HANDLE};

    // Placeholder for unused shadow map slots (1x1 white texture)
    VkImage placeholderImage{VK_NULL_HANDLE};
    VkDeviceMemory placeholderMemory{VK_NULL_HANDLE};
    VkImageView placeholderImageView{VK_NULL_HANDLE};

    bool initialized{false};

    bool createShadowMaps();
    bool createShadowSampler();
    bool createShadowRenderPass();
    bool createFramebuffers();
    bool createPlaceholderTexture();
    void transitionImagesToShaderReadLayout();
};

} // namespace ohao
