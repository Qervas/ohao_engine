#include "diff/device_caps.hpp"
#include "diff/geom/edge_adjacency.hpp"
#include "diff/geom/boundary_integrand.hpp"
#include "diff/geom/silhouette_set.hpp"
#include "diff/geom/vertex_parameterisation.hpp"
#include "diff/grad/arena_layout.hpp"
#include "diff/param/param_registry.hpp"
#include "diff/rng/diff_rng.hpp"
#include "diff/wavefront/path_state_layout.hpp"

#include <gtest/gtest.h>

#include <cmath>
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

/// Positions for weldedCubeIndices(): corner c at (+/-1)^3 with bit0 = x,
/// bit1 = y, bit2 = z -- the same bit convention the index list is written
/// against, so the two cannot drift.
std::vector<float> weldedCubePositions() {
    std::vector<float> p;
    for (std::uint32_t c = 0; c < 8u; ++c) {
        p.push_back((c & 1u) ? 1.0f : -1.0f);
        p.push_back((c & 2u) ? 1.0f : -1.0f);
        p.push_back((c & 4u) ? 1.0f : -1.0f);
    }
    return p;
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

// ===========================================================================
// STAGE 3 TASK 2 -- THE SILHOUETTE SET
// ===========================================================================

TEST(DiffSilhouetteSet, CornerViewGivesAHexagonalLoop) {
    ohao::diff::EdgeAdjacency adj;
    ASSERT_TRUE(adj.build(weldedCubeIndices())) << adj.error();
    const std::vector<float> pos = weldedCubePositions();

    // Looking down the (1,1,1) diagonal: three faces front-facing, three
    // back. The boundary between two connected sets of three cube faces is a
    // HEXAGON -- six edges. That count is a fact about the cube and the
    // direction, derived here and not read off the implementation.
    const float camera[3] = {3.0f, 3.0f, 3.0f};
    ohao::diff::SilhouetteSet sil;
    ASSERT_TRUE(sil.build(adj, pos, weldedCubeIndices(), camera)) << sil.error();
    EXPECT_EQ(sil.size(), 6u);

    const char* why = "";
    EXPECT_TRUE(sil.isSingleClosedLoop(adj, &why)) << why;
}

TEST(DiffSilhouetteSet, FaceOnViewGivesAFourEdgeLoop) {
    ohao::diff::EdgeAdjacency adj;
    ASSERT_TRUE(adj.build(weldedCubeIndices())) << adj.error();
    const std::vector<float> pos = weldedCubePositions();

    // Nearly face-on to +x, but NOT exactly: an exactly face-on camera puts
    // the four side faces edge-on, where dot(n, c - p) is 0 and "front
    // facing" is a coin toss between two equally defensible answers. The
    // offset is what keeps this test about the silhouette rather than about
    // a tie-break. One face front-facing, five back, so the silhouette is
    // that face's four edges.
    const float camera[3] = {5.0f, 0.1f, 0.1f};
    ohao::diff::SilhouetteSet sil;
    ASSERT_TRUE(sil.build(adj, pos, weldedCubeIndices(), camera)) << sil.error();
    EXPECT_EQ(sil.size(), 4u);

    const char* why = "";
    EXPECT_TRUE(sil.isSingleClosedLoop(adj, &why)) << why;
}

TEST(DiffSilhouetteSet, IsViewDependent) {
    // NON-VACUITY, and Stage 2's check 53 is why it is here: two "different"
    // views that produce the same answer make every invariant above hold for
    // a reason that has nothing to do with the view being read. Here the two
    // sets differ in SIZE, which is the strongest form of differing.
    ohao::diff::EdgeAdjacency adj;
    ASSERT_TRUE(adj.build(weldedCubeIndices()));
    const std::vector<float> pos = weldedCubePositions();

    const float cornerView[3] = {3.0f, 3.0f, 3.0f};
    const float faceView[3] = {5.0f, 0.1f, 0.1f};
    ohao::diff::SilhouetteSet a;
    ohao::diff::SilhouetteSet b;
    ASSERT_TRUE(a.build(adj, pos, weldedCubeIndices(), cornerView));
    ASSERT_TRUE(b.build(adj, pos, weldedCubeIndices(), faceView));

    EXPECT_NE(a.size(), b.size())
        << "two cameras gave silhouettes of the same size; the pass may not be reading the view";
    EXPECT_NE(a.markedEdges(), b.markedEdges());
}

TEST(DiffSilhouetteSet, OpenMeshMarksEveryBoundaryEdgeRegardlessOfView) {
    // THE CONSEQUENCE TASK 1 MEASURED, asserted so it cannot be forgotten.
    // Spec 7.1 counts open boundary edges as silhouette unconditionally, so
    // on the probe's split-corner box -- six disconnected quads, 24 of its 30
    // edges open -- the silhouette contains all 24 for EVERY camera. That is
    // correct behaviour and it is also why that box cannot be the scene this
    // pass is tested on: its view-dependence test would compare two sets
    // that differ only in the six interior diagonals.
    ohao::diff::EdgeAdjacency adj;
    const std::vector<std::uint32_t> idx = splitCornerBoxIndices();
    ASSERT_TRUE(adj.build(idx));
    ASSERT_EQ(adj.boundaryEdgeCount(), 24u);

    std::vector<float> pos;
    for (std::uint32_t face = 0; face < 6u; ++face) {
        for (std::uint32_t corner = 0; corner < 4u; ++corner) {
            pos.push_back(static_cast<float>(face) - 2.5f);
            pos.push_back(static_cast<float>(corner) - 1.5f);
            pos.push_back(static_cast<float>((face + corner) % 3u) - 1.0f);
        }
    }

    const float viewA[3] = {10.0f, 0.0f, 0.0f};
    const float viewB[3] = {0.0f, 10.0f, 0.0f};
    ohao::diff::SilhouetteSet a;
    ohao::diff::SilhouetteSet b;
    ASSERT_TRUE(a.build(adj, pos, idx, viewA));
    ASSERT_TRUE(b.build(adj, pos, idx, viewB));
    EXPECT_GE(a.size(), 24u);
    EXPECT_GE(b.size(), 24u);
}

TEST(DiffSilhouetteSet, DisjointLoopsFailTheSingleLoopTest) {
    // The degree test alone is NOT sufficient: two disjoint cycles have
    // every vertex at degree 2. This is what the connectedness walk is for,
    // and asserting it here is what stops that walk being deleted as
    // redundant.
    //
    // Two separate tetrahedra give an adjacency whose "silhouette" from a
    // camera between them is two disjoint loops.
    const std::vector<std::uint32_t> twoTets = {
        0, 1, 2, 0, 2, 3, 0, 3, 1, 1, 3, 2,
        4, 5, 6, 4, 6, 7, 4, 7, 5, 5, 7, 6,
    };
    ohao::diff::EdgeAdjacency adj;
    ASSERT_TRUE(adj.build(twoTets)) << adj.error();

    const std::vector<float> pos = {
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
        8.0f, 0.0f, 0.0f, 9.0f, 0.0f, 0.0f, 8.0f, 1.0f, 0.0f, 8.0f, 0.0f, 1.0f,
    };
    const float camera[3] = {4.0f, 6.0f, 6.0f};
    ohao::diff::SilhouetteSet sil;
    ASSERT_TRUE(sil.build(adj, pos, twoTets, camera)) << sil.error();

    const char* why = "";
    EXPECT_FALSE(sil.isSingleClosedLoop(adj, &why));
    EXPECT_STREQ(why, "the marked set splits into more than one loop");
}

TEST(DiffSilhouetteSet, RejectsMalformedInput) {
    ohao::diff::EdgeAdjacency adj;
    ASSERT_TRUE(adj.build(weldedCubeIndices()));
    const float camera[3] = {3.0f, 3.0f, 3.0f};

    ohao::diff::SilhouetteSet sil;
    EXPECT_FALSE(sil.build(adj, {}, weldedCubeIndices(), camera));
    EXPECT_FALSE(sil.build(adj, {0.0f, 1.0f}, weldedCubeIndices(), camera));
    // An index past the end of the position array: caught rather than read.
    EXPECT_FALSE(sil.build(adj, {0.0f, 0.0f, 0.0f}, weldedCubeIndices(), camera));
}

// ===========================================================================
// STAGE 3 TASK 3 -- THE BOUNDARY INTEGRAND
// ===========================================================================
//
// THE ORACLE IS A SUPERSAMPLED AREA FINITE DIFFERENCE, which shares nothing
// with the closed form it checks. It knows only "which side of a line is
// this point on"; it has no chord, no normal, no barycentric weight and no
// calculus. Two computations of one quantity with nothing in common but the
// answer.

namespace {

/// dA/dtheta for theta = moving p0 along d, by central difference on an area
/// obtained by counting supersamples. `n` is the grid resolution.
double areaDerivativeBySupersampling(const ohao::diff::PixelEdge& edge, const float d[2],
                                     double h, std::uint32_t n) {
    ohao::diff::PixelEdge plus = edge;
    ohao::diff::PixelEdge minus = edge;
    for (int k = 0; k < 2; ++k) {
        plus.p0[k] = static_cast<float>(edge.p0[k] + h * d[k]);
        minus.p0[k] = static_cast<float>(edge.p0[k] - h * d[k]);
    }
    const double aPlus = ohao::diff::areaInsideBySupersampling(plus, n);
    const double aMinus = ohao::diff::areaInsideBySupersampling(minus, n);
    return (aPlus - aMinus) / (2.0 * h);
}

/// An edge that fully crosses the pixel, tilted so the chord length is not 1
/// and the answer is not a round number by accident.
ohao::diff::PixelEdge tiltedCrossingEdge() {
    return {{-0.5f, -0.25f}, {1.5f, 1.1f}};
}

}  // namespace

TEST(DiffBoundaryIntegrand, ClosedFormMatchesASupersampledAreaDerivative) {
    const ohao::diff::PixelEdge edge = tiltedCrossingEdge();
    const float d[2] = {0.0f, 1.0f};  // move p0 in +y
    constexpr double kLIn = 1.0;
    constexpr double kLOut = 0.0;

    // With L_in - L_out = 1 the boundary term IS dA/dtheta, so the two sides
    // are directly comparable without a scale in between.
    const double closed = ohao::diff::boundaryTermMovingP0(edge, d, kLIn, kLOut);
    const double oracle = areaDerivativeBySupersampling(edge, d, 1.0 / 64.0, 2048u);

    // The oracle is a COUNT over a 2048^2 grid, so its own resolution is
    // ~1/2048 per unit area over a step of 2/64 -- about 1.5e-2 relative.
    // This tolerance is that, not a fitted number.
    EXPECT_NEAR(closed, oracle, 2e-2 * std::fabs(oracle) + 1e-3)
        << "closed form " << closed << " against a supersampled area derivative " << oracle;
    EXPECT_GT(std::fabs(closed), 0.1)
        << "the term is near zero, so agreeing with the oracle would mean little";
}

TEST(DiffBoundaryIntegrand, SampledEstimatorConvergesToTheClosedForm) {
    const ohao::diff::PixelEdge edge = tiltedCrossingEdge();
    const float d[2] = {0.3f, 0.9f};
    const double closed = ohao::diff::boundaryTermMovingP0(edge, d, 2.0, 0.5);

    // The midpoint rule on a LINEAR integrand is exact for any sample count,
    // so this is not a convergence test in the usual sense -- it is the
    // assertion that the estimator integrates the (1-u) weight rather than
    // evaluating it at one end. A one-sided or endpoint-weighted estimator
    // gets a different number at every count; this one is exact at all.
    for (std::uint32_t samples : {1u, 2u, 8u, 64u}) {
        const double sampled =
            ohao::diff::boundaryTermMovingP0Sampled(edge, d, 2.0, 0.5, samples);
        EXPECT_NEAR(sampled, closed, 1e-12 * std::fabs(closed) + 1e-12)
            << "at " << samples << " samples";
    }
}

TEST(DiffBoundaryIntegrand, IsAntisymmetricUnderSwappingTheSides) {
    // Swapping which side is "inside" negates the integrand EXACTLY. A
    // one-sided evaluation -- reading only L_in, say -- would pass a
    // magnitude check and fail this one.
    const ohao::diff::PixelEdge edge = tiltedCrossingEdge();
    const float d[2] = {0.7f, -0.2f};
    const double ab = ohao::diff::boundaryTermMovingP0(edge, d, 3.0, 1.0);
    const double ba = ohao::diff::boundaryTermMovingP0(edge, d, 1.0, 3.0);
    EXPECT_DOUBLE_EQ(ab, -ba);
    EXPECT_NE(ab, 0.0);
}

TEST(DiffBoundaryIntegrand, IsLinearInTheJumpAndInTheDisplacement) {
    // Two structural identities, each cheap and each catching a different
    // wrong shape: a term that squared the jump, or that normalised the
    // displacement away.
    const ohao::diff::PixelEdge edge = tiltedCrossingEdge();
    const float d[2] = {0.4f, 0.6f};
    const double base = ohao::diff::boundaryTermMovingP0(edge, d, 1.0, 0.0);

    const double doubledJump = ohao::diff::boundaryTermMovingP0(edge, d, 2.0, 0.0);
    EXPECT_NEAR(doubledJump, 2.0 * base, 1e-12);

    const float d2[2] = {0.8f, 1.2f};
    const double doubledStep = ohao::diff::boundaryTermMovingP0(edge, d2, 1.0, 0.0);
    EXPECT_NEAR(doubledStep, 2.0 * base, 1e-12);
}

TEST(DiffBoundaryIntegrand, AnEdgeMissingThePixelContributesNothing) {
    // The boundary term is an integral over the chord, and an edge that does
    // not cross the pixel has none. Zero, not small.
    const ohao::diff::PixelEdge miss = {{2.0f, 2.0f}, {3.0f, 2.5f}};
    const float d[2] = {1.0f, 0.0f};
    EXPECT_FALSE(ohao::diff::clipEdgeToPixel(miss).crosses);
    EXPECT_EQ(ohao::diff::boundaryTermMovingP0(miss, d, 5.0, 0.0), 0.0);
    EXPECT_EQ(ohao::diff::boundaryTermMovingP0Sampled(miss, d, 5.0, 0.0, 16u), 0.0);
}

TEST(DiffBoundaryIntegrand, MovingAnEndpointAlongTheEdgeChangesNothing) {
    // A displacement PARALLEL to the edge slides the endpoint along the line
    // without moving the line, so (d.n) is 0 and the boundary term vanishes.
    // This is the check that the term reads the NORMAL component and not the
    // displacement's magnitude -- the most plausible way to get a boundary
    // term that is right up to a factor and wrong in direction.
    const ohao::diff::PixelEdge edge = tiltedCrossingEdge();
    const double dx = static_cast<double>(edge.p1[0]) - edge.p0[0];
    const double dy = static_cast<double>(edge.p1[1]) - edge.p0[1];
    const double len = std::sqrt(dx * dx + dy * dy);
    const float along[2] = {static_cast<float>(dx / len), static_cast<float>(dy / len)};
    // The tolerance is float32 rounding, not a fitted number: `along` is a
    // float and the normal is derived from float endpoints, so (d.n) lands at
    // ~1e-7 rather than exactly 0, and it is then multiplied by the segment
    // length (~2.3), the weight (~0.5) and the jump (3). An earlier 1e-12
    // demanded exactness the inputs cannot carry.
    EXPECT_NEAR(ohao::diff::boundaryTermMovingP0(edge, along, 4.0, 1.0), 0.0, 1e-6);
}

// ===========================================================================
// STAGE 3 TASK 4 -- THE BOUNDARY TERM OVER A WHOLE IMAGE
// ===========================================================================
//
// Task 3 checked one pixel against a closed form. This is the step to an
// IMAGE: a triangle summed over every pixel its edges cross, which is the
// boundary integral of spec 4.1 as an optimiser would actually evaluate it.
//
// THE SCENE IS CHOSEN SO THE INTERIOR TERM IS EXACTLY ZERO. A constant-
// radiance triangle against a constant background has no shading that varies
// with position, so moving a vertex changes NOTHING except which pixels are
// covered. dI/dtheta is then PURELY the boundary term, and the rendered
// finite difference is directly comparable to it. Against a shaded scene the
// two terms are summed and neither is separately observable -- which is why
// this case, and not a prettier one, is what pins the boundary term down.
//
// ORTHOGRAPHIC, AND SAID RATHER THAN HIDDEN. Screen space IS world xy here,
// so a vertex's screen-space velocity is its world velocity and no projection
// Jacobian enters. That derivative is real and belongs to the perspective
// case; separating it from the boundary integrand is the point of leaving it
// out here.

namespace {

struct Tri2D {
    float v[3][2];
};

/// Is p inside the triangle? Half-plane test on all three edges, with the
/// sign taken from the triangle's own winding so the test does not assume
/// one.
bool insideTriangle(const Tri2D& t, double px, double py) {
    auto cross = [](double ax, double ay, double bx, double by) { return ax * by - ay * bx; };
    const double s0 = cross(t.v[1][0] - t.v[0][0], t.v[1][1] - t.v[0][1],
                            px - t.v[0][0], py - t.v[0][1]);
    const double s1 = cross(t.v[2][0] - t.v[1][0], t.v[2][1] - t.v[1][1],
                            px - t.v[1][0], py - t.v[1][1]);
    const double s2 = cross(t.v[0][0] - t.v[2][0], t.v[0][1] - t.v[2][1],
                            px - t.v[2][0], py - t.v[2][1]);
    return (s0 >= 0.0 && s1 >= 0.0 && s2 >= 0.0) || (s0 <= 0.0 && s1 <= 0.0 && s2 <= 0.0);
}

/// J = SUM over pixels of the pixel's average radiance, by supersampling.
/// The ORACLE's primitive: it knows only "is this point inside the
/// triangle", and nothing about edges, normals or derivatives.
double imageTotalBySupersampling(const Tri2D& t, std::uint32_t w, std::uint32_t h, double lTri,
                                 double lEnv, std::uint32_t sub) {
    double total = 0.0;
    for (std::uint32_t py = 0; py < h; ++py) {
        for (std::uint32_t px = 0; px < w; ++px) {
            std::uint32_t in = 0;
            for (std::uint32_t sy = 0; sy < sub; ++sy) {
                for (std::uint32_t sx = 0; sx < sub; ++sx) {
                    const double x = px + (sx + 0.5) / sub;
                    const double y = py + (sy + 0.5) / sub;
                    if (insideTriangle(t, x, y)) ++in;
                }
            }
            const double cov = static_cast<double>(in) / (sub * sub);
            total += cov * lTri + (1.0 - cov) * lEnv;
        }
    }
    return total;
}

/// The boundary term for moving vertex `movedVertex` along `d`, summed over
/// every pixel every edge crosses. This is the estimator, built from Task 3's
/// per-pixel integrand.
double imageBoundaryTerm(const Tri2D& t, std::uint32_t w, std::uint32_t h, double lTri,
                         double lEnv, std::uint32_t movedVertex, const float d[2]) {
    double total = 0.0;
    for (std::uint32_t e = 0; e < 3u; ++e) {
        const std::uint32_t a = e;
        const std::uint32_t b = (e + 1u) % 3u;
        // Only the two edges TOUCHING the moved vertex have a velocity; the
        // opposite edge does not move at all, so it contributes nothing.
        // Skipping it is not an optimisation -- including it with a zero
        // velocity would be the same number, and stating which edges move is
        // what makes the (1-u) weight's endpoint unambiguous.
        if (a != movedVertex && b != movedVertex) continue;
        // Task 3's integrand takes p0 as the MOVING endpoint, so the edge is
        // oriented with the moved vertex first.
        const bool reversed = (a != movedVertex);
        const std::uint32_t p0 = reversed ? b : a;
        const std::uint32_t p1 = reversed ? a : b;
        // REVERSING THE EDGE FLIPS ITS NORMAL, and boundaryTermMovingP0
        // defines "inside" as the negative side of that normal -- so a
        // reversed edge has its two sides swapped and must be given the
        // radiances swapped to compensate.
        //
        // Getting this wrong is not a small error and it is not obviously a
        // sign either: the two edges touching a moved vertex partly CANCEL,
        // because extending one sweeps area in as the other sweeps it out.
        // With one of them negated they ADD instead, and the sum came out at
        // -9.625 against the oracle's -3.123 -- same sign, three times too
        // large. A sign check would have passed it.
        const double edgeIn = reversed ? lEnv : lTri;
        const double edgeOut = reversed ? lTri : lEnv;

        for (std::uint32_t py = 0; py < h; ++py) {
            for (std::uint32_t px = 0; px < w; ++px) {
                // Task 3 works in a UNIT pixel, so the edge is expressed in
                // this pixel's local frame. The integrand is an area rate, so
                // no scale factor is needed: a unit pixel here IS one pixel.
                ohao::diff::PixelEdge pe;
                pe.p0[0] = t.v[p0][0] - static_cast<float>(px);
                pe.p0[1] = t.v[p0][1] - static_cast<float>(py);
                pe.p1[0] = t.v[p1][0] - static_cast<float>(px);
                pe.p1[1] = t.v[p1][1] - static_cast<float>(py);
                total += ohao::diff::boundaryTermMovingP0(pe, d, edgeIn, edgeOut);
            }
        }
    }
    return total;
}

}  // namespace

TEST(DiffBoundaryImage, SumOverPixelsMatchesARenderedFiniteDifference) {
    // Wound so that the interior is on the NEGATIVE side of each edge's
    // normal (Task 3's convention). Verified by the oracle rather than
    // asserted: a wrong winding shows up as an exact sign flip, which is how
    // Task 3's own sign error was found.
    const Tri2D tri = {{{1.7f, 1.3f}, {2.1f, 6.4f}, {6.8f, 3.9f}}};
    constexpr std::uint32_t kW = 8, kH = 8, kSub = 256;
    constexpr double kLTri = 3.0, kLEnv = 0.5;
    constexpr std::uint32_t kMoved = 0u;
    const float d[2] = {1.0f, 0.0f};

    const double boundary = imageBoundaryTerm(tri, kW, kH, kLTri, kLEnv, kMoved, d);

    // THE ORACLE: a central difference on the supersampled image, which
    // shares no line of code with the estimator -- no edge, no clip, no
    // normal, no (1-u) weight.
    constexpr double kH_step = 1.0 / 32.0;
    Tri2D plus = tri, minus = tri;
    plus.v[kMoved][0] = static_cast<float>(tri.v[kMoved][0] + kH_step * d[0]);
    plus.v[kMoved][1] = static_cast<float>(tri.v[kMoved][1] + kH_step * d[1]);
    minus.v[kMoved][0] = static_cast<float>(tri.v[kMoved][0] - kH_step * d[0]);
    minus.v[kMoved][1] = static_cast<float>(tri.v[kMoved][1] - kH_step * d[1]);
    const double jPlus = imageTotalBySupersampling(plus, kW, kH, kLTri, kLEnv, kSub);
    const double jMinus = imageTotalBySupersampling(minus, kW, kH, kLTri, kLEnv, kSub);
    const double oracle = (jPlus - jMinus) / (2.0 * kH_step);

    EXPECT_GT(std::fabs(oracle), 0.5) << "the oracle is near zero, so agreement means little";
    // The oracle's own resolution is one subsample of area per pixel over a
    // step of 2/32 -- about 1/256^2 * 64 / (1/16), i.e. ~1.5e-2 absolute.
    EXPECT_NEAR(boundary, oracle, 3e-2 * std::fabs(oracle) + 2e-2)
        << "boundary sum " << boundary << " against a supersampled image derivative " << oracle;
}

TEST(DiffBoundaryImage, VanishesWhenTheJumpVanishes) {
    // No discontinuity, no boundary term -- exactly, not approximately. This
    // is spec 4.1's "for appearance-only parameters the boundary term is
    // mathematically absent" seen from the other side: it is the JUMP that
    // makes the term exist, and with lTri == lEnv there is nothing to move.
    const Tri2D tri = {{{1.7f, 1.3f}, {2.1f, 6.4f}, {6.8f, 3.9f}}};
    const float d[2] = {0.6f, -0.4f};
    EXPECT_DOUBLE_EQ(imageBoundaryTerm(tri, 8u, 8u, 2.0, 2.0, 1u, d), 0.0);
}

TEST(DiffBoundaryImage, EveryVertexMovesTheImage) {
    // NON-VACUITY over the vertices. A term that only ever accumulated for
    // one endpoint -- the (1-u) weight applied to the wrong end, say -- would
    // give zero for some vertex while still matching the oracle for another.
    const Tri2D tri = {{{1.7f, 1.3f}, {2.1f, 6.4f}, {6.8f, 3.9f}}};
    const float d[2] = {1.0f, 0.0f};
    for (std::uint32_t v = 0; v < 3u; ++v) {
        EXPECT_GT(std::fabs(imageBoundaryTerm(tri, 8u, 8u, 3.0, 0.5, v, d)), 0.1)
            << "vertex " << v << " contributes nothing";
    }
}

// ===========================================================================
// STAGE 3 -- THE SCATTER TO TWO VERTICES
// ===========================================================================

TEST(DiffBoundaryScatter, TheTwoVertexTermsSumToARigidTranslation) {
    // THE CONSERVATION IDENTITY. (1-u) + u = 1 identically, so a rigid
    // translation of the edge -- velocity d everywhere -- must equal the sum
    // of the two endpoints' terms. This is the analogue of check 44's
    // bilinear conservation: the scatter distributes an edge's contribution
    // over its two vertices and must neither create nor destroy any of it.
    //
    // The rigid term is computed INDEPENDENTLY of the two parts rather than
    // as their sum; an identity checked against the sum of its own parts is
    // not a check.
    // SEVERAL EDGES, AND THE ASYMMETRIC ONES ARE THE POINT. The first entry
    // here was the only one originally, and it is nearly SYMMETRIC about the
    // segment's midpoint -- its chord has uA + uB ~= 1, and there the
    // integral of u and a constant half-share of the chord are the SAME
    // NUMBER. A demonstration that replaced p1's integrated weight with a
    // flat half passed every assertion in this file. An identity is only as
    // strong as the cases it is evaluated on.
    const ohao::diff::PixelEdge edges[2] = {
        {{0.1f, 0.1f}, {2.4f, 2.6f}},    // chord in the FIRST part of the segment
        {{-1.4f, -1.6f}, {0.9f, 0.9f}},  // chord in the LAST part
    };
    // The edge {{-0.5,-0.25},{1.5,1.1}} was the ONLY one here originally and
    // has been REMOVED: its chord is nearly symmetric about the segment
    // midpoint, and the assertion below measured it splitting evenly to
    // within a part in 10^4. Such an edge cannot fail this identity for the
    // right reason, so keeping it would be keeping a case that cannot
    // discriminate.
    const float d[2] = {0.35f, 0.8f};
    constexpr double kIn = 2.5, kOut = 0.75;

    for (const auto& edge : edges) {
        const double p0 = ohao::diff::boundaryTermMovingP0(edge, d, kIn, kOut);
        const double p1 = ohao::diff::boundaryTermMovingP1(edge, d, kIn, kOut);
        const double rigid = ohao::diff::boundaryTermTranslating(edge, d, kIn, kOut);

        EXPECT_NEAR(p0 + p1, rigid, 1e-12 * std::fabs(rigid) + 1e-15)
            << "p0 " << p0 << " + p1 " << p1 << " against rigid " << rigid;
        // NON-VACUITY: both halves must carry something, or the identity
        // holds because one of them is zero and the scatter really goes to
        // one vertex.
        EXPECT_GT(std::fabs(p0), 1e-4);
        EXPECT_GT(std::fabs(p1), 1e-4);
        // AND THEY MUST DIFFER. Where the two shares are equal the identity
        // cannot tell an integrated weight from a flat half, which is exactly
        // how the weaker version of this test passed a corrupted one.
        EXPECT_GT(std::fabs(std::fabs(p0) - std::fabs(p1)), 1e-4 * std::fabs(rigid))
            << "this edge splits evenly, so it cannot distinguish an integrated weight "
               "from a constant half-share";
    }
}

TEST(DiffBoundaryScatter, TheSplitFollowsWhereTheChordLies) {
    // The weights are not half and half -- they are integrals of (1-u) and u
    // over the CHORD, so a chord lying near p0 gives p0 the larger share.
    // Asserting that ordering is what distinguishes an integrated weight from
    // a constant 1/2 each, which would also satisfy the conservation identity
    // above.
    //
    // This edge enters the pixel almost immediately and leaves near its
    // midpoint, so the chord sits in the FIRST half of the segment and p0
    // must dominate.
    const ohao::diff::PixelEdge nearP0 = {{0.1f, 0.1f}, {2.4f, 2.6f}};
    const float d[2] = {1.0f, 0.0f};
    const double a0 = ohao::diff::boundaryTermMovingP0(nearP0, d, 1.0, 0.0);
    const double a1 = ohao::diff::boundaryTermMovingP1(nearP0, d, 1.0, 0.0);
    EXPECT_GT(std::fabs(a0), std::fabs(a1))
        << "the chord lies near p0, so p0 must take the larger share; got "
        << a0 << " and " << a1;

    // And the mirror image: an edge whose chord sits in the LAST part of the
    // segment gives p1 the larger share. Both directions, because a weight
    // stuck at one endpoint would pass one of them.
    const ohao::diff::PixelEdge nearP1 = {{-1.4f, -1.6f}, {0.9f, 0.9f}};
    const double b0 = ohao::diff::boundaryTermMovingP0(nearP1, d, 1.0, 0.0);
    const double b1 = ohao::diff::boundaryTermMovingP1(nearP1, d, 1.0, 0.0);
    EXPECT_GT(std::fabs(b1), std::fabs(b0))
        << "the chord lies near p1, so p1 must take the larger share; got "
        << b0 << " and " << b1;
}

TEST(DiffBoundaryScatter, ARigidTranslationAlongTheEdgeMovesNothing) {
    // Sliding the whole edge along its own direction does not move the LINE,
    // so the rigid term vanishes -- and so must both halves, since each is a
    // positive weight times the same (d.n).
    const ohao::diff::PixelEdge edge = {{-0.5f, -0.25f}, {1.5f, 1.1f}};
    const double dx = static_cast<double>(edge.p1[0]) - edge.p0[0];
    const double dy = static_cast<double>(edge.p1[1]) - edge.p0[1];
    const double len = std::sqrt(dx * dx + dy * dy);
    const float along[2] = {static_cast<float>(dx / len), static_cast<float>(dy / len)};
    EXPECT_NEAR(ohao::diff::boundaryTermTranslating(edge, along, 3.0, 1.0), 0.0, 1e-6);
    EXPECT_NEAR(ohao::diff::boundaryTermMovingP0(edge, along, 3.0, 1.0), 0.0, 1e-6);
    EXPECT_NEAR(ohao::diff::boundaryTermMovingP1(edge, along, 3.0, 1.0), 0.0, 1e-6);
}

// ===========================================================================
// STAGE 3 -- THE PARAMETERISATION, AND ITS PULLBACK
// ===========================================================================
//
// THE ORACLE IS A FINITE DIFFERENCE ON THE JACOBIAN, not a second copy of the
// chain rule. `pullback` claims to compute J^T g; the test builds J COLUMN BY
// COLUMN by differencing `apply` -- which knows nothing about derivatives --
// and multiplies. Two computations of one quantity with only `apply` in
// common, and `apply` is the thing whose derivative is at issue.

namespace {

std::vector<float> paramBase() {
    return {1.7f, 1.3f, 2.1f, 6.4f, 6.8f, 3.9f};
}

}  // namespace

TEST(DiffVertexParameterisation, PullbackIsTheJacobianTransposeTimesTheGradient) {
    ohao::diff::AffineVertexParameterisation param;
    ASSERT_TRUE(param.setBase(paramBase()));

    const std::vector<float> theta = {0.4f, -0.7f, 0.25f};
    // An arbitrary, NON-UNIFORM position gradient. Uniform would make the
    // translation columns indistinguishable from each other.
    const std::vector<float> g = {0.9f, -0.3f, 0.2f, 1.4f, -0.6f, 0.5f};

    const std::vector<float> got = param.pullback(theta, g);
    ASSERT_EQ(got.size(), ohao::diff::AffineVertexParameterisation::kParamCount);

    // J^T g, with J built by differencing `apply`.
    constexpr double kStep = 1.0 / 512.0;
    for (std::size_t k = 0; k < got.size(); ++k) {
        std::vector<float> plus = theta;
        std::vector<float> minus = theta;
        plus[k] = static_cast<float>(theta[k] + kStep);
        minus[k] = static_cast<float>(theta[k] - kStep);
        const std::vector<float> pPlus = param.apply(plus);
        const std::vector<float> pMinus = param.apply(minus);
        ASSERT_EQ(pPlus.size(), g.size());

        double want = 0.0;
        for (std::size_t i = 0; i < g.size(); ++i) {
            const double dPos = (static_cast<double>(pPlus[i]) - static_cast<double>(pMinus[i])) /
                                (2.0 * kStep);
            want += dPos * static_cast<double>(g[i]);
        }
        EXPECT_NEAR(static_cast<double>(got[k]), want, 1e-3 * std::fabs(want) + 1e-4)
            << "parameter " << k;
        EXPECT_GT(std::fabs(want), 1e-3) << "column " << k << " of the Jacobian is empty";
    }
}

TEST(DiffVertexParameterisation, TheJacobianIsNotTheIdentity) {
    // The POINT of the indirection. An identity Jacobian would make the
    // pullback a copy and the test above vacuous, so this asserts the two
    // properties that make it a real reparameterisation: three parameters
    // move six components, and the scale column depends on the base shape
    // where the translation columns do not.
    ohao::diff::AffineVertexParameterisation param;
    ASSERT_TRUE(param.setBase(paramBase()));
    EXPECT_EQ(param.vertexCount(), 3u);
    EXPECT_LT(ohao::diff::AffineVertexParameterisation::kParamCount, paramBase().size());

    const std::vector<float> theta = {0.0f, 0.0f, 0.0f};
    // A gradient pointing purely +x at every vertex. The translation picks up
    // the plain sum; the scale picks up a sum WEIGHTED by the base x, which
    // is a different number -- and would be the same if the scale column were
    // the translation's.
    const std::vector<float> g = {1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f};
    const std::vector<float> got = param.pullback(theta, g);
    EXPECT_NEAR(got[0], 3.0, 1e-5) << "dL/dtx is the plain sum of three unit x-gradients";
    EXPECT_NEAR(got[1], 0.0, 1e-5);
    // exp(0) * (1.7 + 2.1 + 6.8) = 10.6
    EXPECT_NEAR(got[2], 10.6, 1e-4) << "dL/ds weights each vertex by its base x";
    EXPECT_GT(std::fabs(got[2] - got[0]), 1.0)
        << "the scale and translation columns coincide, so this is not a reparameterisation";
}

TEST(DiffVertexParameterisation, LogScaleCannotReflectTheShape) {
    // Why log-scale and not scale. exp is strictly positive, so no value of
    // the parameter -- however large a step lands on it -- turns the shape
    // inside out. A raw scale parameter crossing zero would, and a reflected
    // triangle has its winding reversed, which flips the sign of every
    // boundary term computed from it.
    ohao::diff::AffineVertexParameterisation param;
    ASSERT_TRUE(param.setBase(paramBase()));
    // THE PROPERTY IS INVARIANCE, NOT SIGN. An earlier version of this test
    // asserted the signed area was POSITIVE, which is a claim about the base
    // triangle's winding -- it happens to be clockwise, so the area is
    // negative at every scale -- and not about the parameterisation at all.
    // What matters is that no value of the parameter CHANGES the sign.
    double reference = 0.0;
    for (float s : {-5.0f, -1.0f, 0.0f, 2.0f, 5.0f}) {
        const std::vector<float> p = param.apply({0.0f, 0.0f, s});
        ASSERT_EQ(p.size(), 6u);
        const double area = (p[2] - p[0]) * (p[5] - p[1]) - (p[4] - p[0]) * (p[3] - p[1]);
        EXPECT_NE(area, 0.0) << "the shape degenerated at log-scale " << s;
        if (reference == 0.0) {
            reference = area;
        } else {
            EXPECT_GT(area * reference, 0.0)
                << "the signed area changed sign at log-scale " << s
                << ", so the shape was reflected -- which reverses the winding and flips the "
                   "sign of every boundary term computed from it";
        }
    }
}

TEST(DiffVertexParameterisation, RejectsMalformedInput) {
    ohao::diff::AffineVertexParameterisation param;
    EXPECT_FALSE(param.setBase({}));
    EXPECT_FALSE(param.setBase({1.0f, 2.0f, 3.0f}));
    ASSERT_TRUE(param.setBase(paramBase()));
    EXPECT_TRUE(param.apply({0.0f, 0.0f}).empty()) << "wrong parameter count";
    EXPECT_TRUE(param.pullback({0.0f, 0.0f, 0.0f}, {1.0f}).empty()) << "wrong gradient length";
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
