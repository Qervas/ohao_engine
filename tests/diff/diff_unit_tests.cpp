#include "diff/device_caps.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

// Bare instance + physical device. No surface, no logical device — this test
// asks only what the hardware advertises.
struct BareVulkan {
    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice physical{VK_NULL_HANDLE};

    bool init() {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "diff_unit_tests";
        app.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) return false;

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (count == 0) return false;
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());

        // Prefer a discrete GPU; the iGPU lacks image atomics and may lack ray query.
        for (VkPhysicalDevice d : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(d, &props);
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                physical = d;
                return true;
            }
        }
        physical = devices[0];
        return true;
    }

    ~BareVulkan() {
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
    }
};

}  // namespace

TEST(DiffDeviceCaps, DiscreteGpuSupportsSubsystemRequirements) {
    BareVulkan vk;
    ASSERT_TRUE(vk.init()) << "no Vulkan instance / physical device available";

    const ohao::diff::DeviceCaps caps = ohao::diff::queryDeviceCaps(vk.physical);

    EXPECT_TRUE(caps.rayQuery)
        << "VK_KHR_ray_query is required: the differentiable traversal must be a single "
           "function, which rules out an SBT-dispatched RT pipeline";
    EXPECT_TRUE(caps.bufferFloat32AtomicAdd)
        << "shaderBufferFloat32AtomicAdd is required for gradient scatter";
    EXPECT_TRUE(caps.sufficient());
}

TEST(DiffDeviceCaps, SufficientRequiresBothFlags) {
    ohao::diff::DeviceCaps caps;
    EXPECT_FALSE(caps.sufficient());

    caps.rayQuery = true;
    EXPECT_FALSE(caps.sufficient());

    caps.rayQuery = false;
    caps.bufferFloat32AtomicAdd = true;
    EXPECT_FALSE(caps.sufficient());

    caps.rayQuery = true;
    EXPECT_TRUE(caps.sufficient());
}
