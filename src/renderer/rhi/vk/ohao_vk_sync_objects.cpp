#include "ohao_vk_sync_objects.hpp"
#include "ohao_vk_device.hpp"

namespace ohao {

OhaoVkSyncObjects::~OhaoVkSyncObjects() {
    cleanup();
}

bool OhaoVkSyncObjects::initialize(OhaoVkDevice* devicePtr, uint32_t maxFramesInFlight) {
    device = devicePtr;
    maxFrames = maxFramesInFlight;
    return createSyncObjects();
}

bool OhaoVkSyncObjects::initializeSwapchainSemaphores(uint32_t imageCount) {
    // Clean up existing swapchain semaphores first
    if (device) {
        for (size_t i = 0; i < swapchainImageCount; i++) {
            if (swapchainRenderFinishedSemaphores[i]) {
                vkDestroySemaphore(device->getDevice(), swapchainRenderFinishedSemaphores[i], nullptr);
            }
            if (swapchainImageAvailableSemaphores[i]) {
                vkDestroySemaphore(device->getDevice(), swapchainImageAvailableSemaphores[i], nullptr);
            }
        }
        swapchainRenderFinishedSemaphores.clear();
        swapchainImageAvailableSemaphores.clear();
    }
    
    swapchainImageCount = imageCount;
    return createSwapchainSemaphores();
}

void OhaoVkSyncObjects::cleanup() {
    if (device) {
        // Cleanup per-frame sync objects
        for (size_t i = 0; i < maxFrames; i++) {
            if (renderFinishedSemaphores[i]) {
                vkDestroySemaphore(device->getDevice(), renderFinishedSemaphores[i], nullptr);
            }
            if (imageAvailableSemaphores[i]) {
                vkDestroySemaphore(device->getDevice(), imageAvailableSemaphores[i], nullptr);
            }
            if (inFlightFences[i]) {
                vkDestroyFence(device->getDevice(), inFlightFences[i], nullptr);
            }
        }
        
        // Cleanup per-swapchain-image semaphores
        for (size_t i = 0; i < swapchainImageCount; i++) {
            if (swapchainRenderFinishedSemaphores[i]) {
                vkDestroySemaphore(device->getDevice(), swapchainRenderFinishedSemaphores[i], nullptr);
            }
            if (swapchainImageAvailableSemaphores[i]) {
                vkDestroySemaphore(device->getDevice(), swapchainImageAvailableSemaphores[i], nullptr);
            }
        }
        
        // Clear vectors
        renderFinishedSemaphores.clear();
        imageAvailableSemaphores.clear();
        inFlightFences.clear();
        swapchainRenderFinishedSemaphores.clear();
        swapchainImageAvailableSemaphores.clear();
    }
}

bool OhaoVkSyncObjects::createSyncObjects() {
    imageAvailableSemaphores.resize(maxFrames);
    renderFinishedSemaphores.resize(maxFrames);
    inFlightFences.resize(maxFrames);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < maxFrames; i++) {
        if (vkCreateSemaphore(device->getDevice(), &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device->getDevice(), &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(device->getDevice(), &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

bool OhaoVkSyncObjects::createSwapchainSemaphores() {
    swapchainImageAvailableSemaphores.resize(swapchainImageCount);
    swapchainRenderFinishedSemaphores.resize(swapchainImageCount);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (size_t i = 0; i < swapchainImageCount; i++) {
        if (vkCreateSemaphore(device->getDevice(), &semaphoreInfo, nullptr, &swapchainImageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device->getDevice(), &semaphoreInfo, nullptr, &swapchainRenderFinishedSemaphores[i]) != VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

VkSemaphore OhaoVkSyncObjects::getImageAvailableSemaphore(uint32_t frameIndex) const {
    return imageAvailableSemaphores[frameIndex];
}

VkSemaphore OhaoVkSyncObjects::getRenderFinishedSemaphore(uint32_t frameIndex) const {
    return renderFinishedSemaphores[frameIndex];
}

VkSemaphore OhaoVkSyncObjects::getSwapchainImageAvailableSemaphore(uint32_t imageIndex) const {
    return swapchainImageAvailableSemaphores[imageIndex];
}

VkSemaphore OhaoVkSyncObjects::getSwapchainRenderFinishedSemaphore(uint32_t imageIndex) const {
    return swapchainRenderFinishedSemaphores[imageIndex];
}

VkFence OhaoVkSyncObjects::getInFlightFence(uint32_t frameIndex) const {
    return inFlightFences[frameIndex];
}

void OhaoVkSyncObjects::waitForFence(uint32_t frameIndex) const {
    vkWaitForFences(device->getDevice(), 1, &inFlightFences[frameIndex], VK_TRUE, UINT64_MAX);
}

void OhaoVkSyncObjects::resetFence(uint32_t frameIndex) const {
    vkResetFences(device->getDevice(), 1, &inFlightFences[frameIndex]);
}

} // namespace ohao
