#pragma once
#include <rhi/vk/ohao_vk_image.hpp>
#include <vulkan/vulkan.h>
#include <memory>

namespace ohao {

class VulkanContext;
class OhaoVkRenderPass;

class ShadowMapRenderTarget {
public:
    static constexpr uint32_t DEFAULT_SHADOW_MAP_SIZE = 2048;

    ShadowMapRenderTarget() = default;
    ~ShadowMapRenderTarget();

    bool initialize(VulkanContext* context, uint32_t width = DEFAULT_SHADOW_MAP_SIZE,
                   uint32_t height = DEFAULT_SHADOW_MAP_SIZE);
    void cleanup();

    VkFramebuffer getFramebuffer() const { return framebuffer; }
    VkRenderPass getVkRenderPass() const { return renderPass; }
    OhaoVkRenderPass* getRenderPass() const { return nullptr; } // Not using wrapper for shadow pass
    VkImageView getDepthImageView() const { return depthTarget ? depthTarget->getImageView() : VK_NULL_HANDLE; }
    VkSampler getShadowSampler() const { return shadowSampler; }
    uint32_t getWidth() const { return width; }
    uint32_t getHeight() const { return height; }
    bool hasValidRenderTarget() const;

private:
    VulkanContext* context{nullptr};
    uint32_t width{0};
    uint32_t height{0};

    std::unique_ptr<OhaoVkImage> depthTarget;
    VkRenderPass renderPass{VK_NULL_HANDLE};
    VkFramebuffer framebuffer{VK_NULL_HANDLE};
    VkSampler shadowSampler{VK_NULL_HANDLE};

    bool createDepthTarget();
    bool createRenderPass();
    bool createFramebuffer();
    bool createShadowSampler();
};

} // namespace ohao
