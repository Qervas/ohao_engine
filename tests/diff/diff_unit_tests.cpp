#include "diff/device_caps.hpp"
#include "diff/grad/arena_layout.hpp"
#include "diff/param/param_registry.hpp"
#include "diff/rng/diff_rng.hpp"

#include <gtest/gtest.h>

#include <cstdint>
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

TEST(DiffParamRegistry, RejectsEightBitSrgbTexture) {
    ohao::diff::ParamRegistry reg;
    const auto result = reg.registerTexture("albedo", {64, 64, 3}, VK_FORMAT_R8G8B8A8_SRGB);

    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("promoteToFloat"), std::string::npos)
        << "the error must name the remedy, not just the problem; got: " << result.error;
    EXPECT_EQ(reg.count(), 0u);
}

TEST(DiffParamRegistry, RejectsEightBitUnormTexture) {
    ohao::diff::ParamRegistry reg;
    const auto result = reg.registerTexture("albedo", {64, 64, 3}, VK_FORMAT_R8G8B8A8_UNORM);

    EXPECT_FALSE(result.ok)
        << "8-bit quantisation stalls Adam silently: any step below 1/255 rounds to nothing";
    EXPECT_EQ(reg.count(), 0u);
}

TEST(DiffParamRegistry, AcceptsFloat32Texture) {
    ohao::diff::ParamRegistry reg;
    const auto result = reg.registerTexture("albedo", {64, 64, 3},
                                            VK_FORMAT_R32G32B32A32_SFLOAT);

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(reg.count(), 1u);

    const ohao::diff::DiffParam* p = reg.find("albedo");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->kind, ohao::diff::ParamKind::Texture);
    EXPECT_EQ(p->floatCount, 64u * 64u * 3u);
}

TEST(DiffParamRegistry, RejectsDuplicateName) {
    ohao::diff::ParamRegistry reg;
    ASSERT_TRUE(reg.registerTexture("albedo", {8, 8, 3}, VK_FORMAT_R32G32B32A32_SFLOAT).ok);

    const auto dup = reg.registerTexture("albedo", {8, 8, 3}, VK_FORMAT_R32G32B32A32_SFLOAT);
    EXPECT_FALSE(dup.ok);
    EXPECT_EQ(reg.count(), 1u);
}

TEST(DiffParamRegistry, ScalarBlockIsNotATextureSpecialCase) {
    // The registry's primitive is "floats with a gradient block". Neural weights,
    // pose, and geometry all register this way -- textures merely add shape.
    ohao::diff::ParamRegistry reg;
    const auto result = reg.registerScalarBlock("ssao_params", 4);

    ASSERT_TRUE(result.ok) << result.error;
    const ohao::diff::DiffParam* p = reg.find("ssao_params");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->kind, ohao::diff::ParamKind::ScalarBlock);
    EXPECT_EQ(p->floatCount, 4u);
}

TEST(DiffParamRegistry, LayoutGrowsWithEachParam) {
    ohao::diff::ParamRegistry reg;
    EXPECT_EQ(reg.layout().blockCount(), 0u);

    ASSERT_TRUE(reg.registerTexture("albedo", {8, 8, 3}, VK_FORMAT_R32G32B32A32_SFLOAT).ok);
    ASSERT_TRUE(reg.registerScalarBlock("ssao_params", 4).ok);

    EXPECT_EQ(reg.layout().blockCount(), 4u);
    EXPECT_GT(reg.layout().totalBytes(), 0u);
}

TEST(DiffParamRegistry, GradAndStateBlocksHaveDistinctCorrectSizes) {
    // Stage 1 routes every gradient scatter through these indices. If gradBlock and
    // stateBlock were swapped, gradients would land in Adam's m/v storage silently.
    // Pinning the SIZES (not just the count) makes that swap impossible to introduce.
    ohao::diff::ParamRegistry reg;
    constexpr std::uint32_t kFloats = 8u * 8u * 3u;

    const auto result = reg.registerTexture("albedo", {8, 8, 3}, VK_FORMAT_R32G32B32A32_SFLOAT);
    ASSERT_TRUE(result.ok) << result.error;

    const ohao::diff::DiffParam* p = reg.find("albedo");
    ASSERT_NE(p, nullptr);
    ASSERT_EQ(p->floatCount, kFloats);
    ASSERT_NE(p->gradBlock, p->stateBlock);

    // Gradient block: one float per parameter value.
    EXPECT_EQ(reg.layout().block(p->gradBlock).sizeBytes, kFloats * sizeof(float));
    // Adam state block: m and v, so exactly twice the gradient block.
    EXPECT_EQ(reg.layout().block(p->stateBlock).sizeBytes, kFloats * 2u * sizeof(float));
}

TEST(DiffParamRegistry, GetReturnsParamForValidIdAndNullForInvalid) {
    ohao::diff::ParamRegistry reg;
    const auto result = reg.registerScalarBlock("ssao_params", 4);
    ASSERT_TRUE(result.ok) << result.error;

    const ohao::diff::DiffParam* byId = reg.get(result.id);
    ASSERT_NE(byId, nullptr);
    EXPECT_EQ(byId->name, "ssao_params");

    EXPECT_EQ(reg.get(ohao::diff::ParamId{}), nullptr);          // default-constructed sentinel
    EXPECT_EQ(reg.get(ohao::diff::ParamId{9999u}), nullptr);     // out of range
}

TEST(DiffParamRegistry, FindReturnsNullForUnregisteredName) {
    ohao::diff::ParamRegistry reg;
    ASSERT_TRUE(reg.registerScalarBlock("present", 2).ok);
    EXPECT_EQ(reg.find("absent"), nullptr);
}

TEST(DiffParamRegistry, RejectsEmptyNameAndZeroFloatCount) {
    ohao::diff::ParamRegistry reg;

    EXPECT_FALSE(reg.registerScalarBlock("", 4).ok);
    EXPECT_FALSE(reg.registerScalarBlock("zero", 0).ok);

    // A rejected registration must consume nothing.
    EXPECT_EQ(reg.count(), 0u);
    EXPECT_EQ(reg.layout().blockCount(), 0u);
}

TEST(DiffPathRng, SameTupleProducesIdenticalStream) {
    auto a = ohao::diff::PathRng::forPath(1234, 7, 99);
    auto b = ohao::diff::PathRng::forPath(1234, 7, 99);

    for (int i = 0; i < 32; ++i) {
        EXPECT_FLOAT_EQ(a.next1D(), b.next1D()) << "divergence at draw " << i;
    }
}

TEST(DiffPathRng, ReplayFromTupleReproducesTheStream) {
    // This is the seed invariant that path replay backpropagation depends on:
    // the backward pass reconstructs the RNG from the tuple alone and must
    // walk the same path the forward pass walked.
    auto forward = ohao::diff::PathRng::forPath(4096, 3, 12345);
    std::vector<float> forwardDraws;
    for (int i = 0; i < 16; ++i) forwardDraws.push_back(forward.next1D());

    auto backward = ohao::diff::PathRng::forPath(4096, 3, 12345);
    for (int i = 0; i < 16; ++i) {
        EXPECT_FLOAT_EQ(backward.next1D(), forwardDraws[static_cast<std::size_t>(i)])
            << "replay diverged at draw " << i;
    }
}

TEST(DiffPathRng, DifferentPixelsDecorrelate) {
    auto a = ohao::diff::PathRng::forPath(10, 0, 1);
    auto b = ohao::diff::PathRng::forPath(11, 0, 1);

    int identical = 0;
    for (int i = 0; i < 16; ++i) {
        if (a.next1D() == b.next1D()) ++identical;
    }
    EXPECT_LT(identical, 3) << "neighbouring pixels are producing correlated streams";
}

TEST(DiffPathRng, DifferentSeedsDecorrelate) {
    auto a = ohao::diff::PathRng::forPath(10, 0, 1);
    auto b = ohao::diff::PathRng::forPath(10, 0, 2);

    int identical = 0;
    for (int i = 0; i < 16; ++i) {
        if (a.next1D() == b.next1D()) ++identical;
    }
    EXPECT_LT(identical, 3);
}

TEST(DiffPathRng, DrawsAreInUnitInterval) {
    auto rng = ohao::diff::PathRng::forPath(777, 5, 42);
    for (int i = 0; i < 4096; ++i) {
        const float v = rng.next1D();
        EXPECT_GE(v, 0.0f);
        EXPECT_LT(v, 1.0f);
    }
}

TEST(DiffPathRng, DrawCountTracksConsumption) {
    // Forward and backward must consume the same number of draws. A mismatch
    // here is the failure mode that silently corrupts every gradient, so the
    // counter exists to be asserted on in later stages.
    auto rng = ohao::diff::PathRng::forPath(1, 1, 1);
    EXPECT_EQ(rng.drawCount(), 0u);
    rng.next1D();
    rng.next1D();
    EXPECT_EQ(rng.drawCount(), 2u);
}
