#pragma once
#include "ohao_vk_render_pass.hpp"
#include "rhi/vk/ohao_vk_image.hpp"
#include <vulkan/vulkan.h>
#include <memory>

namespace ohao {

class VulkanContext;

class SceneRenderTarget {
public:
    SceneRenderTarget() = default;
    ~SceneRenderTarget();

    bool initialize(VulkanContext* context, uint32_t width, uint32_t height);
    void cleanup();

    VkDescriptorSet getDescriptorSet() const { return descriptorSet; }
    VkFramebuffer getFramebuffer() const { return framebuffer; }
    OhaoVkRenderPass* getRenderPass() const { return renderPass.get(); }
    VkRenderPass getVkRenderPass() const {return renderPass->getVkRenderPass();}
    uint32_t getWidth() const { return colorTarget ? colorTarget->getWidth() : 0; }
    uint32_t getHeight() const { return colorTarget ? colorTarget->getHeight() : 0; }
    OhaoVkImage* getColorTarget() const { return colorTarget.get(); }
    bool hasValidRenderTarget() const ;

    void resize(uint32_t width, uint32_t height);

private:
    VulkanContext* context{nullptr};

    std::unique_ptr<OhaoVkImage> colorTarget;
    std::unique_ptr<OhaoVkImage> depthTarget;
    std::unique_ptr<OhaoVkRenderPass> renderPass;
    VkSampler sampler{VK_NULL_HANDLE};
    VkFramebuffer framebuffer{VK_NULL_HANDLE};
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};

    bool createRenderTargets(uint32_t width, uint32_t height);
    bool createSampler();
    bool createRenderPass();
    bool createFramebuffer();
    bool createDescriptor();
};

} // namespace ohao
