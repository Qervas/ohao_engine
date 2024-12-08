#include "ohao_vk_surface.hpp"
#include "ohao_vk_instance.hpp"
#include <stdexcept>
#include <iostream>

namespace ohao {

OhaoVkSurface::~OhaoVkSurface() {
    cleanup();
}

bool OhaoVkSurface::initialize(OhaoVkInstance* instance, GLFWwindow* window) {
    vkInstance = instance;
    if (!vkInstance) {
        std::cerr << "Null instance provided to OhaoVkSurface::initialize" << std::endl;
        return false;
    }

    VkResult result = glfwCreateWindowSurface(
        vkInstance->getInstance(),
        window,
        nullptr,
        &surface
    );

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to create window surface: " << result << std::endl;
        return false;
    }

    return true;
}

void OhaoVkSurface::cleanup() {
    if (surface && vkInstance) {
        vkDestroySurfaceKHR(vkInstance->getInstance(), surface, nullptr);
        surface = VK_NULL_HANDLE;
    }
}

VkSurfaceCapabilitiesKHR OhaoVkSurface::getCapabilities(VkPhysicalDevice physicalDevice) const {
    VkSurfaceCapabilitiesKHR capabilities;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities) != VK_SUCCESS) {
        throw std::runtime_error("failed to get physical device surface capabilities");
    }
    return capabilities;
}

std::vector<VkSurfaceFormatKHR> OhaoVkSurface::getFormats(VkPhysicalDevice physicalDevice) const {
    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);

    std::vector<VkSurfaceFormatKHR> formats;
    if (formatCount != 0) {
        formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());
    }
    return formats;
}

std::vector<VkPresentModeKHR> OhaoVkSurface::getPresentModes(VkPhysicalDevice physicalDevice) const {
    uint32_t presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, nullptr);

    std::vector<VkPresentModeKHR> presentModes;
    if (presentModeCount != 0) {
        presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(
            physicalDevice,
            surface,
            &presentModeCount,
            presentModes.data()
        );
    }
    return presentModes;
}

} // namespace ohao
