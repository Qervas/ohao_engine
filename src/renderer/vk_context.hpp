#pragma once
#include <vulkan/vulkan.hpp>

namespace ohao {

class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();

    bool initialize();
    void cleanup();

private:
    VkInstance instance;
    // More Vulkan objects will be added later
};

} // namespace ohao
