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

VkSemaphore OhaoVkSyncObjects::getImageAvailableSemaphore(uint32_t frameIndex) const {
    return imageAvailableSemaphores[frameIndex];
}

VkSemaphore OhaoVkSyncObjects::getRenderFinishedSemaphore(uint32_t frameIndex) const {
    return renderFinishedSemaphores[frameIndex];
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
