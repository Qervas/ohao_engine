#pragma once
#include <functional>
#include <vector>
#include <vulkan/vulkan.h>

namespace ohao {

// What a windowing system hands the renderer so it can present. The renderer
// never includes a windowing header: it adds `instanceExtensions` to the
// instance and calls `createSurface` once, right after the instance exists.
struct PresentTarget {
    std::vector<const char*> instanceExtensions;
    std::function<VkSurfaceKHR(VkInstance)> createSurface;
    VkExtent2D extent{0, 0};
    bool vsync{false};
};

} // namespace ohao
