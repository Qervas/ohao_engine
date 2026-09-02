#include "diff/device_caps.hpp"
#include "diff/geom/edge_adjacency.hpp"
#include "diff/grad/arena_layout.hpp"
#include "diff/param/param_registry.hpp"
#include "diff/rng/diff_rng.hpp"
#include "diff/wavefront/path_state_layout.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
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

TEST(DiffParamRegistry, RejectsShapeThatOverflows32Bits) {
    // An undersized block from a wrapped multiply is silent wrongness -- the exact
    // failure class this subsystem exists to make impossible.
    ohao::diff::ParamRegistry reg;
    const auto result = reg.registerTexture("huge", {65536u, 65536u, 4u},
                                            VK_FORMAT_R32G32B32A32_SFLOAT);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(reg.count(), 0u);
    EXPECT_EQ(reg.layout().blockCount(), 0u);
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

// ===========================================================================
// STAGE 3 TASK 1 -- EDGE ADJACENCY
// ===========================================================================
//
// The oracles here are TOPOLOGICAL INVARIANTS, not a second implementation:
// Euler's formula, "a closed manifold has two faces per edge", and
// "consistently wound faces traverse a shared edge in opposite directions".
// Each is a statement about what the structure must satisfy whatever the
// mesh is, so none of them can be satisfied by transcribing the code they
// check.

namespace {

/// The closed box tests/diff/context/probe_scene.hpp builds: six quads, two
/// triangles each, each face owning its own four vertices. Indices only --
/// adjacency never sees a position.
///
/// NOTE ON WHAT THIS BOX IS. Because every face owns its own vertices, the
/// 24 corners are SPLIT: the mesh is twelve triangles that are topologically
/// six DISCONNECTED quads, not a closed surface. That is exactly why the
/// welded box below exists, and the contrast between them is what makes the
/// boundary-edge count meaningful rather than incidental.
std::vector<std::uint32_t> splitCornerBoxIndices() {
    std::vector<std::uint32_t> indices;
    for (std::uint32_t face = 0; face < 6u; ++face) {
        const std::uint32_t b = face * 4u;
        indices.insert(indices.end(), {b + 0u, b + 1u, b + 2u, b + 0u, b + 2u, b + 3u});
    }
    return indices;
}

/// A WELDED cube: 8 shared corners, 12 triangles, every edge used twice.
/// Wound so that every face's normal points outward, which is what makes the
/// opposite-orientation assertion below a real test of the winding rather
/// than of the builder.
std::vector<std::uint32_t> weldedCubeIndices() {
    // Corner c has bit0 = x, bit1 = y, bit2 = z.
    return {
        0, 2, 3, 0, 3, 1,  // -z
        4, 5, 7, 4, 7, 6,  // +z
        0, 1, 5, 0, 5, 4,  // -y
        2, 6, 7, 2, 7, 3,  // +y
        0, 4, 6, 0, 6, 2,  // -x
        1, 3, 7, 1, 7, 5,  // +x
    };
}

}  // namespace

TEST(DiffEdgeAdjacency, WeldedCubeIsClosedAndSatisfiesEuler) {
    ohao::diff::EdgeAdjacency adj;
    ASSERT_TRUE(adj.build(weldedCubeIndices())) << adj.error();

    // V - E + F = 2 for any closed surface of genus 0. With V = 8 and
    // F = 12 this forces E = 18, and the builder is not consulted for any
    // of those three numbers.
    EXPECT_EQ(adj.edgeCount(), 18u);
    EXPECT_EQ(8u - adj.edgeCount() + 12u, 2u);

    // Closed: no edge has a single face.
    EXPECT_EQ(adj.boundaryEdgeCount(), 0u);

    // Consistently wound: the two faces of every edge traverse it in
    // opposite directions. A single inverted face breaks this and would
    // otherwise only show up as an inward-pointing normal much later.
    for (const auto& e : adj.edges()) {
        EXPECT_TRUE(e.oppositelyOriented)
            << "edge (" << e.v0 << ", " << e.v1 << ") is traversed the same way by both faces";
        EXPECT_LT(e.v0, e.v1) << "edges must be stored canonically, min endpoint first";
    }
}

TEST(DiffEdgeAdjacency, SplitCornerBoxIsSixDisconnectedQuads) {
    ohao::diff::EdgeAdjacency adj;
    ASSERT_TRUE(adj.build(splitCornerBoxIndices())) << adj.error();

    // Each quad contributes 5 edges (4 sides + 1 diagonal), and no vertex is
    // shared between quads, so there are 30 edges of which the 24 sides are
    // BOUNDARY edges and the 6 diagonals are interior.
    EXPECT_EQ(adj.edgeCount(), 30u);
    EXPECT_EQ(adj.boundaryEdgeCount(), 24u);

    // THE POINT OF THIS TEST. The probe's box LOOKS closed and is not,
    // topologically: buildAxisAlignedBoxGeometry gives every face its own
    // four vertices so that no face's winding constrains a neighbour's.
    // Spec 7.1 counts open boundary edges as silhouette edges
    // unconditionally, so Task 2 run on this mesh would mark all 24 sides
    // REGARDLESS of view. That is a real constraint on which scene Stage 3
    // can use, and it is recorded here rather than discovered there.
}

TEST(DiffEdgeAdjacency, IsInvariantUnderVertexMotion) {
    // The claim the whole per-iteration cost argument rests on (spec 7.1):
    // moving a vertex changes geometry, not connectivity. This type takes
    // only indices, so the claim is true by construction -- and this test is
    // what stops a future version quietly taking positions too.
    ohao::diff::EdgeAdjacency before;
    ASSERT_TRUE(before.build(weldedCubeIndices()));

    // "Moving every vertex" is expressed the only way a positionless
    // structure can see it: rebuilding from the SAME indices. If adjacency
    // ever depended on positions, this test would have to change to pass
    // them -- and that change is the alarm.
    ohao::diff::EdgeAdjacency after;
    ASSERT_TRUE(after.build(weldedCubeIndices()));

    ASSERT_EQ(before.edgeCount(), after.edgeCount());
    for (std::size_t i = 0; i < before.edgeCount(); ++i) {
        EXPECT_EQ(before.edges()[i].v0, after.edges()[i].v0);
        EXPECT_EQ(before.edges()[i].v1, after.edges()[i].v1);
        EXPECT_EQ(before.edges()[i].face0, after.edges()[i].face0);
        EXPECT_EQ(before.edges()[i].face1, after.edges()[i].face1);
    }
}

TEST(DiffEdgeAdjacency, RejectsNonManifoldAndMalformedInput) {
    ohao::diff::EdgeAdjacency adj;

    EXPECT_FALSE(adj.build({}));
    EXPECT_FALSE(adj.build({0, 1}));

    // THREE triangles sharing one edge. Refused rather than truncated to the
    // first two: Task 2's front/back-facing test asks about two faces, and
    // an edge with three has no answer -- keeping the first two found would
    // manufacture one, silently and order-dependently.
    const std::vector<std::uint32_t> fin = {0, 1, 2, 0, 1, 3, 0, 1, 4};
    EXPECT_FALSE(adj.build(fin));
    EXPECT_EQ(adj.edgeCount(), 0u) << "a refused build must leave nothing behind";
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

TEST(DiffPathRng, ReplayMatchesGoldenSequence) {
    // ReplayFromTupleReproducesTheStream above proves PathRng is
    // self-consistent -- two independent PathRng instances built from the
    // same tuple agree. It does NOT prove the sequence itself is correct or
    // stable: shaders/includes/diff/rng.glsl and this class are declared
    // mirrors ("change both or neither"), so diff_gpu_probe's check 6 and
    // its per-bounce RNG-parity check (Task 6) only ever prove the CPU and
    // GPU sides agree WITH EACH OTHER -- a coordinated edit to both files
    // would keep every one of those checks green while silently changing
    // the actual stream. Stage 1's path-replay backpropagation depends on
    // the specific sequence, not merely on the two sides concurring, so
    // this test anchors the CPU side to a fixed, independently-recorded
    // sequence that nothing else in the codebase can also drift.
    //
    // GOLDEN VALUES: captured by running the current PathRng implementation
    // once for forPath(pixelIndex=1234, sampleIndex=0, iterationSeed=20260828)
    // -- the exact tuple diff_gpu_probe.cpp's wf_scatter RNG-parity check
    // (kChosenPath=1234, kIterationSeed=20260828u) already replays. If the
    // sampler is ever changed deliberately, THIS TEST IS SUPPOSED TO FAIL --
    // update these literals (and diff_gpu_probe.cpp's check, and
    // rng.glsl in lockstep) with intent, not by silently loosening the
    // comparison.
    constexpr float kGolden[8] = {
        0.918804526f, 0.265892982f, 0.166186035f, 0.319334388f,
        0.689956903f, 0.765392363f, 0.388430357f, 0.743075907f,
    };

    auto rng = ohao::diff::PathRng::forPath(1234, 0, 20260828u);
    for (int i = 0; i < 8; ++i) {
        EXPECT_FLOAT_EQ(rng.next1D(), kGolden[static_cast<std::size_t>(i)])
            << "PathRng's sequence drifted from its recorded golden value at draw " << i
            << " -- if this is an intentional sampler change, update kGolden (and the GPU "
               "mirror in rng.glsl, and diff_gpu_probe.cpp's RNG-parity check) together";
    }
    EXPECT_EQ(rng.drawCount(), 8u);
}

TEST(DiffPathStateLayout, EachFieldGetsItsOwnBlockSizedByComponentCount) {
    // SoA: one contiguous block per field, so a stage touching only throughput
    // reads a dense run rather than striding over whole path structs.
    //
    // Iterate 0..kFieldCount rather than hand-listing enumerators: a
    // hand-enumerated array silently stops covering "every field" the
    // moment a new one is added to the enum (this happened once already --
    // HitT was added in Task 5 and this test kept testing the pre-HitT 16
    // until a review caught it). Deriving the loop bound from kFieldCount
    // means a future field addition is covered automatically, with no line
    // here to remember to update.
    constexpr std::uint32_t kCapacity = 1024;
    ohao::diff::PathStateLayout layout(kCapacity);

    using F = ohao::diff::PathStateField;

    std::set<std::size_t> seen;
    for (std::uint32_t i = 0; i < ohao::diff::PathStateLayout::kFieldCount; ++i) {
        const F f = static_cast<F>(i);
        const std::size_t b = layout.block(f);
        ASSERT_NE(b, ohao::diff::ArenaLayout::kInvalidBlock);
        EXPECT_TRUE(seen.insert(b).second) << "two fields share a block";
        EXPECT_EQ(layout.arena().block(b).sizeBytes, kCapacity * sizeof(float));
    }
    EXPECT_EQ(seen.size(), ohao::diff::PathStateLayout::kFieldCount);
    EXPECT_EQ(layout.capacity(), kCapacity);
}

TEST(DiffPathStateLayout, BlocksDoNotOverlap) {
    ohao::diff::PathStateLayout layout(256);
    const ohao::diff::ArenaLayout& a = layout.arena();
    for (std::size_t i = 1; i < a.blockCount(); ++i) {
        const auto prev = a.block(i - 1);
        const auto cur = a.block(i);
        EXPECT_GE(cur.offsetBytes, prev.offsetBytes + prev.sizeBytes);
    }
}

TEST(DiffPathStateLayout, ZeroCapacityIsRejected) {
    ohao::diff::PathStateLayout layout(0);
    EXPECT_EQ(layout.capacity(), 0u);
    EXPECT_EQ(layout.arena().blockCount(), 0u);
    EXPECT_EQ(layout.block(ohao::diff::PathStateField::OriginX),
              ohao::diff::ArenaLayout::kInvalidBlock);
}
