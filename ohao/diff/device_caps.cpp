#include "diff/device_caps.hpp"

#include <cstring>
#include <vector>

namespace ohao::diff {
namespace {

bool hasExtension(VkPhysicalDevice device, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.data());
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

}  // namespace

DeviceCaps queryDeviceCaps(VkPhysicalDevice physicalDevice) {
    DeviceCaps caps;
    if (physicalDevice == VK_NULL_HANDLE) return caps;

    // An advertised extension does not imply an enabled feature bit, so check both.
    const bool rqExt = hasExtension(physicalDevice, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    const bool afExt = hasExtension(physicalDevice, VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);

    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomicFloat{};
    atomicFloat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;

    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{};
    rayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    rayQuery.pNext = &atomicFloat;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &rayQuery;

    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

    caps.rayQuery = rqExt && (rayQuery.rayQuery == VK_TRUE);
    caps.bufferFloat32AtomicAdd = afExt && (atomicFloat.shaderBufferFloat32AtomicAdd == VK_TRUE);
    return caps;
}

}  // namespace ohao::diff
