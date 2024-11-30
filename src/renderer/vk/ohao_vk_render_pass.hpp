#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <vector>

namespace ohao {

class OhaoVkDevice;
class OhaoVkSwapChain;

class OhaoVkRenderPass {
public:
    OhaoVkRenderPass() = default;
    ~OhaoVkRenderPass();

    bool initialize(OhaoVkDevice* device, OhaoVkSwapChain* swapchain);
    void cleanup();

    VkRenderPass getRenderPass() const { return renderPass; }

    void begin(VkCommandBuffer commandBuffer,
              VkFramebuffer framebuffer,
              const VkExtent2D& extent,
              const std::array<float, 4>& clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
              float clearDepth = 1.0f,
              uint32_t clearStencil = 0);

    void end(VkCommandBuffer commandBuffer);

private:
    OhaoVkDevice* device{nullptr};
    OhaoVkSwapChain* swapchain{nullptr};
    VkRenderPass renderPass{VK_NULL_HANDLE};
    VkSampleCountFlagBits msaaSamples{VK_SAMPLE_COUNT_1_BIT};

    bool createRenderPass();
    VkFormat findDepthFormat() const;
    bool hasStencilComponent(VkFormat format) const;
};

} // namespace ohao
