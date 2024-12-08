#include "ohao_vk_framebuffer.hpp"
#include "ohao_vk_device.hpp"
#include "ohao_vk_swapchain.hpp"
#include "ohao_vk_render_pass.hpp"
#include "ohao_vk_image.hpp"
#include <iostream>
#include <stdexcept>

namespace ohao {

OhaoVkFramebuffer::~OhaoVkFramebuffer() {
    cleanup();
}

bool OhaoVkFramebuffer::initialize(
    OhaoVkDevice* devicePtr,
    OhaoVkSwapChain* swapchainPtr,
    OhaoVkRenderPass* renderPassPtr,
    OhaoVkImage* depthImagePtr)
{
    device = devicePtr;
    swapchain = swapchainPtr;
    renderPass = renderPassPtr;
    depthImage = depthImagePtr;

    return createFramebuffers();
}

void OhaoVkFramebuffer::cleanup() {
    if (device) {
        for (auto framebuffer : framebuffers) {
            if (framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device->getDevice(), framebuffer, nullptr);
            }
        }
    }
    framebuffers.clear();
}

VkFramebuffer OhaoVkFramebuffer::getFramebuffer(size_t index) const {
    if (index >= framebuffers.size()) {
        throw std::runtime_error("Framebuffer index out of range");
    }
    return framebuffers[index];
}

bool OhaoVkFramebuffer::createFramebuffers() {
    const auto& swapChainImageViews = swapchain->getImageViews();
    framebuffers.resize(swapChainImageViews.size());

    for (size_t i = 0; i < swapChainImageViews.size(); i++) {
        std::array<VkImageView, 2> attachments = {
            swapChainImageViews[i],
            depthImage->getImageView()
        };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass->getRenderPass();
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = swapchain->getExtent().width;
        framebufferInfo.height = swapchain->getExtent().height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(device->getDevice(), &framebufferInfo, nullptr, &framebuffers[i]) != VK_SUCCESS) {
            std::cerr << "Failed to create framebuffer!" << std::endl;
            return false;
        }
    }

    return true;
}

} // namespace ohao
