#pragma once
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <vector>

namespace ohao {

class OhaoVkInstance; // Forward declaration

class OhaoVkSurface {
public:
    OhaoVkSurface() = default;
    ~OhaoVkSurface();

    bool initialize(OhaoVkInstance* instance, GLFWwindow* window);
    void cleanup();

    VkSurfaceKHR getSurface() const { return surface; }

    // Helper functions for device compatibility checks
    VkSurfaceCapabilitiesKHR getCapabilities(VkPhysicalDevice physicalDevice) const;
    std::vector<VkSurfaceFormatKHR> getFormats(VkPhysicalDevice physicalDevice) const;
    std::vector<VkPresentModeKHR> getPresentModes(VkPhysicalDevice physicalDevice) const;

private:
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    OhaoVkInstance* vkInstance{nullptr}; // Non-owning pointer
};

} // namespace ohao
