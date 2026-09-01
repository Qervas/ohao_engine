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

/// Stage 1 Task 3 -- what a gradient run differentiates, and whether it holds
/// its sampled directions still while doing it.
///
/// THE SECOND FIELD IS THE INSTRUMENT. Spec section 6.3 lists sampled
/// directions as NOT differentiated, so the adjoint computes the derivative of
/// the estimator at FIXED directions. Perturbing roughness or metallic and
/// re-running the sampler moves the GGX VNDF's alpha and the lobe-selection
/// probability, so a naive common-random-number difference measures that
/// derivative PLUS the movement of every direction -- the term the adjoint
/// deliberately omits. `freezeSampling` pushes `WavefrontLoop::Config`'s
/// sampling-material override, so the +h and -h renders draw from the SAME
/// material as the unperturbed one and only `f` and the densities move. That
/// makes the difference quotient and the adjoint two computations of one
/// quantity.
///
/// Running the same measurement with it OFF is the detached-sampling BIAS,
/// which is a number to report rather than a gate to pass.
struct WavefrontGradientOptions {
    /// 0 = base colour (Stage 1 Task 2), 1 = roughness, 2 = metallic. Matches
    /// DIFF_PARAM_* in shaders/includes/diff/bsdf_adjoint.glsl.
    std::uint32_t diffParam{0};
    /// When true, `samplingAlbedo`/`samplingMaterial` are what every sampling
    /// decision uses, regardless of the evaluated material passed alongside.
    bool freezeSampling{false};
    float samplingAlbedo{0.0f};
    WavefrontScatterMaterial samplingMaterial{};
    /// Optional: receives the FORWARD run's binding-3 vertex trace as it stood
    /// after the LAST bounce (`capacity * kDebugDrawFloats` floats). It is how
    /// a caller MEASURES the frozen-direction claim instead of asserting it:
    /// slots 6-8, 9-11 and 15 are the ray origin, the ray direction and the
    /// hit distance the traversal read out of path state, so two renders whose
    /// paths did not move produce bit-identical values there.
    std::vector<float>* outForwardTrace{nullptr};
    /// Stage 1 Task 4. The uniform self-emission scalar, pushed to BOTH the
    /// forward and replay runs verbatim (`WavefrontLoop::Config::emission`),
    /// exactly as `albedo` is. Not part of `WavefrontScatterMaterial` because
    /// it is not read by the BSDF at all -- it is a property of the FILM,
    /// added by the forward hook, not a property of the surface the BSDF
    /// evaluates. Defaults to 0.0, so an `options` left at `{}` renders the
    /// film every caller that predates this task already expects.
    float emission{0.0f};
    /// Stage 1 Task 5. The EMISSION TEXTURE's primal values, uploaded to a
    /// fresh buffer bound at the traversal's binding 11 for BOTH runs, and
    /// its shape and uv map (`WavefrontLoop::Config`'s seven matching
    /// fields).
    ///
    /// Empty (the default) means "no texture": width/height are then pushed
    /// as 0, the forward hook adds the `emission` scalar above exactly as it
    /// did before this task, and a placeholder buffer is bound at 11 so the
    /// descriptor set still covers every binding the shader declares.
    ///
    /// The values are indexed by `ohao::diff::ParamShape::elementIndex` --
    /// `(y*width + x)*channels + c` -- which is the SAME ordering the shader
    /// addresses both this array and the gradient block by. A caller
    /// perturbing one texel element for a finite difference perturbs
    /// `emissionTexture[shape.elementIndex(x, y, c)]` and compares against
    /// the arena float at the same k.
    std::vector<float> emissionTexture;
    std::uint32_t emissionTexWidth{0};
    std::uint32_t emissionTexHeight{0};
    std::uint32_t emissionTexChannels{0};
    float emissionUvScaleU{0.0f};
    float emissionUvScaleV{0.0f};
    float emissionUvBiasU{0.0f};
    float emissionUvBiasV{0.0f};
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
inline constexpr std::uint32_t kNeeSampleFloats = 25;

/// Floats per PATH INDEX in the traversal's binding-3 VERTEX TRACE sink and
/// in its binding-6 environment-sample sink (a unit direction plus the
/// density it was drawn with).
///
/// Named for the same reason kNeeSampleFloats is, and tied the same way:
/// diff_gpu_probe.cpp's `checkWfScatterSinkLayoutTie()` parses the shader's
/// own writes into those two buffers and refuses to run the probe unless the
/// stride, the set of written offsets AND (for binding 3 and binding 7) the
/// expression written into each slot match these numbers and the enums below
/// exactly. They were bare `3u`/`4u` literals on both sides of the boundary
/// until then -- the same untied shape kNeeSampleFloats was called out for,
/// with the same silent-wrong-slot-read failure mode.
///
/// kDebugDrawFloats was 3 through Stage 0b-2b, holding (u1, u2, drawCount).
/// Stage 1 Task 1 widened it to a full per-vertex record (see TraceSlot).
/// Slots 0, 1 and 2 still carry exactly those three values, so checks 15 and
/// 18 -- which have read them since Stage 0b-1 and index through this
/// constant -- are untouched by the widening.
inline constexpr std::uint32_t kDebugDrawFloats = 18;
inline constexpr std::uint32_t kEnvSampleFloats = 4;

/// RNG draws the traversal takes per bounce -- shaders/includes/diff/traverse.glsl's
/// own `kDrawsPerBounce`. Three for the BSDF (a 2-D direction sample plus a
/// 1-D lobe choice) and two for the environment sample.
///
/// A HOST constant rather than a per-check literal, and TIED to the shader by
/// diff_gpu_probe.cpp's `checkDrawsPerBounceTie()`, because it is what the
/// CPU-side PathRng oracle fast-forwards by: get it wrong and the oracle
/// walks a DIFFERENT stream than the shader, which is the exact failure the
/// checks that use it exist to detect -- so they would be comparing two
/// wrong things and could agree. The count must also stay independent of
/// which lobe was chosen and of whether the path hit anything, which is why
/// the traversal draws the lobe sample unconditionally and takes both
/// environment draws before its miss guard.
inline constexpr std::uint32_t kDrawsPerBounce = 5;

/// Named offsets into one path's kDebugDrawFloats-float VERTEX TRACE record,
/// binding 3 of shaders/includes/diff/traverse.glsl.
///
/// WHAT THIS RECORD IS FOR. It is the observable that makes REPLAY
/// EQUIVALENCE checkable at all. Two instantiations of the traversal --
/// shaders/diff/wf_scatter.comp and shaders/diff/wf_scatter_replay.comp --
/// must walk the identical path, consuming the identical RNG values in the
/// identical order (spec section 6.2); divergence by one draw silently
/// invalidates every gradient a later task computes on the replayed path.
/// This record is what each of them writes about the vertex it reached, and
/// the two are compared bit for bit.
///
/// It therefore holds ALL FIVE of the bounce's draws, not just the two the
/// old debug sink carried, and the ray, throughput and hit distance the
/// traversal read OUT OF PATH STATE before overwriting them -- the four
/// quantities "the replay reconstructs the same vertex" is a statement about.
/// Nothing here is handed to the shader: every value is either a draw from
/// the RNG rebuilt out of (pixelIndex, sampleIndex, iterationSeed, bounce) or
/// a field of path state.
enum TraceSlot : std::uint32_t {
    /// The bounce's first two uniform draws -- the BSDF's 2-D direction
    /// sample. Slots 0-2 are Stage 0b-1's original debug record, unmoved.
    kTraceSlotU1 = 0,
    kTraceSlotU2 = 1,
    /// The RNG's draw count AFTER this bounce's draws, bit-cast to float.
    /// The tripwire spec section 6.2 names: forward and backward must consume
    /// the same NUMBER of draws at every bounce, and that is assertable per
    /// stage rather than only per path.
    kTraceSlotDrawCount = 2,
    /// The remaining three draws: the BSDF's lobe-selection sample and the
    /// environment sample's two uniforms. Recorded because a divergence in
    /// draws 3-5 moves every LATER bounce's stream while leaving this
    /// bounce's u1/u2 identical -- the failure mode a two-value record
    /// cannot see.
    kTraceSlotULobe = 3,
    kTraceSlotUEnv1 = 4,
    kTraceSlotUEnv2 = 5,
    /// The ray that PRODUCED this vertex, as read from path state before the
    /// traversal advanced the path. Three floats each.
    kTraceSlotOrigin = 6,
    kTraceSlotDir = 9,
    /// The path's throughput on ARRIVAL, before this vertex's f*cos/pdf
    /// decay. Three floats. Distinct from NeeSampleSlot's
    /// kNeeSlotArrivalThroughput, which is zeroed on the miss path; this one
    /// is the raw path-state field and is defined on every path.
    kTraceSlotThroughput = 12,
    /// Hit distance, -1 on a miss.
    kTraceSlotHitT = 15,
    /// The bounce index this record is for, bit-cast to float. Lets the host
    /// verify it is comparing bounce b of one run against bounce b of the
    /// other rather than trusting the dispatch bookkeeping that produced
    /// them.
    kTraceSlotBounce = 16,
    /// psGetPixelIndex, bit-cast to float. The film-hazard enforcement reads
    /// this: exactly one path per pixel per dispatch (see
    /// wf_scatter.comp's hook, "WHICH OPTION THIS SUBSYSTEM TOOK").
    kTraceSlotPixelIndex = 17,
};

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
    /// The path's throughput ON ARRIVAL at this vertex -- before this
    /// bounce's f*cos/pdf decay. Three floats. Recorded because it is the
    /// one factor of wf_scatter.comp's film contribution that is otherwise
    /// unobservable per bounce: path state holds only the CURRENT (already
    /// decayed) throughput, and in a fused multi-bounce run the earlier
    /// bounces' values are gone by the time the host reads anything.
    kNeeSlotArrivalThroughput = 21,
    /// The pixel this path's radiance is accumulated into -- psGetPixelIndex,
    /// stored as a float (exact for any capacity below 2^24). The film is
    /// indexed by PIXEL, not by path, so a host oracle that reconstructs the
    /// film has to be told the mapping rather than assume pathIndex ==
    /// pixelIndex, which holds only at one sample per pixel.
    kNeeSlotPixelIndex = 24,
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
    ///
    /// The FILM (binding 9, Stage 0b-2b Task 5) is allocated, zeroed and
    /// bound by this function and is NOT read back. That is deliberate: this
    /// probe submits one command buffer per call with a full device idle
    /// wait around it, so the accumulation across bounces it would observe
    /// is ordered by that wait rather than by any barrier, which is the
    /// exact configuration wavefront_loop.hpp's class comment says makes
    /// ordering unobservable. The film check lives on
    /// runWavefrontFusedLoopProbe, where the ordering IS a barrier's job.
    /// The buffer still has to exist here, because a descriptor set must
    /// cover every binding the shader statically uses.
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
    /// With `unoccludedShadowRays` at its default of false, the scene is the
    /// CLOSED box, so every shadow ray this probe's next-event estimator
    /// traces is occluded and every contribution in that record must be
    /// exactly zero -- which is what makes it the check that the shadow ray
    /// is traced at all, as opposed to a visibility term silently stuck at
    /// 1. That does NOT hold when `unoccludedShadowRays` is true (see the
    /// next paragraph): the shadow rays then reach the environment on
    /// purpose, and a nonzero record is the expected, correct result -- the
    /// film check below is exactly the caller that passes true.
    ///
    /// `unoccludedShadowRays` DECOUPLES the acceleration structure bound to
    /// wf_scatter.comp's shadow rays (binding 8) from the one
    /// wf_intersect.comp traces the path rays against (its binding 5). It
    /// defaults to false, which binds ONE TLAS -- the closed box -- to both,
    /// exactly as before; check 28 depends on that and is unchanged by this
    /// parameter existing.
    ///
    /// Passing true substitutes runWavefrontScatterProbe's "no occluders"
    /// rig -- a single triangle a million units out, three orders of
    /// magnitude past the shader's kShadowTMax -- for the SHADOW scene only,
    /// leaving the box in place for the path rays. That is deliberately not
    /// a physical scene: it is a rig. It exists because the film check needs
    /// two things at once that the closed box cannot give together --
    /// (a) every path alive at every bounce, which needs a closed body, and
    /// (b) a NONZERO direct-lighting contribution at every bounce, which
    /// needs shadow rays that reach the environment. Inside a closed box
    /// every contribution is exactly zero (that IS check 28), so a film
    /// check run there would compare 0 against 0 and could not fail. The
    /// path rays and the shadow rays therefore see two scenes here, on
    /// purpose, and nothing about the ESTIMATOR is asserted from this run --
    /// checks 29-31 own that, in a scene where both agree.
    ///
    /// `outNeeSamplesPerRun`, when non-null, receives one NEE record per
    /// RUN rather than only the last: `outNeeSamplesPerRun[b]` is binding 7
    /// after the run of b+1 bounces, i.e. bounce b's values, by the same
    /// argument `outDrawsPerBounce` rests on. Every run starts from zeroed
    /// buffers with the same seed and the RNG is reconstructed from
    /// (pixelIndex, sampleIndex, iterationSeed, bounce) rather than carried,
    /// and every stage indexes path state by path index, so bounce b is
    /// bit-identical between the run of b+1 bounces and the run of any more.
    /// (Queue ORDER varies between runs -- compaction offsets come from an
    /// atomicAdd race -- but nothing here reads a path's values by queue
    /// position.)
    ///
    /// `outFilmPerRun`, when non-null, receives the caller-owned FILM buffer
    /// (binding 9, Stage 0b-2b Task 5) after each run: `width*height*3`
    /// floats, R/G/B per pixel index. The film is zeroed at the top of every
    /// run's command buffer (vkCmdFillBuffer + a TRANSFER_WRITE ->
    /// SHADER_READ|SHADER_WRITE barrier), so `outFilmPerRun[b]` is the total
    /// accumulated over exactly b+1 bounces and nothing earlier. It is
    /// passed to WavefrontLoop::record's `extraBarrierBuffers` alongside the
    /// three probe sinks -- see wavefront_loop.hpp for why that is not
    /// optional -- and named in this function's host-read barrier.
    [[nodiscard]] bool runWavefrontFusedLoopProbe(
        WavefrontBuffers& buffers, uint32_t width, uint32_t height, uint32_t maxBounces,
        float albedo, uint32_t iterationSeed, std::vector<std::vector<float>>& outDrawsPerBounce,
        std::vector<uint32_t>& outLiveCountPerRun, std::vector<uint32_t>& outFinalQueue,
        std::vector<float>* outEnvSamples = nullptr, std::vector<float>* outNeeSamples = nullptr,
        bool unoccludedShadowRays = false,
        std::vector<std::vector<float>>* outNeeSamplesPerRun = nullptr,
        std::vector<std::vector<float>>* outFilmPerRun = nullptr);

    /// Stage 0b-2b Task 6 -- the PARITY probe. Drives the whole wavefront
    /// integrator (generate -> [prepare_indirect, intersect,
    /// prepare_indirect, scatter] x `bounces`) through
    /// ohao::diff::WavefrontLoop over a CALLER-SUPPLIED scene, camera and
    /// material, once per entry in `iterationSeeds`, and hands back the film
    /// each of those runs produced.
    ///
    /// WHY THIS IS NOT runWavefrontFusedLoopProbe WITH MORE PARAMETERS.
    /// That probe's scene, camera and material are FIXED by a build-time
    /// survival derivation (the closed box; see its static_asserts) and its
    /// expected values -- the bit-exact 0.0625 throughput of checks 14/17,
    /// the per-bounce PathRng parity, the live counts -- are calibrated to
    /// exactly that configuration. It also runs the loop once per bounce
    /// COUNT (1, 2, ... B) because its checks need to see each bounce
    /// separately. This probe needs the opposite of all of that: one fixed
    /// bounce count, an arbitrary scene, and many runs at DIFFERENT SEEDS so
    /// the caller can average them. Generalising the other function would
    /// have put every one of those calibrated checks one parameter default
    /// away from silently changing scene.
    ///
    /// ONE GEOMETRY, TWO CONSUMERS. `positions`/`indices` (3 floats per
    /// vertex, 3 uints per triangle -- runWavefrontIntersectOnGeometry's
    /// packing) are bound BOTH as wf_intersect.comp's acceleration structure
    /// plus vertex/index storage buffers AND as wf_scatter.comp's shadow-ray
    /// acceleration structure. There is deliberately no `unoccludedShadowRays`
    /// escape hatch here: a parity check whose shadow rays test different
    /// geometry than its path rays is comparing two different scenes.
    ///
    /// EACH SEED IS ONE INDEPENDENT SAMPLE PER PIXEL. Every run re-zeroes
    /// `buffers` and the film, so `outFilmPerSeed[i]` is the complete
    /// width*height*3 float film of the run at `iterationSeeds[i]` and
    /// nothing earlier. wf_generate.comp writes sampleIndex 0 for every path,
    /// so the seed is the ONLY thing that decorrelates two runs --
    /// wf_scatter.comp rebuilds its RNG from (pixelIndex, sampleIndex,
    /// iterationSeed) each bounce, so distinct seeds give distinct streams
    /// for every path. Passing the same seed twice yields two identical
    /// films, not two samples; that is the caller's invariant to keep.
    ///
    /// Keeping the samples SEPARATE rather than accumulating them into one
    /// film on the GPU is what lets the caller form a per-pixel sample
    /// VARIANCE, which is what a derived Monte Carlo bound needs. It also
    /// keeps every run at exactly one sample per pixel, so the film's
    /// atomicAdd never contends within a dispatch and the run stays
    /// bit-reproducible.
    ///
    /// `outLiveCountPerSeed[i]` is the live-path count after that run's
    /// final bounce. At `bounces == 1` that is exactly the number of primary
    /// rays that HIT something (wf_intersect.comp compacts only survivors and
    /// wf_scatter.comp re-queues everything it is given), which is how a
    /// caller establishes that no primary ray escaped -- the one term the
    /// film does not contain. See diff_gpu_probe.cpp's parity check.
    ///
    /// `width`/`height`/`bounces` carry runWavefrontFusedLoopProbe's dispatch
    /// shape requirements for the same reason: height must equal
    /// wf_generate.comp's local_size_y (8) and width a non-zero multiple of
    /// it, width*height must equal `buffers.layout().capacity()`, and
    /// `bounces` must be non-zero. Unlike that probe, NOTHING here requires
    /// paths to survive: a scene that lets rays escape is the normal case
    /// and simply produces smaller live counts.
    ///
    /// Stage 1 Task 1 -- the REPLAY-EQUIVALENCE probe.
    ///
    /// Runs the SAME closed-box fused loop `runWavefrontFusedLoopProbe` runs,
    /// TWICE per bounce count: once with `shaders/diff/wf_scatter.comp` (the
    /// FORWARD instantiation of the traversal) and once with
    /// `shaders/diff/wf_scatter_replay.comp` (the REPLAY instantiation),
    /// each writing its OWN binding-3 vertex-trace buffer, and hands both
    /// traces back.
    ///
    /// WHAT THE REPLAY RUN IS HANDED. Nothing the forward run produced. It
    /// re-zeroes `buffers`, re-dispatches generate, and re-runs every bounce
    /// from scratch with the SAME `iterationSeed`. It never sees the forward
    /// run's trace, its RNG stream, or its path state -- the forward run's
    /// trace has already been copied out to the host by then, and the device
    /// buffers are separate allocations besides. That is what makes the
    /// comparison a statement about the traversal: the only thing the two
    /// runs share is (pixel, sampleIndex, iterationSeed), which is exactly
    /// the seed invariant (spec section 4.5) path-replay backpropagation
    /// rests on. Handing the replay any recorded value would test nothing.
    ///
    /// WHY BOTH RUNS ARE FULL LOOPS AND NOT A SINGLE RESUMED DISPATCH. Path
    /// state after a forward run of b bounces holds the FINAL vertex, not the
    /// per-bounce ones; a replay that started from it would be replaying one
    /// vertex, not a path. Under the seed invariant a second full run IS the
    /// replay -- that is the whole content of "path replay": the backward
    /// pass re-derives the path rather than storing it.
    ///
    /// `outForwardTracePerBounce[b]` and `outReplayTracePerBounce[b]` each
    /// receive `capacity * kDebugDrawFloats` floats, laid out by TraceSlot,
    /// as written by the run of b+1 bounces -- i.e. BOUNCE b's records. The
    /// sink is indexed by path index, so within one fused run only the last
    /// bounce's write survives; running the loop 1, 2, ... `maxBounces` times
    /// exposes every bounce in turn, exactly as `outDrawsPerBounce` does on
    /// the fused-loop probe and for the same reason.
    ///
    /// Scene, camera, material and dispatch shape are
    /// `runWavefrontFusedLoopProbe`'s: the CLOSED box of half-extent
    /// `kFusedLoopBoxHalfExtent` entered from its centre, whose survival
    /// induction (see that function's scene-constants header) is what makes
    /// "for every path and every bounce" a non-vacuous quantification -- a
    /// scene that killed paths would leave the comparison ranging over
    /// records nothing wrote. `height` must equal 8 and `width` be a non-zero
    /// multiple of 8 for the same 1-D generate dispatch reason.
    ///
    /// `width * height` must equal `buffers.layout().capacity()`, and that
    /// requirement carries a SECOND meaning here beyond dispatch shape: it is
    /// exactly the condition "one path per pixel per dispatch", which is the
    /// film-hazard option this subsystem took (spec section 4.5 offers three;
    /// see the long note on `diffVertexHook` in wf_scatter.comp). This
    /// function refuses to run without it, and the check that consumes its
    /// output histograms `kTraceSlotPixelIndex` to confirm it held in fact
    /// and not merely in argument.
    ///
    /// Returns false on any Vulkan error -- INCLUDING the replay stage's SPV
    /// being absent, which is what this probe reports before that shader
    /// exists.
    [[nodiscard]] bool runWavefrontReplayProbe(
        WavefrontBuffers& buffers, uint32_t width, uint32_t height, uint32_t maxBounces,
        float albedo, uint32_t iterationSeed,
        std::vector<std::vector<float>>& outForwardTracePerBounce,
        std::vector<std::vector<float>>& outReplayTracePerBounce);

    /// Stage 1 Task 2 -- the GRADIENT probe. ONE evaluation point of
    /// (film, dJ/d(albedo)) on a caller-supplied scene.
    ///
    /// Runs the fused wavefront loop TWICE from zeroed buffers with the SAME
    /// `iterationSeed`, over the same scene, camera, material and bounce
    /// count:
    ///
    ///   1. through `shaders/diff/wf_scatter.comp` -- the FORWARD
    ///      instantiation, whose hook accumulates the film -- and copies the
    ///      resulting `width*height*3` floats into `outFilm`;
    ///   2. through `shaders/diff/wf_scatter_replay.comp` -- the REPLAY
    ///      instantiation, whose hook scatters the adjoint -- into `arena`,
    ///      which is zeroed at the top of that run's command buffer.
    ///
    /// The two runs share nothing but the seed, which is exactly the seed
    /// invariant (spec 4.5) the replay rests on and which check 36 measures
    /// directly. Splitting them is what keeps the finite-difference gate's
    /// two sides on separate buffers: the FD side reads a film the forward
    /// hook wrote and never touches the arena; the analytic side reads an
    /// arena the replay hook wrote and never touches the film.
    ///
    /// `gradArenaFloats` and `gradAlbedoOffset` are pushed to the traversal
    /// verbatim (see WavefrontLoop::Config). `arena.buffer()` is bound at
    /// binding 10, passed to `record`'s `extraBarrierBuffers` -- the write is
    /// an atomicAdd, so bounce k's accumulation must be available to bounce
    /// k+1's -- and named in this function's SHADER_WRITE -> HOST_READ
    /// barrier, because the caller reads it back through
    /// `GradientArena::readback`. The caller must NOT zero the arena itself;
    /// this function does, inside the same command buffer as the loop.
    ///
    /// REFUSES to dispatch unless `material.metallic` and
    /// `material.specularWeight` are both exactly 0. That is not a
    /// convenience restriction: `shaders/includes/diff/bsdf_adjoint.glsl` is
    /// exact only for the pure Lambertian configuration (its header names
    /// both failure directions), and at `metallic > 0` the lobe-selection
    /// probability itself depends on the base colour, so a finite-difference
    /// perturbation of the albedo would MOVE THE SAMPLED DIRECTION and common
    /// random numbers would not hold at all -- the comparison would be
    /// between two different paths, and would fail for a reason that has
    /// nothing to do with the derivative.
    ///
    /// Dispatch-shape requirements are runWavefrontParityProbe's, including
    /// `width * height == capacity` (one path per pixel, the film-hazard
    /// resolution). Returns false on any Vulkan error, including the replay
    /// stage's SPV being absent.
    /// The material refusal above is `options.diffParam == 0`'s. For the two
    /// parameters Stage 1 Task 3 adds it is REPLACED, not relaxed, by the
    /// conditions those derivatives need: a specular lobe must EXIST
    /// (`metallic > 0 || specularWeight > 0`, else the lobe probability q is
    /// identically 0, the GGX terms are never evaluated and both gradients are
    /// trivially zero), the roughness must sit strictly above `unpackHitPbr`'s
    /// 0.01 floor, and a metallic run must sit strictly inside the [0,1]
    /// clamp -- at either endpoint the true derivative is one-sided and a
    /// central difference measures half of it.
    [[nodiscard]] bool runWavefrontGradientProbe(
        WavefrontBuffers& buffers, uint32_t width, uint32_t height, uint32_t bounces,
        const WavefrontGenerateCamera& camera, std::span<const float> positions,
        std::span<const uint32_t> indices, float albedo,
        const WavefrontScatterMaterial& material, uint32_t iterationSeed, GradientArena& arena,
        uint32_t gradArenaFloats, uint32_t gradAlbedoOffset, std::vector<float>& outFilm,
        const WavefrontGradientOptions& options = {});

    /// Returns false on any Vulkan error.
    [[nodiscard]] bool runWavefrontParityProbe(WavefrontBuffers& buffers, uint32_t width,
                                               uint32_t height, uint32_t bounces,
                                               const WavefrontGenerateCamera& camera,
                                               std::span<const float> positions,
                                               std::span<const uint32_t> indices, float albedo,
                                               const WavefrontScatterMaterial& material,
                                               std::span<const uint32_t> iterationSeeds,
                                               std::vector<std::vector<float>>& outFilmPerSeed,
                                               std::vector<uint32_t>& outLiveCountPerSeed);

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
