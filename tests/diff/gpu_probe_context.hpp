#pragma once

#include "diff/grad/gradient_arena.hpp"
#include "diff/wavefront/wavefront_buffers.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace ohao::diff {

/// Camera basis for runWavefrontGenerateProbe, byte-layout-matched to
/// wf_generate.comp's Push block (see gpu_probe_context.cpp). Kept as plain
/// float arrays rather than glm::vec3 so this header does not need to pull
/// in glm just for a POD parameter block.
struct WavefrontGenerateCamera {
    float origin[3]{0.0f, 0.0f, 0.0f};
    float forward[3]{0.0f, 0.0f, -1.0f};
    float right[3]{1.0f, 0.0f, 0.0f};
    float up[3]{0.0f, 1.0f, 0.0f};
    float tanHalfFov{0.2f};
};

/// One BSDF configuration for runBsdfProbe. Byte-layout-matched to
/// shaders/diff/bsdf_probe.comp's Push block: four vec4s followed by
/// {float, float, uint, uint}. Kept as plain arrays for the same reason
/// WavefrontGenerateCamera is -- no glm in this header.
///
/// `normal`, `view` and `light` are world-space directions; `view` points
/// AWAY from the surface (it is -rayDirection), matching what
/// wf_scatter.comp hands the BSDF. `roughness`/`metallic` go through
/// shaders/includes/pbr_unpack.glsl's unpackHitPbr inside the shader, so
/// values below its 0.01 roughness floor are clamped there.
struct BsdfProbeCase {
    float normal[3]{0.0f, 0.0f, 1.0f};
    float roughness{1.0f};
    float view[3]{0.0f, 0.0f, 1.0f};
    float metallic{0.0f};
    float light[3]{0.0f, 0.0f, 1.0f};
    float specularWeight{0.0f};
    float baseColor[3]{1.0f, 1.0f, 1.0f};
    float u1{0.5f};
    float u2{0.5f};
    float uLobe{0.5f};
    std::uint32_t outIndex{0};
    std::uint32_t pad{0};
};
static_assert(sizeof(BsdfProbeCase) == 80,
              "BsdfProbeCase must match bsdf_probe.comp's Push block layout");

/// The material parameters wf_scatter.comp's BSDF needs beyond the base
/// colour (`albedo`), byte-matched to the tail of
/// WavefrontLoop::ScatterPush.
///
/// The defaults are the PURE LAMBERTIAN configuration: `specularWeight` 0
/// removes the specular lobe entirely (both from f and from the lobe
/// selection probability), leaving f = albedo/pi sampled by a cosine
/// hemisphere, whose estimator weight f*cos/pdf is exactly `albedo` -- which
/// is what keeps the pre-existing constant-albedo throughput checks (14, 17)
/// asserting exactly what they asserted before, bit for bit.
struct WavefrontScatterMaterial {
    float roughness{1.0f};
    float metallic{0.0f};
    float specularWeight{0.0f};
};

/// Floats per PATH INDEX in wf_scatter.comp's binding-7 next-event sink.
/// Must equal that shader's `kNeeSampleFloats`.
///
/// Naming each other in a comment was the whole of the tie and was not
/// enough: a mismatch is a silent wrong-slot read, not a validation error.
/// GLSL has no static_assert and the value survives into the SPV only as an
/// unnamed folded literal, so the tie is a RUNTIME one --
/// diff_gpu_probe.cpp's `checkNeeStrideTie()` reads the declaration out of
/// shaders/diff/wf_scatter.comp and refuses to run the probe at all if the
/// two numbers disagree.
inline constexpr std::uint32_t kNeeSampleFloats = 21;

/// Named offsets into one path's kNeeSampleFloats-float record. The order is
/// wf_scatter.comp's single write block, in the order it writes them.
///
/// The record deliberately carries BOTH single-strategy estimators and BOTH
/// halves of each sample's MIS partition rather than one combined radiance:
/// the two checks that matter -- next-event-only and BSDF-only converging to
/// the same integral, and the two weights at ONE direction summing to
/// exactly 1 -- are not recoverable from a combined value.
enum NeeSampleSlot : std::uint32_t {
    /// f*cos*L*V / p_env at the light sample: next-event estimation's own
    /// unbiased estimator of the direct-lighting integral, NO MIS weight
    /// applied. Three floats.
    kNeeSlotNeeUnweighted = 0,
    /// MIS weight for the ENVIRONMENT strategy at the light sample -- the
    /// weight the combined estimator multiplies kNeeSlotNeeUnweighted by.
    kNeeSlotWEnvAtLight = 3,
    /// MIS weight the BSDF strategy would carry at that SAME direction.
    /// kNeeSlotWEnvAtLight + this == 1 for every sample.
    kNeeSlotWBsdfAtLight = 4,
    /// f*cos*L*V / p_bsdf at the BSDF sample: the BSDF strategy's own
    /// unbiased estimator of the same integral. Three floats.
    kNeeSlotBsdfUnweighted = 5,
    /// MIS weight for the BSDF strategy at the BSDF sample.
    kNeeSlotWBsdfAtBsdf = 8,
    /// MIS weight the environment strategy would carry at that SAME
    /// direction. kNeeSlotWBsdfAtBsdf + this == 1 for every sample.
    kNeeSlotWEnvAtBsdf = 9,
    /// Grey environment radiance the shader recovered from the light
    /// sample's density, ScatterPush::envIntegral and the map's dimensions
    /// (shaders/includes/diff/nee.glsl's diffEnvRadianceFromPdf). This is
    /// the ONLY observable that depends on envIntegral having reached the
    /// GPU intact.
    kNeeSlotEnvRadiance = 10,
    /// env_sampling.glsl's pdfEnvMap evaluated at the BSDF sample's
    /// direction -- the MIS partner density for the BSDF strategy, and
    /// pdfEnvMap's first caller under test anywhere in this repository.
    kNeeSlotPdfEnvAtBsdf = 11,
    /// diffBsdfEval's pdf at the LIGHT sample's direction -- the MIS partner
    /// density for the environment strategy.
    kNeeSlotPdfBsdfAtLight = 12,
    /// Shadow-ray results, 1 unoccluded / 0 occluded, at the light sample
    /// and the BSDF sample respectively. 0 also when the direction was below
    /// the horizon and no ray was traced.
    kNeeSlotVisLight = 13,
    kNeeSlotVisBsdf = 14,
    /// 1 if this path took the surface branch (hit distance >= 0) in this
    /// dispatch, 0 if it took the miss guard. Every other slot is 0 when
    /// this is 0.
    kNeeSlotSurfaceBranch = 15,
    /// The BSDF sample's own direction (three floats) and the density it was
    /// drawn with. Recorded because kNeeSlotPdfEnvAtBsdf cannot be checked
    /// without the direction pdfEnvMap was asked about -- see
    /// wf_scatter.comp's write block.
    kNeeSlotBsdfDir = 16,
    kNeeSlotPdfBsdfAtBsdf = 19,
    /// The grey environment radiance the BSDF strategy multiplied in at its
    /// own sampled direction, recovered by the shader from
    /// `pdfEnvMapTexel` rather than from `pdfEnvMap` (kNeeSlotPdfEnvAtBsdf).
    /// The two differ by sin(theta_centre)/sin(theta_query), so the density
    /// at slot 11 cannot tell a correct recovery from an off-by-that-factor
    /// one; this slot can, and check 31 asserts it against the environment
    /// image texel by texel.
    kNeeSlotBsdfRadiance = 20,
};

/// Occluder geometry for the shadow rays wf_scatter.comp's next-event
/// estimator traces. `positions` is 3 floats per vertex, `indices` 3 uints
/// per triangle -- the same packing runWavefrontIntersectOnGeometry takes.
///
/// EMPTY MEANS "NO OCCLUDERS", not "no acceleration structure": a
/// VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR binding cannot be
/// VK_NULL_HANDLE without the nullDescriptor feature, which this context
/// does not enable, so "nothing blocks the environment" has to be spelled as
/// geometry no shadow ray can reach. runWavefrontScatterProbe substitutes a
/// single triangle a million units away -- three orders of magnitude beyond
/// the shader's own kShadowTMax of 1000 -- for an empty scene.
struct WavefrontShadowScene {
    std::span<const float> positions;
    std::span<const uint32_t> indices;

    [[nodiscard]] bool empty() const noexcept {
        return positions.empty() || indices.empty();
    }
};

/// Headless Vulkan context for standalone GPU probe executables.
///
/// Owns a VkInstance, VkDevice (with the differentiable-renderer device
/// extensions enabled unconditionally -- this binary is meant to fail loudly
/// on hardware that cannot run the subsystem, not degrade gracefully), a
/// GpuAllocator, a command pool, and one compute-capable queue.
class GpuProbeContext {
public:
    GpuProbeContext() = default;
    // init() can return false after having already created the instance,
    // messenger, device, and/or command pool -- shutdown() is idempotent and
    // safely tears down whatever partial state exists, so running it from
    // the destructor closes that leak even when a caller's `if (!ctx.init())
    // return 1;` never reaches an explicit ctx.shutdown().
    ~GpuProbeContext() { shutdown(); }

    // Not copyable or movable: the compiler-generated versions would copy
    // the raw Vulkan handles, leaving two objects that each believe they own
    // (and will each destroy) the same instance/device/pool.
    GpuProbeContext(const GpuProbeContext&) = delete;
    GpuProbeContext& operator=(const GpuProbeContext&) = delete;
    GpuProbeContext(GpuProbeContext&&) = delete;
    GpuProbeContext& operator=(GpuProbeContext&&) = delete;

    [[nodiscard]] bool init();
    void shutdown();

    [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept { return m_physicalDevice; }
    // Exposed so callers (diff_gpu_probe.cpp's Task 1 ComputePipeline sanity
    // check, and Task 4's further migration) can build ohao::diff library
    // objects -- e.g. ComputePipeline -- directly against this context's
    // device without every such object needing its own device-creation path.
    [[nodiscard]] VkDevice device() const noexcept { return m_device; }
    [[nodiscard]] GpuAllocator& allocator() noexcept { return m_allocator; }

    /// Allocates a one-time-submit primary command buffer, records `fn`,
    /// submits it on the compute queue, and waits for completion.
    void runImmediate(const std::function<void(VkCommandBuffer)>& fn);

    /// Loads shaders/diff/atomic_probe.comp's SPIR-V, binds `arena.buffer()`
    /// at binding 0, pushes {targetIndex, invocations}, and dispatches
    /// ceil(invocations / 64) groups. Returns false on any Vulkan error.
    [[nodiscard]] bool runAtomicProbe(GradientArena& arena, uint32_t targetIndex,
                                      uint32_t invocations);

    /// Runs shaders/diff/rng_probe.comp for one path and returns its first
    /// `drawCount` RNG values. Compared bit-exactly against ohao::diff::PathRng,
    /// this is what proves the GLSL mirror in shaders/includes/diff/rng.glsl
    /// agrees with the CPU reference. Path replay backpropagation replays each
    /// light path from its seed instead of storing a tape, so a single differing
    /// bit means the backward pass walks a different path than the forward pass
    /// and every gradient is silently wrong.
    [[nodiscard]] bool runRngParityProbe(uint32_t pixelIndex, uint32_t sampleIndex,
                                         uint32_t iterationSeed, uint32_t drawCount,
                                         GradientArena& scratch, std::size_t blockIndex,
                                         std::vector<float>& outDraws);

    /// Builds a BLAS/TLAS for a single axis-aligned quad spanning
    /// x in [-1,1], y in [quadMinY,1] at z = -planeDistance, traces one ray
    /// per pixel from the origin looking down -Z, and fills `outHits` with
    /// width*height distances (-1.0 on miss). `quadMinY` defaults to -1.0
    /// (a full-frustum-covering quad, so every ray hits); passing 0.0 makes
    /// only the top half of the quad present, which turns hit/miss into an
    /// orientation-sensitive signal -- see diff_gpu_probe.cpp's half-quad
    /// check for why the plain distance check alone can't catch a flipped
    /// camera convention.
    /// Returns false on any Vulkan error.
    [[nodiscard]] bool runVisibilityProbe(float planeDistance, uint32_t width, uint32_t height,
                                          float tanHalfFov, std::vector<float>& outHits,
                                          float quadMinY = -1.0f);

    /// Runs shaders/diff/bsdf_probe.comp for ONE BSDF configuration,
    /// writing 12 floats into `sink`'s block `blockIndex` at
    /// `probeCase.outIndex * 12` (see bsdf_probe.comp's header for the
    /// slot layout). One dispatch per case; the caller reads the whole
    /// block back once at the end.
    ///
    /// The shader calls the same shaders/includes/diff/bsdf.glsl entry
    /// points wf_scatter.comp calls, so this observes the production BSDF,
    /// not a copy of it.
    [[nodiscard]] bool runBsdfProbe(GradientArena& sink, const BsdfProbeCase& probeCase);

    /// Runs shaders/diff/wf_generate.comp over a `width` x `height` grid of
    /// pixels (one path per pixel), writing origin/dir/throughput/radiance/
    /// pixelIndex/sampleIndex/bounce/alive into `buffers`' state arena and
    /// pushing each path index into queue 0. `buffers.layout().capacity()`
    /// is what the shader derives every field's offset from -- see
    /// path_state.glsl's header comment for why this is a single pushed
    /// uint rather than 16 precomputed offsets.
    ///
    /// `outQueue0` receives a host-side copy of exactly queue 0's region
    /// (capacity elements): WavefrontBuffers exposes only a raw VkBuffer for
    /// the queue, not the GpuBuffer wrapper GpuAllocator::invalidateBuffer
    /// needs, so this copies queue 0 out into a buffer this function owns
    /// via vkCmdCopyBuffer before mapping and reading that copy.
    ///
    /// Returns false on any Vulkan error. Caller reads back state fields and
    /// the counter directly through `buffers` afterwards.
    [[nodiscard]] bool runWavefrontGenerateProbe(WavefrontBuffers& buffers, uint32_t width,
                                                 uint32_t height,
                                                 const WavefrontGenerateCamera& camera,
                                                 std::vector<uint32_t>& outQueue0);

    /// Runs shaders/diff/wf_layout_probe.comp: a single invocation that
    /// writes a distinct, non-degenerate value to every PathStateField of
    /// path index 0 in `buffers`' state arena (float fields get
    /// 1000+fieldIndex, integer fields get 7000+fieldIndex), through the
    /// same psSet* accessors the real wavefront stages use.
    ///
    /// This exists because wf_generate.comp's round-trip check writes
    /// genuinely degenerate values (throughput (1,1,1), radiance (0,0,0),
    /// sampleIndex/bounce both 0), so a transposition *within* a group of
    /// same-valued fields would round-trip undetected there. Every field
    /// here has a unique value, so any permutation of the field->offset
    /// mapping between path_state_layout.hpp and path_state.glsl is caught.
    ///
    /// Returns false on any Vulkan error. Caller reads state fields back
    /// through `buffers.readbackField`.
    [[nodiscard]] bool runWavefrontLayoutProbe(WavefrontBuffers& buffers);

    /// Runs the wavefront intersect stage end to end: shaders/diff/
    /// wf_prepare_indirect.comp converts `buffers`' counter slot
    /// kCurrentCountSlot into a {groupCountX,1,1} vkCmdDispatchIndirect
    /// triple at counter slot kIndirectArgsSlot, then shaders/diff/
    /// wf_intersect.comp is dispatched indirectly from that triple, tracing
    /// queue ring 0 against a BLAS/TLAS built for a quad spanning
    /// x in [-1,1], y in [quadMinY,1] at z=-planeDistance -- the same
    /// half-quad geometry runVisibilityProbe/the half-quad orientation check
    /// use. Survivors are compacted into queue ring 1 / counter slot
    /// kNextCountSlot; every invocation that runs increments counter slot
    /// kCanarySlot.
    ///
    /// `buffers` must already hold a populated queue ring 0 / counter slot
    /// kCurrentCountSlot (e.g. from a prior, separately-submitted-and-waited
    /// runWavefrontGenerateProbe call) -- this call does not populate path
    /// state itself, and a counter slot 0 of exactly 0 is a valid input
    /// (the empty-queue / zero-cost-dispatch case).
    ///
    /// UNDOCUMENTED-UNTIL-NOW PRECONDITION: `buffers`' counter slot
    /// kNextCountSlot -- the compaction DESTINATION this call hardcodes and,
    /// unlike runWavefrontScatterProbe, never zeroes itself -- must already
    /// be 0 on entry. This currently always holds, but only by construction
    /// of every existing caller, not by anything this function enforces:
    /// `WavefrontBuffers::zero()` zeroes the whole counter buffer,
    /// `wf_generate.comp` atomicAdds only into slot 0 (kCurrentCountSlot),
    /// and all three call sites in this codebase run this probe exactly
    /// once per `zero()`. Call it a second time on the same `buffers`
    /// without an intervening zero, or pair it with a scatter call whose
    /// `dstCountSlot == kNextCountSlot`, and wf_intersect.comp's
    /// atomicAdd(counters.value[kNextCountSlot], 1u) starts from a stale,
    /// non-zero base -- silently displacing every compaction offset after
    /// the first, exactly the "left 1024 live paths, expected all 512"
    /// failure mode measured in the fused loop, with no diagnostic.
    ///
    /// Unlike every other probe here, wf_prepare_indirect.comp's dispatch,
    /// the barrier ordering its writes before vkCmdDispatchIndirect reads
    /// them, and wf_intersect.comp's indirect dispatch are all recorded on
    /// ONE command buffer with no `vkQueueWaitIdle` between them -- that
    /// full-device-idle wait is what silently hides a missing
    /// SHADER_WRITE -> INDIRECT_COMMAND_READ barrier in every earlier probe.
    ///
    /// `outQueue1` receives a host-side copy of exactly queue ring 1's
    /// `capacity`-element region (same reasoning as
    /// runWavefrontGenerateProbe's outQueue0). Returns false on any Vulkan
    /// error. Caller reads state fields and counters back through `buffers`
    /// afterwards.
    [[nodiscard]] bool runWavefrontIntersectProbe(WavefrontBuffers& buffers, float planeDistance,
                                                  float quadMinY, std::vector<uint32_t>& outQueue1);

    /// Exactly runWavefrontIntersectProbe's dispatch (same shaders, same
    /// barriers, same compaction ring 0 -> ring 1), but against the CLOSED
    /// AXIS-ALIGNED BOX of half-extent `halfExtent` centred on the origin,
    /// wound so that every face's winding-order normal points OUT of the box
    /// -- see buildAxisAlignedBoxGeometry in the .cpp.
    ///
    /// This is the scene the geometric-normal check needs and the single
    /// quad cannot supply: a quad viewed from the -Z side yields the one
    /// normal, (0,0,1), that the placeholder `const vec3 normal =
    /// vec3(0,0,1)` in wf_scatter.comp happened to be correct for, so a
    /// check built on it could not distinguish a real normal from the
    /// hardcoded one. Five of the box's six faces are reachable from a
    /// camera at its centre, giving five distinct analytic normals; and
    /// because the outward winding is the opposite of what a ray leaving
    /// the interior needs, every one of them also exercises
    /// wf_intersect.comp's flip-to-oppose-the-ray step.
    ///
    /// Preconditions and outputs are runWavefrontIntersectProbe's, including
    /// its counter-slot precondition (`kNextCountSlot` must be 0 on entry).
    /// Every path hits -- a ray from strictly inside a closed convex body
    /// always leaves it through a face -- so `outQueue1` receives all
    /// `capacity` path indices.
    [[nodiscard]] bool runWavefrontBoxIntersectProbe(WavefrontBuffers& buffers, float halfExtent,
                                                     std::vector<uint32_t>& outQueue1);

    /// Runs the wavefront scatter stage (shaders/diff/wf_scatter.comp) for
    /// one bounce: wf_prepare_indirect.comp converts counter slot
    /// `srcCountSlot` into a dispatch-indirect triple (same pattern as
    /// runWavefrontIntersectProbe -- one command buffer, the
    /// SHADER_WRITE -> INDIRECT_COMMAND_READ barrier between them), then
    /// wf_scatter.comp is dispatched indirectly, reading queue ring
    /// `srcQueueBase`/`srcCountSlot`, multiplying throughput by `albedo`,
    /// sampling a new direction from a per-path RNG reconstructed from
    /// (pixelIndex, sampleIndex, `iterationSeed`) and fast-forwarded by the
    /// path's stored bounce count, and re-queuing every path (nothing
    /// terminates in scatter yet) into `dstQueueBase`/`dstCountSlot`.
    ///
    /// Unlike wf_intersect, which this stage's checks only ever run
    /// ring0->ring1 once, a multi-bounce loop must ping-pong the SAME two
    /// physical rings across many scatter calls -- so `dstCountSlot` is
    /// explicitly zeroed (vkCmdFillBuffer + a barrier ordering that fill
    /// before the indirect dispatch's atomicAdd) at the start of this
    /// method's own command buffer, every call, rather than assumed to
    /// already be 0. Reusing a stale prior count as the atomicAdd base would
    /// silently corrupt compaction offsets.
    ///
    /// `outQueueDst` receives a host-side copy of queue ring `dstQueueBase`'s
    /// `capacity`-element region (same reasoning as
    /// runWavefrontGenerateProbe's outQueue0). `outDebugDraws` receives a
    /// host-side copy of the probe-only DebugDraws buffer (binding 3,
    /// `capacity * 3` floats: per path index, (u1, u2,
    /// uintBitsToFloat(rng.draws)) as computed by THIS dispatch) -- the only
    /// way to verify RNG values a real GPU dispatch produced, bit-exactly,
    /// against ohao::diff::PathRng.
    ///
    /// `outEnvSamples`, when non-null, receives the other probe-only sink
    /// wf_scatter.comp writes: binding 6, `capacity * 4` floats holding the
    /// (dirX, dirY, dirZ, pdf) `shaders/includes/rt/env_sampling.glsl`'s
    /// sampleEnvMap returned for each path index, against the CDF currently
    /// uploaded into `buffers` (bindings 4 and 5, read-only). It is a
    /// pointer with a nullptr default rather than a reference so the callers
    /// that have no interest in environment sampling are unchanged; the
    /// buffer is allocated, bound and written either way, because the
    /// descriptor set is not optional. Returns false on any Vulkan error.
    ///
    /// `shadowScene` is the occluder geometry the stage's next-event
    /// estimator traces shadow rays against (binding 8). It defaults to
    /// empty, which this function realises as unreachable geometry rather
    /// than a null acceleration structure -- see WavefrontShadowScene. A
    /// caller that wants the estimator's visibility term to mean anything
    /// must pass the SAME geometry it traced the primary rays against.
    ///
    /// `outNeeSamples`, when non-null, receives binding 7: `capacity *
    /// kNeeSampleFloats` floats per dispatch, indexed by path index and laid
    /// out by NeeSampleSlot. Like outEnvSamples it is a pointer with a
    /// nullptr default -- the buffer is allocated, bound and written either
    /// way, because the descriptor set is not optional.
    [[nodiscard]] bool runWavefrontScatterProbe(WavefrontBuffers& buffers, uint32_t srcQueueBase,
                                                uint32_t srcCountSlot, uint32_t dstQueueBase,
                                                uint32_t dstCountSlot, float albedo,
                                                uint32_t iterationSeed,
                                                std::vector<uint32_t>& outQueueDst,
                                                std::vector<float>& outDebugDraws,
                                                const WavefrontScatterMaterial& material = {},
                                                std::vector<float>* outEnvSamples = nullptr,
                                                const WavefrontShadowScene& shadowScene = {},
                                                std::vector<float>* outNeeSamples = nullptr);

    /// Runs wf_prepare_indirect.comp + an indirectly-dispatched
    /// wf_intersect.comp over queue ring 0 against an ARBITRARY triangle
    /// soup, compacting survivors into ring 1 and copying that ring out.
    ///
    /// `positions` is 3 floats per vertex and `indices` 3 uints per
    /// triangle; BOTH are also bound to wf_intersect.comp as storage buffers
    /// (bindings 3 and 4), which is how it recovers the hit triangle's
    /// vertices to compute a geometric normal.
    ///
    /// runWavefrontIntersectProbe (a single quad) and
    /// runWavefrontBoxIntersectProbe (a closed box) are the two named scenes
    /// built on it. This entry point is public because a caller that needs
    /// its OWN scene also needs to hand the same triangles to
    /// runWavefrontScatterProbe's `shadowScene`, so that the primary rays
    /// and the shadow rays see one geometry rather than two that happen to
    /// agree.
    [[nodiscard]] bool runWavefrontIntersectOnGeometry(WavefrontBuffers& buffers,
                                                       std::span<const float> positions,
                                                       std::span<const uint32_t> indices,
                                                       std::vector<uint32_t>& outQueue1);

    /// Runs the WHOLE wavefront bounce loop through ohao::diff::WavefrontLoop
    /// -- generate, then prepare_indirect/intersect/prepare_indirect/scatter
    /// once per bounce -- fused into ONE command buffer per run, with no
    /// vkQueueWaitIdle anywhere inside it. This is the only probe here whose
    /// stage ordering is the barriers' job rather than a full-device idle
    /// wait's; see wavefront_loop.hpp for the ordering contract it exercises.
    ///
    /// Scene: the CLOSED axis-aligned box of half-extent
    /// `kFusedLoopBoxHalfExtent`, entered from its centre (see the
    /// anonymous-namespace section in the .cpp, which carries the full
    /// survival derivation). This replaced a staircase of parallel quads
    /// that only worked while wf_scatter.comp hardcoded the surface normal
    /// to (0,0,1): with that constant every scattered ray had dir.z > 0
    /// whatever it hit, so paths marched monotonically in +Z and a stack of
    /// planes caught each in turn. A REAL forward-facing normal always
    /// opposes the incoming ray, so a path now bounces back off the plane it
    /// hits -- and a staircase becomes precisely the scene whose survival is
    /// a grazing-angle probability rather than a fact.
    ///
    /// A closed convex body has no such failure mode: a ray from strictly
    /// inside it always leaves through a face, at a distance no greater than
    /// the body's longest chord, and the scatter stage's offset along the
    /// inward normal puts the next origin strictly inside again. The
    /// guarantee is therefore UNIFORM in the bounce count rather than
    /// decaying with it -- see the static_asserts next to the fused-loop
    /// scene constants in the .cpp (space diagonal vs. wf_intersect.comp's
    /// tMax, scatter offset vs. half-extent both above AND below float
    /// resolution at the box's scale, camera inside the box), which enforce
    /// every one of those conditions at BUILD time since none of them
    /// depends on `maxBounces`; only `maxBounces >= 1` is still checked at
    /// runtime, alongside this probe's dispatch-shape requirements.
    ///
    /// This does NOT make the throughput assertion (check 17 in
    /// diff_gpu_probe.cpp) pass vacuously, and it is not check 16 (the
    /// survivor/compaction check) that would prevent that. Check 17 iterates
    /// every one of `kCapacity` PathState entries, not the survivor ring --
    /// a dead path's Throughput field still holds whatever an earlier bounce
    /// left in it (e.g. 0.5 after one bounce), so check 17 fails LOUDLY on a
    /// scene that kills paths, regardless of check 16. This was proved for
    /// the previous scene by stubbing both of check 16's assertions and
    /// rebuilding: a single-quad run still failed at check 17 with
    /// "throughput = (0.5,0.5,0.5) after 4 bounces, expected exactly
    /// (0.0625,0.0625,0.0625)". Non-vacuity comes from check 17's
    /// quantification over the whole path-state array, not from check 16.
    ///
    /// A scene every path survives is still necessary -- this probe
    /// genuinely cannot pass without one, since the intended bit-exact
    /// 0.0625 outcome is only reachable if nothing dies early -- which is
    /// what makes "throughput is exactly albedo^bounces for every path" a
    /// real, non-vacuous assertion rather than a loud failure.
    ///
    /// `height` must be exactly 8 and `width` a multiple of 8. This is a
    /// CALIBRATION constraint, not a capability one: every expected value
    /// this probe asserts (the 0.0625 throughput, the per-bounce PathRng
    /// parity for path 333, the live counts, the one-of-each ring check) is
    /// computed for exactly 512 paths at 64x8, so changing the resolution
    /// would silently invalidate them. `WavefrontStage::Fixed` carries
    /// `groupsY`/`groupsZ` and can dispatch a genuine 3-D grid, so a stage
    /// recorded through `WavefrontLoop` CAN cover any resolution; this probe
    /// simply leaves them at 1 and dispatches (width/8, 1, 1) x local_size
    /// (8,8), which covers exactly 8 pixel rows. Widening it means
    /// recomputing the expected values, not changing the dispatch source.
    ///
    /// Runs the loop `maxBounces` times, with 1, 2, ... `maxBounces`
    /// bounces, re-zeroing `buffers` at the top of each run. Every run is
    /// itself fully fused; the only reason for more than one is
    /// observability: wf_scatter.comp writes its (u1, u2, drawCount)
    /// diagnostics at a fixed `pathIndex*3` offset, so within a single fused
    /// run only the LAST bounce's draws survive in the buffer. Running with
    /// b bounces therefore exposes bounce b-1's draws, and the set of runs
    /// exposes every bounce's -- with no shader change and no weakening of
    /// the per-bounce RNG-parity assertion.
    ///
    /// `outDrawsPerBounce[b]` receives the `capacity*3` DebugDraws floats
    /// bounce b produced. `outLiveCountPerRun[b]` receives the live-path
    /// count left after the run of b+1 bounces. `outFinalQueue` receives the
    /// live ring's `capacity` elements after the FINAL (maxBounces) run, and
    /// `buffers` is left holding that run's path state, so the caller reads
    /// throughput back through `buffers.readbackField` afterwards.
    ///
    /// `outEnvSamples`, when non-null, receives binding 6's `capacity * 4`
    /// floats (dirX, dirY, dirZ, pdf per path index) after the FINAL run --
    /// the env-sample sink every scatter dispatch in that run wrote to, at a
    /// fixed pathIndex*4 offset, so only the last bounce's write survives
    /// (same reasoning as outDrawsPerBounce, but this sink is not re-read
    /// bounce by bounce because nothing here needs the intermediate values).
    /// Unlike runWavefrontScatterProbe, which fills wf_scatter.comp's
    /// ScatterPush envWidth/envHeight/envIntegral fields BY HAND at its own
    /// call site, this probe's push constants are filled by
    /// ohao::diff::WavefrontLoop::record itself from `buffers` -- this is
    /// the only test path that exercises record()'s fill of those three
    /// fields, which is otherwise unobserved. Default nullptr, matching
    /// runWavefrontScatterProbe's outEnvSamples, so existing callers are
    /// unchanged; the buffer is allocated, bound and written either way.
    ///
    /// Returns false on any Vulkan error.
    /// `outNeeSamples`, when non-null, receives binding 7 after the FINAL
    /// run: `capacity * kNeeSampleFloats` floats laid out by NeeSampleSlot.
    /// The scene is the CLOSED box, so every shadow ray this probe's
    /// next-event estimator traces is occluded and every contribution in
    /// that record must be exactly zero -- which is what makes it the check
    /// that the shadow ray is traced at all, as opposed to a visibility term
    /// silently stuck at 1.
    [[nodiscard]] bool runWavefrontFusedLoopProbe(WavefrontBuffers& buffers, uint32_t width,
                                                  uint32_t height, uint32_t maxBounces,
                                                  float albedo, uint32_t iterationSeed,
                                                  std::vector<std::vector<float>>& outDrawsPerBounce,
                                                  std::vector<uint32_t>& outLiveCountPerRun,
                                                  std::vector<uint32_t>& outFinalQueue,
                                                  std::vector<float>* outEnvSamples = nullptr,
                                                  std::vector<float>* outNeeSamples = nullptr);

private:
    /// Shared boilerplate for the single-storage-buffer compute probes:
    /// load SPIR-V, one STORAGE_BUFFER at binding 0, push constants, dispatch,
    /// barrier to host reads, wait. Every object is destroyed on every path.
    [[nodiscard]] bool dispatchStorageBufferCompute(const char* spvName, VkBuffer buffer,
                                                    const void* pushData, uint32_t pushSize,
                                                    uint32_t groupCountX);

    VkInstance m_instance{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    VkDevice m_device{VK_NULL_HANDLE};
    VkQueue m_queue{VK_NULL_HANDLE};
    uint32_t m_queueFamily{0};
    VkCommandPool m_commandPool{VK_NULL_HANDLE};
    GpuAllocator m_allocator;

    // Best-effort validation: present only when VK_LAYER_KHRONOS_validation is
    // available on the host. See init()/shutdown() -- absence is a warning,
    // never a failure.
    VkDebugUtilsMessengerEXT m_debugMessenger{VK_NULL_HANDLE};
    bool m_validationEnabled{false};
};

}  // namespace ohao::diff
