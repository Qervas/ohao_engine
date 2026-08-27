#include "diff/device_caps.hpp"
#include "diff/grad/arena_layout.hpp"

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

TEST(DiffArenaLayout, EmptyLayoutIsZeroBytes) {
    ohao::diff::ArenaLayout layout;
    EXPECT_EQ(layout.blockCount(), 0u);
    EXPECT_EQ(layout.totalBytes(), 0u);
}

TEST(DiffArenaLayout, SingleBlockStartsAtZeroAndPadsToAlignment) {
    ohao::diff::ArenaLayout layout;
    const std::size_t idx = layout.add(3);  // 3 floats = 12 bytes

    EXPECT_EQ(idx, 0u);
    EXPECT_EQ(layout.blockCount(), 1u);
    EXPECT_EQ(layout.block(0).offsetBytes, 0u);
    EXPECT_EQ(layout.block(0).sizeBytes, 12u);
    EXPECT_EQ(layout.totalBytes(), ohao::diff::ArenaLayout::kAlignmentBytes);
}

TEST(DiffArenaLayout, SecondBlockIsAligned) {
    ohao::diff::ArenaLayout layout;
    layout.add(3);
    const std::size_t idx = layout.add(64);  // 256 bytes

    EXPECT_EQ(idx, 1u);
    EXPECT_EQ(layout.block(1).offsetBytes, ohao::diff::ArenaLayout::kAlignmentBytes);
    EXPECT_EQ(layout.block(1).sizeBytes, 256u);
    EXPECT_EQ(layout.totalBytes(), ohao::diff::ArenaLayout::kAlignmentBytes + 256u);
}

TEST(DiffArenaLayout, BlocksNeverOverlap) {
    ohao::diff::ArenaLayout layout;
    for (std::size_t n : {1u, 7u, 100u, 4096u, 3u}) {
        layout.add(n);
    }
    for (std::size_t i = 1; i < layout.blockCount(); ++i) {
        const auto prev = layout.block(i - 1);
        const auto cur = layout.block(i);
        EXPECT_GE(cur.offsetBytes, prev.offsetBytes + prev.sizeBytes)
            << "block " << i << " overlaps block " << (i - 1);
    }
}

TEST(DiffArenaLayout, ZeroFloatBlockIsRejected) {
    ohao::diff::ArenaLayout layout;
    EXPECT_EQ(layout.add(0), ohao::diff::ArenaLayout::kInvalidBlock);
    EXPECT_EQ(layout.blockCount(), 0u);
}

TEST(DiffArenaLayout, OutOfRangeIndexReturnsInvalidBlock) {
    // sizeBytes == 0 is the invalid marker: add() rejects floatCount == 0, so no
    // valid block can have zero size. A caller must not read offsetBytes without
    // checking sizeBytes first -- offset 0 is a real location in the arena.
    ohao::diff::ArenaLayout layout;
    layout.add(8);

    const ohao::diff::ArenaBlock oob = layout.block(99);
    EXPECT_EQ(oob.sizeBytes, 0u);

    const ohao::diff::ArenaBlock valid = layout.block(0);
    EXPECT_NE(valid.sizeBytes, 0u);
}
