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

void OhaoVkSyncObjects::cleanup() {
    if (device) {
        // Clean up per-image semaphores
        cleanupPerImageSemaphores();
        
        // Clean up per-frame objects
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
    }
}

void OhaoVkSyncObjects::cleanupPerImageSemaphores() {
    if (device) {
        for (auto semaphore : perImageRenderFinishedSemaphores) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device->getDevice(), semaphore, nullptr);
            }
        }
        for (auto semaphore : perImageAvailableSemaphores) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device->getDevice(), semaphore, nullptr);
            }
        }
    }
    perImageAvailableSemaphores.clear();
    perImageRenderFinishedSemaphores.clear();
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

void OhaoVkSyncObjects::createPerImageSemaphores(uint32_t imageCount) {
    // Clean up existing per-image semaphores
    cleanupPerImageSemaphores();
    
    // Create new per-image semaphores
    perImageAvailableSemaphores.resize(imageCount);
    perImageRenderFinishedSemaphores.resize(imageCount);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (uint32_t i = 0; i < imageCount; i++) {
        if (vkCreateSemaphore(device->getDevice(), &semaphoreInfo, nullptr, &perImageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device->getDevice(), &semaphoreInfo, nullptr, &perImageRenderFinishedSemaphores[i]) != VK_SUCCESS) {
            // Clean up on failure
            cleanupPerImageSemaphores();
            return;
        }
    }
}

VkSemaphore OhaoVkSyncObjects::getImageAvailableSemaphore(uint32_t frameIndex) const {
    return imageAvailableSemaphores[frameIndex];
}

VkSemaphore OhaoVkSyncObjects::getRenderFinishedSemaphore(uint32_t frameIndex) const {
    return renderFinishedSemaphores[frameIndex];
}

VkSemaphore OhaoVkSyncObjects::getImageAvailableSemaphoreForImage(uint32_t imageIndex) const {
    return perImageAvailableSemaphores[imageIndex];
}

VkSemaphore OhaoVkSyncObjects::getRenderFinishedSemaphoreForImage(uint32_t imageIndex) const {
    return perImageRenderFinishedSemaphores[imageIndex];
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
