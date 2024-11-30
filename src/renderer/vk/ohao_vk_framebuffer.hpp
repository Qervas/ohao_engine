#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <array>

namespace ohao {

class OhaoVkDevice;
class OhaoVkSwapChain;
class OhaoVkRenderPass;
class OhaoVkImage;

class OhaoVkFramebuffer {
public:
    OhaoVkFramebuffer() = default;
    ~OhaoVkFramebuffer();

    bool initialize(
        OhaoVkDevice* device,
        OhaoVkSwapChain* swapchain,
        OhaoVkRenderPass* renderPass,
        OhaoVkImage* depthImage
    );
    void cleanup();

    VkFramebuffer getFramebuffer(size_t index) const;
    size_t getFramebufferCount() const { return framebuffers.size(); }

private:
    OhaoVkDevice* device{nullptr};
    OhaoVkSwapChain* swapchain{nullptr};
    OhaoVkRenderPass* renderPass{nullptr};
    OhaoVkImage* depthImage{nullptr};

    std::vector<VkFramebuffer> framebuffers;

    bool createFramebuffers();
};

} // namespace ohao
