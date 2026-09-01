// Standalone GPU probe for the differentiable renderer scaffolding.
// Requires a working Vulkan device. Returns 0 on success.
//
// Checks:
//   1. GradientArena allocates, zeroes, and reads back.
//   2. atomicAdd on a float SSBO accumulates correctly under contention;
//      also exercises ohao::diff::ComputePipeline's build/bind/destroy
//      lifecycle directly (Stage 0b-2a Task 1), then replays the same
//      dispatch through ohao::diff::WavefrontStage::record() (Stage 0b-2a
//      Task 2) into an independent arena block, proving record() actually
//      drives bind/push/dispatch rather than silently no-op'ing.
//   3. rayQueryEXT visibility matches a closed-form plane intersection.
//   4. A half-quad's hit/miss pattern pins the camera's Y orientation --
//      check 3's distance formula is even in dy, so it can't catch a flipped
//      up-vector or NDC-Y sign on its own.
//   5. A block index handed out by ohao::diff::ParamRegistry resolves
//      correctly against a GradientArena built from that registry's layout --
//      both hold ArenaLayout by value, so this is what proves the positional
//      indices survive the copy.
//   6. ohao::diff::PathRng and shaders/includes/diff/rng.glsl agree
//      bit-exactly over a whole stream, values AND draw count -- the
//      invariant path replay in Stage 1 rests on.
//   7. WavefrontBuffers builds, and every PathStateField of every path index
//      reads back zero afterwards.
//   8. wf_generate.comp's generated rays reproduce check 3's closed form.
//   9. Every PathStateField wf_generate.comp writes round-trips exactly.
//   10. wf_generate.comp's queue/counter population has no atomicAdd races.
//   11. path_state_layout.hpp and path_state.glsl agree field-by-field.
//   12. wf_intersect.comp compacts survivors of a known-fraction scene into
//       queue ring 1 exactly (no duplicates, no dead paths), across an
//       indirectly-sized dispatch prepared by wf_prepare_indirect.comp.
//   13. An indirect dispatch sized from a live-count of 0 launches zero
//       invocations -- dead paths are genuinely free.
//   14. wf_scatter.comp's constant-albedo throughput decay is exact after 4
//       bounces (p=0.5 -> 0.0625, compared bit-exact, no tolerance) --
//       proves the Throughput and Bounce fields survive 4 separate dispatch
//       boundaries intact. (Origin/Dir are written every bounce but not
//       exercised by any assertion here -- intersect runs once, not per
//       bounce, so positions after bounce 0 are geometrically meaningless
//       scaffolding; Stage 0b-2 running intersect per bounce is what would
//       make them worth checking.)
//   15. wf_scatter.comp's per-bounce RNG draws (values AND drawCount) match
//       ohao::diff::PathRng's replay exactly, for every one of those 4
//       dispatches -- extends check 6's single-stream parity guarantee
//       across dispatch boundaries, which is what path replay in Stage 1
//       depends on.
//   16-18. The SAME two analytic properties as checks 14-15, re-proved with
//       the whole bounce loop fused into ONE command buffer through
//       ohao::diff::WavefrontLoop -- no vkQueueWaitIdle between stages, so
//       ordering is the barriers' job rather than a full-device idle wait's.
//       16 also asserts every path survives every bounce and the live ring
//       holds each path index exactly once, which is what keeps 17 from
//       passing vacuously over an empty survivor set.
//   19. wf_intersect.comp's stored geometric normal equals the analytic
//       surface normal of the face it hit, for every path, against a closed
//       axis-aligned box whose face planes and outward winding are known
//       host-side. The oracle is that geometry plus the closed-form camera
//       ray -- nothing the shader computed.
//   20. The BSDF itself (shaders/includes/diff/bsdf.glsl, as called by
//       wf_scatter.comp and observed through bsdf_probe.comp): f, the
//       sampling pdf, and the sampler's f*cos/pdf weight all match a CPU
//       oracle written from Walter et al. 2007, Heitz 2014, Heitz 2018,
//       Schlick 1994 and PBRT -- not from the GLSL under test.
//   21. White furnace: albedo 1, no absorption, constant environment. The
//       cosine-sampled Lambert estimator is a constant 1 with ZERO variance,
//       so this is asserted to 4 ulp rather than to a statistical tolerance.
//       Catches energy-loss bugs in the whole sample-evaluate-weight loop
//       that a per-term comparison structurally cannot. Runs at q = 0
//       EXACTLY, which is the pure-Lambert early-return branch.
//   22. The same furnace with a white rough conductor: every path's weight
//       lies in [0,1] (provable pointwise -- it is G2/G1) and the mean is
//       strictly below 1, which is the known single-scattering GGX deficit
//       and NOT something that should be asserted to equal 1. Runs at
//       q = 1 EXACTLY.
//   23. The same furnace at an INTERMEDIATE lobe probability (q ~ 0.69), so
//       the mixture density and the f*cos/pdf division are executed at all
//       -- 21 and 22 sit at the two values of q that cannot bias anything,
//       and 21 does not even reach the division. Asserts a derived pointwise
//       energy-creation bound and a derived interval for the mean, the
//       latter anchored to 22's own measurement of the single-scattering
//       GGX albedo.
//   24. Environment importance sampling (shaders/includes/rt/env_sampling.glsl
//       as called by wf_scatter.comp): a Pearson chi-squared goodness-of-fit
//       test over 24576 directions a real GPU dispatch drew, binned into the
//       map's 128 texels, against an oracle computed here from the luminance
//       image and the analytic sin(theta) solid-angle weight -- NOT from the
//       CDF that was uploaded, so the host-side CDF builder is under test
//       too. The rejection threshold is derived (Wilson-Hilferty at
//       alpha = 1e-6), never tuned.
//   25. Every returned pdf is finite and strictly positive over a strictly
//       positive environment; the pdfs' range equals the map's own luminance
//       range, which is the sin(theta) Jacobian asserted as an equality
//       rather than as an integral; and every sampled direction inverts to a
//       texel CENTRE -- which is what makes 24's binning meaningful at all.
//   26. Those pdfs integrate to 1 over the sphere: pdf * dOmega is
//       condDiff * margDiff with the sine cancelling, so the sum is an
//       identity rather than a quadrature, and is asserted to a derived
//       float32 error budget.
//   27. ohao::diff::WavefrontLoop::record's OWN fill of ScatterPush's
//       envWidth/envHeight (production path -- checks 24-26 exercise
//       runWavefrontScatterProbe's hand-filled push constants, a different
//       call site) against a genuinely NON-SQUARE environment, read back
//       through the fused loop's binding-6 sink. The oracle is closed-form:
//       wf.build()'s default UV-uniform CDF makes pdf = 1/(2 pi^2 sin(theta))
//       exactly, so no CDF builder is needed to know what record() should
//       have produced.
//   28. THE SHADOW RAY EXISTS (Stage 0b-2b Task 4). Check 27's run is a
//       CLOSED box entered from its centre, so every next-event shadow ray
//       wf_scatter.comp traces is occluded and every direct-lighting
//       contribution must be EXACTLY zero -- bit for bit, because the
//       estimator multiplies by the visibility term rather than attenuating
//       by it. A visibility term stuck at 1 is invisible to checks 29-31,
//       which run unoccluded. Non-vacuity: the recovered environment
//       radiance is asserted strictly positive on the same samples, so the
//       zeros are the shadow ray's doing and not an absence of light.
//   29. Next-event-only, BSDF-only and their MIS combination estimate ONE
//       direct-lighting integral and agree, over 49152 i.i.d. paired
//       samples, to a bound derived from the run's own standard errors at a
//       fixed z -- with the one systematic term (the sampler's midpoint
//       quadrature, kappa = sinc(pi/envH)) derived in closed form rather
//       than fitted.
//   30. The MIS partition, PER SAMPLE and exactly: at the light sample's
//       direction and again at the BSDF sample's, the two balance-heuristic
//       weights sum to 1 to a couple of ulp. Unbiasedness needs that
//       pointwise, not on average.
//   31. The three things nothing tested before Task 4: ScatterPush::
//       envIntegral reaching the GPU intact (an absolute comparison against
//       the environment image -- check 29 is blind to it, since a wrong
//       integral rescales all three estimators together); env_sampling.glsl's
//       pdfEnvMap, which had no caller under test anywhere in this
//       repository; and the ROUTING claim, that next-event estimation
//       consumes the very sample check 24 bins rather than drawing a second
//       one.
//   32. RADIANCE ACCUMULATION INTO THE FILM (Stage 0b-2b Task 5). After a
//       FUSED multi-bounce run through WavefrontLoop::record, the
//       caller-owned film buffer equals the sum of the per-bounce
//       contributions, reconstructed on the host from primitives the
//       binding-7 sink records separately (arrival throughput, both
//       single-strategy estimators, both MIS weights, the pixel index) --
//       so the accumulator is never compared against a copy of itself. The
//       per-bounce values come from the probe's existing one-run-per-bounce
//       structure. Asserted to a derived gamma_{k+4} float32 bound, with
//       explicit non-vacuity gates on how far a dropped bounce would move
//       the film relative to that bound.
//   33-34. THE STAGE GATE (Stage 0b-2b Task 6): the whole wavefront
//       integrator -- fused loop, MIS direct lighting, throughput recursion,
//       film -- against an INDEPENDENT CPU reference path tracer on a
//       probe-owned scene. The reference shares no intersector, no basis, no
//       RNG and no importance sampling with the GPU (it is
//       cosine-hemisphere, no env sampling, no MIS); two different unbiased
//       estimators of one integral agree only if both are unbiased. 33 is
//       PER PIXEL at a family-wise-corrected z; 34 is POOLED on the image
//       total, whose variance is taken ACROSS RUNS so it assumes nothing
//       about inter-pixel independence, plus a CONVERGENCE assertion that
//       rms(D) shrinks by the predicted factor of sqrt(1/4) -- which a fixed
//       bias cannot satisfy. The pooled test also refuses to return a
//       verdict at all if its own resolution is too coarse to have detected
//       anything. Why the reference is not ohao::PathTracer, and exactly
//       what the two sides share, is derived at the head of this file's
//       anonymous namespace ("INDEPENDENT CPU REFERENCE INTEGRATOR").
//   35-36. REPLAY EQUIVALENCE (Stage 1 Task 1): 35 establishes that the
//       FORWARD traversal's binding-3 vertex trace is real and independently
//       correct (every draw against a CPU PathRng the GPU never sees, every
//       arrival throughput exactly albedo^bounce, every pixel covered once);
//       36 then compares that trace bit-for-bit against the REPLAY
//       instantiation's, from two independent runs of the loop at one seed.
//   37-38. THE FIRST GRADIENT (Stage 1 Task 2). 37 is a COMMON-RANDOM-NUMBER
//       finite difference: the film is rendered at albedo +/- h through the
//       forward instantiation and differenced, and compared against what the
//       replay instantiation's hook scattered into the gradient arena on a
//       separate run at the same seed. Because both sides describe ONE
//       realisation of the estimator there is no sampling error in the
//       budget at all -- the tolerance is a derived arithmetic bound
//       (cancellation + truncation, computed from the run's own J and h) and
//       the step size is derived, not tuned. It runs at 1, 2 and 3 bounces,
//       which is what separates "the direct scatter line is right" from "the
//       whole derivative is right". 38 is the NULL TEST: every arena block
//       the scatter was not told to write comes back EXACTLY zero.
//
// Checks 44-45 are Stage 1 Task 5's, the first parameter that is not a
// scalar: 44 asserts that a bilinear scatter's four weights sum to the
// incoming adjoint (an identity independent of the weights' individual
// values, measured against a separate DIFF_PARAM_EMISSION run) and that every
// arena float outside the host-predicted footprint is EXACTLY zero; 45 is the
// per-element magnitude gate, perturbing ONE primal texel on the host.
//
// SIX GLSL/C++ ties run BEFORE any Vulkan object exists, and refuse to run
// the probe at all if they do not hold -- see checkNeeStrideTie,
// checkWfScatterSinkLayoutTie, checkScatterPushSizeTie,
// checkBsdfShaderConstantTies, checkParityRefConstantsTie and
// checkTexelOrderingTie, which now live in probe/ties.{hpp,cpp} rather than
// this file's anonymous namespace. They print NOTE lines rather than OK
// lines: they are preconditions of the checks above meaning anything, not
// checks in their own right.

#include "gpu_probe_context.hpp"

// The helper sections this file used to carry inline. Moved out verbatim;
// nothing about what any check compares changed.
#include "probe/fd_harness.hpp"
#include "probe/oracle_bsdf.hpp"
#include "probe/oracle_integrator.hpp"
#include "probe/scene.hpp"
#include "probe/ties.hpp"

#include "diff/device_caps.hpp"
#include "diff/grad/arena_layout.hpp"
#include "diff/grad/gradient_arena.hpp"
#include "diff/rng/diff_rng.hpp"
#include "diff/param/param_registry.hpp"
#include "diff/wavefront/compute_pipeline.hpp"
#include "diff/wavefront/wavefront_buffers.hpp"
#include "diff/wavefront/wavefront_loop.hpp"
#include "diff/wavefront/wavefront_stage.hpp"

// ohao::EnvCDF is the RT pipeline's own environment CDF builder. Check 24
// uploads what IT produces rather than re-deriving the CDF here, so the two
// pipelines cannot drift apart on the convention
// shaders/includes/rt/env_sampling.glsl assumes.
#include "render/rt/env_cdf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <cstdint>
#include <random>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// The independent CPU BSDF oracle (Stage 0b-2b Task 2, check 20) lives in
// probe/oracle_bsdf.{hpp,cpp}. Moved there verbatim, provenance comments and
// all; nothing about what check 20 compares changed.
using ohao::diff::probe::kBsdfProbeFloatsPerCase;
using ohao::diff::probe::kOraclePi;
using ohao::diff::probe::kShaderGrazingCos;
using ohao::diff::probe::OracleEnvTexel;
using ohao::diff::probe::OracleMaterial;
using ohao::diff::probe::OracleVec3;
using ohao::diff::probe::oracleAdd;
using ohao::diff::probe::oracleBsdfEval;
using ohao::diff::probe::oracleCosineHemisphere;
using ohao::diff::probe::oracleCross;
using ohao::diff::probe::oracleDirFromAngles;
using ohao::diff::probe::oracleDirectionalAlbedo;
using ohao::diff::probe::oracleDistance;
using ohao::diff::probe::oracleDot;
using ohao::diff::probe::oracleEnvTexelOf;
using ohao::diff::probe::oracleF0;
using ohao::diff::probe::oracleFrame;
using ohao::diff::probe::oracleGgxD;
using ohao::diff::probe::oracleNormalize;
using ohao::diff::probe::oracleRelDiff;
using ohao::diff::probe::oracleScale;
using ohao::diff::probe::oracleSchlick;
using ohao::diff::probe::oracleSmithG1;
using ohao::diff::probe::oracleSmithG2;
using ohao::diff::probe::oracleSmithLambda;
using ohao::diff::probe::oracleSpecProb;
using ohao::diff::probe::oracleSpecScale;

// The independent CPU reference integrator (Stage 0b-2b Task 6, checks 33-34)
// lives in probe/oracle_integrator.{hpp,cpp}, moved there verbatim.
using ohao::diff::probe::ParityRefScene;
using ohao::diff::probe::ParityTriangle;
using ohao::diff::probe::kParityRayTMax;
using ohao::diff::probe::kParitySurfaceOffset;
using ohao::diff::probe::parityAddQuad;
using ohao::diff::probe::parityBasis;
using ohao::diff::probe::parityCameraRay;
using ohao::diff::probe::parityCosineSample;
using ohao::diff::probe::parityEnvRadiance;
using ohao::diff::probe::parityMoments;
using ohao::diff::probe::parityMomentsFromSums;
using ohao::diff::probe::parityNextU;
using ohao::diff::probe::parityOccluded;
using ohao::diff::probe::parityRayTriangle;
using ohao::diff::probe::parityReferenceSample;
using ohao::diff::probe::parityTraceNearest;
using ohao::diff::probe::parityTrianglesFromSoup;

// The eight startup source-parsing ties live in probe/ties.{hpp,cpp}, moved
// there verbatim. main() still calls them in the same order, before any
// Vulkan object exists.
using ohao::diff::probe::checkBsdfShaderConstantTies;
using ohao::diff::probe::checkDrawsPerBounceTie;
using ohao::diff::probe::checkNeeStrideTie;
using ohao::diff::probe::checkParityRefConstantsTie;
using ohao::diff::probe::checkScatterPushSizeTie;
using ohao::diff::probe::checkTexelOrderingTie;
using ohao::diff::probe::checkTraverseInstantiationTie;
using ohao::diff::probe::checkWfScatterSinkLayoutTie;

// The shared scene -- one geometry, one environment, one camera -- lives in
// probe/scene.{hpp,cpp}, moved there verbatim.
using ohao::diff::probe::buildParityEnvironment;
using ohao::diff::probe::buildParityScene;
using ohao::diff::probe::parityCamera;

// The finite-difference harnesses (Stage 1 Tasks 2-5) live in
// probe/fd_harness.{hpp,cpp}, moved there verbatim.
using ohao::diff::probe::CrnFdMeasurement;
using ohao::diff::probe::GgxFdMeasurement;
using ohao::diff::probe::HostBilinearFootprint;
using ohao::diff::probe::emissionTextureOptions;
using ohao::diff::probe::filmTotal;
using ohao::diff::probe::hostBilinearFootprint;
using ohao::diff::probe::measureCrnAlbedoGradient;
using ohao::diff::probe::measureCrnEmissionGradient;
using ohao::diff::probe::measureCrnEmissionTexelGradient;
using ohao::diff::probe::measureDetachedGgxGradient;
using ohao::diff::probe::traceGeometryMismatches;

}  // namespace

int main() {
    // The GLSL/C++ record-stride tie, before any Vulkan object exists: if it
    // does not hold, nothing measured below means anything.
    if (!checkNeeStrideTie()) return 1;
    // The GLSL/C++ ScatterPush byte-size tie (review Finding 6, Task 5): same
    // reasoning, same failure mode (a silent wrong-field push), checked here
    // for the same "before anything downstream trusts it" reason.
    if (!checkScatterPushSizeTie()) return 1;
    // The per-SLOT layout of wf_scatter.comp's three probe sinks (whole-branch
    // review finding): the stride tie above cannot see a transposition of two
    // same-arity slots, and the 24 offsets are what checks 28-32 index by.
    if (!checkWfScatterSinkLayoutTie()) return 1;
    // The per-bounce RNG draw count, which positions every CPU-side PathRng
    // oracle below relative to the shader's stream.
    if (!checkDrawsPerBounceTie()) return 1;
    // The one-source/two-instantiations property (spec 6.2), before any GPU
    // object exists: if the forward and replay kernels can diverge, the
    // replay-equivalence check below is comparing two different traversals and
    // its agreement means nothing.
    if (!checkTraverseInstantiationTie()) return 1;
    // bsdf_probe.comp's output stride and bsdf.glsl's DIFF_BSDF_MIN_COS -- the
    // latter is what check 20 EXCUSES a grazing rejection with, so drift there
    // widens an excuse silently.
    if (!checkBsdfShaderConstantTies()) return 1;
    // checks 33-34's CPU reference constants tied to wf_intersect.comp's
    // kTraceTMax and wf_scatter.comp's own kShadowTMax/kSurfaceOffset (review
    // findings, Task 6 fix): same comment-stripping, checked here for the same
    // reason.
    if (!checkParityRefConstantsTie()) return 1;
    if (!checkTexelOrderingTie()) return 1;

    ohao::diff::GpuProbeContext ctx;
    if (!ctx.init()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: could not initialise Vulkan\n");
        return 1;
    }

    const ohao::diff::DeviceCaps caps = ohao::diff::queryDeviceCaps(ctx.physicalDevice());
    if (!caps.sufficient()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: device lacks ray query or float atomics\n");
        return 1;
    }

    ohao::diff::ArenaLayout layout;
    const std::size_t blockA = layout.add(16);
    const std::size_t blockB = layout.add(4);
    // Reserved for the WavefrontStage check below (Stage 0b-2a Task 2) so it
    // has its own independent target index rather than reusing blockA's,
    // which check 2 has already mutated by the time this runs -- reusing it
    // would make this check's result depend on execution order.
    const std::size_t blockC = layout.add(16);

    ohao::diff::GradientArena arena;
    if (!arena.build(ctx.allocator(), layout)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: arena build\n");
        return 1;
    }

    // 1. zero + readback
    //
    // The guard below asserts the EXPECTED element count, not merely that
    // something came back. `values.empty()` would let a readback that
    // returned ONE zeroed float pass a loop that then verifies one element
    // and calls the block zeroed -- the same weakness check 7 documents and
    // closes in its own case (review finding). The expected counts are the
    // sizes handed to ArenaLayout::add above, paired with their indices here
    // so a future block cannot be added to one list and not the other.
    ctx.runImmediate([&](VkCommandBuffer cmd) { arena.zero(cmd); });
    const std::pair<std::size_t, std::size_t> kZeroChecked[] = {{blockA, 16}, {blockB, 4}};
    for (const auto& [b, expectedCount] : kZeroChecked) {
        const std::vector<float> values = arena.readback(ctx.allocator(), b);
        if (values.size() != expectedCount) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: block %zu readback returned %zu floats, expected "
                         "%zu (the size it was added to the layout with). A short readback would "
                         "otherwise let this check pass having verified only the elements that "
                         "came back\n",
                         b, values.size(), expectedCount);
            return 1;
        }
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (values[i] != 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: block %zu element %zu = %f, expected 0\n",
                             b, i, values[i]);
                return 1;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: arena zero + readback\n");

    // 2. float atomics under contention. Also exercises
    // ohao::diff::ComputePipeline (Task 1, Stage 0b-2a) directly: build()
    // for diff_atomic_probe.comp.spv, confirm pipeline()/layout()/
    // descriptorSet() are all non-null, bind the arena buffer, then call
    // destroy() twice to confirm the second call is a no-op and that
    // destroy() nulls all three handles.
    //
    // This has its own OK line. It used to have none, on the stated grounds
    // that folding it into the atomics check below "keeps this section at
    // one printed OK line, matching the pre-refactor count" -- which is
    // exactly backwards: two of the assertions here (a second destroy() is
    // a no-op; destroy() nulls pipeline()/layout()/descriptorSet()) are
    // covered by nothing else, so deleting them would leave this probe's
    // stdout byte-identical. A check whose removal is invisible is not a
    // check. Printed OK-line counts are an output of what is verified,
    // never a target to hold constant.
    {
        ohao::diff::ComputePipeline pipelineSanity;
        const VkDescriptorType bindingTypes[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        if (!pipelineSanity.build(ctx.device(), "diff_atomic_probe.comp.spv", bindingTypes,
                                  /*pushConstantSize=*/8)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: ComputePipeline::build\n");
            return 1;
        }
        if (pipelineSanity.pipeline() == VK_NULL_HANDLE ||
            pipelineSanity.layout() == VK_NULL_HANDLE ||
            pipelineSanity.descriptorSet() == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: ComputePipeline built a null handle\n");
            return 1;
        }
        const VkBuffer buffersToBind[] = {arena.buffer()};
        if (!pipelineSanity.bindBuffers(ctx.device(), buffersToBind)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: ComputePipeline::bindBuffers\n");
            return 1;
        }
        pipelineSanity.destroy(ctx.device());
        pipelineSanity.destroy(ctx.device());  // must be a no-op, not a double-free
        if (pipelineSanity.pipeline() != VK_NULL_HANDLE ||
            pipelineSanity.layout() != VK_NULL_HANDLE ||
            pipelineSanity.descriptorSet() != VK_NULL_HANDLE) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: ComputePipeline::destroy left a handle live\n");
            return 1;
        }
    }
    std::printf("[diff_gpu_probe] OK: ComputePipeline build + bind + double destroy (second "
                "destroy is a no-op, all handles nulled)\n");

    constexpr uint32_t kInvocations = 4096;
    if (!ctx.runAtomicProbe(arena, /*targetIndex=*/0, kInvocations)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: atomic probe dispatch\n");
        return 1;
    }
    const std::vector<float> after = arena.readback(ctx.allocator(), blockA);
    if (after.size() != 16) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: block %zu readback returned %zu floats, expected 16 "
                     "(the size it was added to the layout with). A short readback would "
                     "otherwise leave the out-of-target-index scan below covering fewer elements "
                     "than the block has\n",
                     blockA, after.size());
        return 1;
    }
    if (after[0] != static_cast<float>(kInvocations)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: atomicAdd gave %f, expected %u "
                     "(lost updates = non-atomic accumulation)\n",
                     after[0], kInvocations);
        return 1;
    }
    // Every element of the block, not just after[1] -- the message says
    // "wrote outside target index" and now asserts it (review finding),
    // matching what check 2b's twin loop already did over its own block.
    for (std::size_t i = 1; i < after.size(); ++i) {
        if (after[i] != 0.0f) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: atomicAdd wrote outside target index (block %zu "
                         "element %zu = %f, expected 0)\n",
                         blockA, i, after[i]);
            return 1;
        }
    }
    std::printf("[diff_gpu_probe] OK: atomicAdd accumulated %u contended adds exactly\n",
                kInvocations);

    // 2b. ohao::diff::WavefrontStage (Stage 0b-2a Task 2): drives the same
    // atomic-probe canary as check 2, but through record() -- bind
    // pipeline, bind descriptor set, push constants, vkCmdDispatch -- rather
    // than a hand-rolled sequence. This is a real differential, not just a
    // handle-non-null sanity check: if record() forgot to bind the
    // descriptor set, pushed the wrong constants, or dispatched a Fixed
    // group count of 0, the canary would land on something other than
    // exactly kStageInvocations (most likely 0, since block C was zeroed
    // alongside every other block in check 1 and nothing else writes to
    // it), not silently pass.
    constexpr uint32_t kStageInvocations = 2048;
    {
        ohao::diff::WavefrontStage stage;
        const VkDescriptorType bindingTypes[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        struct PushConstants {
            uint32_t targetIndex;
            uint32_t invocationCount;
        };
        if (!stage.build(ctx.device(), "diff_atomic_probe.comp.spv", bindingTypes,
                         sizeof(PushConstants))) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: WavefrontStage::build\n");
            return 1;
        }
        const VkBuffer buffersToBind[] = {arena.buffer()};
        if (!stage.bindBuffers(ctx.device(), buffersToBind)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: WavefrontStage::bindBuffers\n");
            return 1;
        }

        // Absolute float index of block C within the arena's single flat
        // `data[]` array -- computed from the layout rather than hardcoded,
        // since blockC's byte offset depends on blockA/blockB's sizes.
        const ohao::diff::ArenaBlock blockCInfo = layout.block(blockC);
        const uint32_t targetIndex = static_cast<uint32_t>(blockCInfo.offsetBytes / sizeof(float));

        const PushConstants push{targetIndex, kStageInvocations};
        stage.setPushConstants(&push, sizeof(push));
        stage.setGroupCount(
            ohao::diff::WavefrontStage::Fixed{(kStageInvocations + 63u) / 64u});

        ctx.runImmediate([&](VkCommandBuffer cmd) {
            stage.record(cmd);

            // Same host-read barrier dispatchStorageBufferCompute records
            // after its own dispatch -- that function, in
            // gpu_probe_context.cpp, IS the reference sequence for this
            // pattern (compute_pipeline.cpp records no barriers at all; it
            // contains no vkCmdPipelineBarrier call) --
            // WavefrontStage::record() deliberately does not
            // add this itself (see its class comment), so the check adds it
            // directly, exactly as WavefrontLoop will for each stage it
            // sequences in Task 3.
            VkBufferMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = arena.buffer();
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &barrier, 0,
                                 nullptr);
        });
        stage.destroy(ctx.device());

        const std::vector<float> stageResult = arena.readback(ctx.allocator(), blockC);
        if (stageResult.empty()) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: block %zu readback returned no data\n", blockC);
            return 1;
        }
        if (stageResult[0] != static_cast<float>(kStageInvocations)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: WavefrontStage::record produced %f, expected "
                         "%u contended adds (stage did not run, or ran the wrong invocation "
                         "count)\n",
                         stageResult[0], kStageInvocations);
            return 1;
        }
        for (std::size_t i = 1; i < stageResult.size(); ++i) {
            if (stageResult[i] != 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: WavefrontStage::record wrote outside its "
                             "target index (block %zu element %zu = %f)\n",
                             blockC, i, stageResult[i]);
                return 1;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: WavefrontStage::record replays the atomic-probe canary -- "
                "%u contended adds exactly through a Fixed dispatch\n", kStageInvocations);

    // 3. Ray-query visibility against a plane whose intersections are analytic.
    //
    // kW != kH deliberately: a square resolution makes the closed form
    // (even in both dx and dy, see check 4's comment) blind to a transposed
    // pixel index, and leaves the ndcX*aspect term in the shader untested
    // since aspect == 1 would hide a missing/backwards aspect multiply.
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 48;
    constexpr float kPlaneDistance = 2.0f;
    constexpr float kTanHalfFov = 0.2f;  // narrow, so every ray hits the quad
    constexpr float kAspect = static_cast<float>(kW) / static_cast<float>(kH);
    // Widened horizontal extent still lands inside the +-1 quad:
    // kTanHalfFov * kAspect ~= 0.267, so max |x| at z=-2 is ~0.53.

    std::vector<float> hitsT;
    if (!ctx.runVisibilityProbe(kPlaneDistance, kW, kH, kTanHalfFov, hitsT)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: visibility probe dispatch\n");
        return 1;
    }
    if (hitsT.size() != static_cast<std::size_t>(kW) * kH) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: hit buffer size %zu, expected %u\n",
                     hitsT.size(), kW * kH);
        return 1;
    }

    // The centre pixel looks straight down -Z, so t is exactly the plane distance.
    const std::size_t centre = static_cast<std::size_t>(kH / 2) * kW + (kW / 2);
    if (std::fabs(hitsT[centre] - kPlaneDistance) > 1e-4f) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: centre ray t = %f, expected %f\n",
                     hitsT[centre], kPlaneDistance);
        return 1;
    }

    // Off-axis rays are longer by exactly 1/cos(theta), and every ray must hit.
    // Tolerance is 1e-4: float32 at t~=2 supports ~1e-5, so this still leaves
    // ~10x headroom over float noise while catching e.g. a dropped +0.5
    // pixel-centre offset (worst-case error ~1.19e-3 at this FOV, comfortably
    // above 1e-4). Do not widen this to make a failure pass -- report the
    // actual max error and stop.
    float maxAbsError = 0.0f;
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * kW + x;
            if (hitsT[i] < 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: pixel (%u,%u) missed a quad that "
                             "covers the whole frustum\n", x, y);
                return 1;
            }
            const float ndcX = 2.0f * (static_cast<float>(x) + 0.5f) / kW - 1.0f;
            const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / kH;
            const float dx = ndcX * kAspect * kTanHalfFov;
            const float dy = ndcY * kTanHalfFov;
            const float expected = kPlaneDistance * std::sqrt(1.0f + dx * dx + dy * dy);
            const float err = std::fabs(hitsT[i] - expected);
            maxAbsError = std::max(maxAbsError, err);
            if (err > 1e-4f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: pixel (%u,%u) t = %f, closed form %f "
                             "(|err| = %g)\n",
                             x, y, hitsT[i], expected, err);
                return 1;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: ray query matches closed-form plane intersection "
                "over %u pixels (max |err| = %g)\n", kW * kH, maxAbsError);

    // 4. A half-quad (y in [0,1] only) makes the hit/miss pattern asymmetric
    // in Y. The closed form sqrt(1+dx^2+dy^2) above is even in dy, so a
    // flipped up-vector or a flipped NDC-Y sign is invisible to it -- every
    // ray still lands at the "right" distance from *some* consistent-looking
    // convention, even a backwards one. This check pins the actual convention
    // Stage 0b's integrator inherits: ndcY > 0 for y < kH/2, i.e. the top
    // half of the image must hit and the bottom half must miss.
    std::vector<float> halfHits;
    if (!ctx.runVisibilityProbe(kPlaneDistance, kW, kH, kTanHalfFov, halfHits,
                                /*quadMinY=*/0.0f)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: half-quad visibility probe dispatch\n");
        return 1;
    }
    if (halfHits.size() != static_cast<std::size_t>(kW) * kH) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: half-quad hit buffer size %zu, expected %u\n",
                     halfHits.size(), kW * kH);
        return 1;
    }
    for (uint32_t y = 0; y < kH; ++y) {
        const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(kH);
        const bool expectHit = (ndcY > 0.0f);
        for (uint32_t x = 0; x < kW; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * kW + x;
            const bool didHit = (halfHits[i] >= 0.0f);
            if (didHit != expectHit) {
                std::fprintf(stderr,
                    "[diff_gpu_probe] FAIL: half-quad orientation at (%u,%u): expected %s, got %s "
                    "(camera up-vector or NDC-Y sign is inverted)\n",
                    x, y, expectHit ? "hit" : "miss", didHit ? "hit" : "miss");
                return 1;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: half-quad pins camera Y orientation over %u pixels\n",
                kW * kH);

    // 5. The seam Stage 1 depends on most: a block index handed out by the registry
    //    must resolve correctly against an arena built from that registry's layout.
    //    Both hold ArenaLayout by value, so this proves the positional indices survive
    //    the copy -- previously true only by inspection.
    {
        ohao::diff::ParamRegistry reg;
        const auto reg1 = reg.registerTexture("albedo", {8, 8, 3}, VK_FORMAT_R32G32B32A32_SFLOAT);
        const auto reg2 = reg.registerScalarBlock("ssao_params", 4);
        if (!reg1.ok || !reg2.ok) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: registry setup: %s %s\n",
                         reg1.error.c_str(), reg2.error.c_str());
            return 1;
        }

        ohao::diff::GradientArena regArena;
        if (!regArena.build(ctx.allocator(), reg.layout())) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: arena build from registry layout\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { regArena.zero(cmd); });

        const ohao::diff::DiffParam* albedo = reg.find("albedo");
        const ohao::diff::DiffParam* ssao = reg.find("ssao_params");
        if (albedo == nullptr || ssao == nullptr) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: registered params not found\n");
            regArena.destroy(ctx.allocator());
            return 1;
        }

        struct Expect { const char* name; std::size_t block; std::size_t floats; };
        const Expect expects[] = {
            {"albedo.grad",  albedo->gradBlock,  albedo->floatCount},
            {"albedo.state", albedo->stateBlock, albedo->floatCount * 2u},
            {"ssao.grad",    ssao->gradBlock,    ssao->floatCount},
            {"ssao.state",   ssao->stateBlock,   ssao->floatCount * 2u},
        };
        for (const Expect& e : expects) {
            const std::vector<float> block = regArena.readback(ctx.allocator(), e.block);
            if (block.size() != e.floats) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: %s resolved to %zu floats, registry says %zu "
                             "(registry/arena block indices disagree)\n",
                             e.name, block.size(), e.floats);
                regArena.destroy(ctx.allocator());
                return 1;
            }
            for (float v : block) {
                if (v != 0.0f) {
                    std::fprintf(stderr, "[diff_gpu_probe] FAIL: %s not zeroed\n", e.name);
                    regArena.destroy(ctx.allocator());
                    return 1;
                }
            }
        }
        regArena.destroy(ctx.allocator());
        std::printf("[diff_gpu_probe] OK: registry block indices resolve against arena "
                    "(4 blocks, sizes match)\n");
    }


    // 6. CPU/GPU RNG parity -- the invariant path replay backpropagation rests on.
    //    The backward pass stores no tape; it replays each light path from its seed.
    //    If shaders/includes/diff/rng.glsl and ohao/diff/rng/diff_rng.cpp disagree by
    //    a single bit, the replayed path is a DIFFERENT path and every gradient is
    //    silently wrong -- no crash, no NaN. Until now the two were verified
    //    identical only by reading them side by side.
    {
        constexpr uint32_t kDraws = 64;
        constexpr uint32_t kPixel = 4096;
        constexpr uint32_t kSample = 3;
        constexpr uint32_t kSeed = 12345;

        ohao::diff::ArenaLayout rngLayout;
        const std::size_t drawBlock = rngLayout.add(kDraws);
        ohao::diff::GradientArena rngArena;
        if (!rngArena.build(ctx.allocator(), rngLayout)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: rng arena build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { rngArena.zero(cmd); });

        std::vector<float> gpuDraws;
        if (!ctx.runRngParityProbe(kPixel, kSample, kSeed, kDraws, rngArena, drawBlock, gpuDraws)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: rng parity dispatch\n");
            rngArena.destroy(ctx.allocator());
            return 1;
        }

        ohao::diff::PathRng cpuRng = ohao::diff::PathRng::forPath(kPixel, kSample, kSeed);
        for (uint32_t i = 0; i < kDraws; ++i) {
            const float cpu = cpuRng.next1D();
            const float gpu = gpuDraws[i];
            // Bit-exact on purpose: an epsilon here would defeat the entire check.
            if (cpu != gpu) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: RNG diverges at draw %u: CPU %.9g, GPU %.9g\n"
                             "  shaders/includes/diff/rng.glsl and ohao/diff/rng/diff_rng.cpp must\n"
                             "  produce identical sequences -- path replay depends on it\n",
                             i, static_cast<double>(cpu), static_cast<double>(gpu));
                rngArena.destroy(ctx.allocator());
                return 1;
            }
        }
        if (cpuRng.drawCount() != kDraws) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: drawCount %u, expected %u\n",
                         cpuRng.drawCount(), kDraws);
            rngArena.destroy(ctx.allocator());
            return 1;
        }
        std::printf("[diff_gpu_probe] OK: CPU PathRng drawCount matches expected count (%u)\n",
                    kDraws);
        rngArena.destroy(ctx.allocator());
        std::printf("[diff_gpu_probe] OK: CPU and GLSL RNG agree bit-exactly over %u draws\n",
                    kDraws);
    }

    // 7. Wavefront path-state/queue/counter buffers: build, zero, and confirm
    //    every one of the PathStateFields (all kFieldCount of them, whatever
    //    that count is today) and both counter slots come back zeroed at
    //    exactly the expected element count. This is the
    //    substrate every bounce dispatch in the wavefront integrator reads
    //    and writes through -- state cannot live in registers across a
    //    dispatch boundary.
    {
        constexpr std::uint32_t kCapacity = 4096;
        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wavefront buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        for (std::uint32_t i = 0; i < ohao::diff::PathStateLayout::kFieldCount; ++i) {
            const auto field = static_cast<ohao::diff::PathStateField>(i);
            const std::vector<float> values = wf.readbackField(ctx.allocator(), field);
            // Explicit size check (not just empty()) guards against a vacuous
            // pass: a readback regression that silently truncated to a
            // smaller-but-nonempty vector would otherwise loop over fewer
            // elements than the field actually has and pass having verified
            // less than it claims.
            if (values.size() != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: field %u readback returned %zu elements, "
                             "expected %u\n",
                             i, values.size(), kCapacity);
                wf.destroy(ctx.allocator());
                return 1;
            }
            for (std::size_t e = 0; e < values.size(); ++e) {
                if (values[e] != 0.0f) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: field %u element %zu = %f, expected 0\n",
                                 i, e, values[e]);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }
        }

        for (std::uint32_t slot : {ohao::diff::WavefrontBuffers::kCurrentCountSlot,
                                    ohao::diff::WavefrontBuffers::kNextCountSlot}) {
            const std::uint32_t count = wf.readbackCounter(ctx.allocator(), slot);
            if (count != 0) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: counter slot %u = %u, expected 0\n",
                             slot, count);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }

        wf.destroy(ctx.allocator());
        std::printf("[diff_gpu_probe] OK: wavefront buffers zero across %u fields x %u paths "
                    "and both counter slots\n",
                    ohao::diff::PathStateLayout::kFieldCount, kCapacity);
    }

    // 8-10. Wavefront generate stage (shaders/diff/wf_generate.comp): one
    // dispatch, three checks against its output.
    //   8.  Closed form: generated directions reproduce the same hit
    //       distances check 3 validated analytically -- wf_generate.comp
    //       builds rays through camera_ray.glsl, the same include
    //       visibility_probe.comp uses, so this is what catches the two
    //       drifting apart.
    //   9.  Field round-trip: every PathStateField the shader wrote reads
    //       back on the CPU as what was written -- proves path_state.glsl
    //       and path_state_layout.hpp agree on the arena's byte layout, the
    //       same failure mode rng.glsl had (compiled, never executed).
    //   10. Queue population: the counter equals the pixel count exactly and
    //       queue 0 contains each path index exactly once -- an atomicAdd
    //       race would show up as duplicates or a short count.
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 48;
        constexpr uint32_t kCapacity = kW * kH;
        constexpr float kPlaneDistance = 2.0f;
        constexpr float kTanHalfFov = 0.2f;
        constexpr float kAspect = static_cast<float>(kW) / static_cast<float>(kH);

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_generate buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        // Default camera: origin (0,0,0), forward (0,0,-1), right (1,0,0),
        // up (0,1,0) -- the same convention checks 3/4's runVisibilityProbe
        // calls use.
        ohao::diff::WavefrontGenerateCamera camera;
        camera.tanHalfFov = kTanHalfFov;

        std::vector<uint32_t> queue0;
        if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_generate dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        const std::vector<float> originX = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::OriginX);
        const std::vector<float> originY = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::OriginY);
        const std::vector<float> originZ = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::OriginZ);
        const std::vector<float> dirX = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::DirX);
        const std::vector<float> dirY = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::DirY);
        const std::vector<float> dirZ = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::DirZ);
        const std::vector<float> throughputR = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputR);
        const std::vector<float> throughputG = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputG);
        const std::vector<float> throughputB = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputB);
        const std::vector<float> radianceR = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::RadianceR);
        const std::vector<float> radianceG = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::RadianceG);
        const std::vector<float> radianceB = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::RadianceB);
        const std::vector<float> pixelIndexField = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::PixelIndex);
        const std::vector<float> sampleIndexField = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::SampleIndex);
        const std::vector<float> bounceField = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::Bounce);
        const std::vector<float> aliveField = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::Alive);
        const std::vector<float> tangentR = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::TangentR);
        const std::vector<float> tangentG = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::TangentG);
        const std::vector<float> tangentB = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::TangentB);

        // Deliberately NOT all kFieldCount fields: wf_generate.comp does not
        // write HitT (that is wf_intersect.comp's output, checked
        // separately in check 12), so this list -- and the count printed
        // below, which is derived from its size rather than kFieldCount --
        // covers exactly what this stage actually produces.
        const std::vector<const std::vector<float>*> allFields = {
            &originX, &originY, &originZ, &dirX, &dirY, &dirZ,
            &throughputR, &throughputG, &throughputB,
            &radianceR, &radianceG, &radianceB,
            &pixelIndexField, &sampleIndexField, &bounceField, &aliveField,
            &tangentR, &tangentG, &tangentB,
        };
        for (const auto* f : allFields) {
            if (f->size() != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: wf_generate field readback size %zu, "
                             "expected %u\n",
                             f->size(), kCapacity);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }

        // 8. Closed form.
        float maxAbsError = 0.0f;
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const uint32_t i = y * kW + x;
                const float ndcX = 2.0f * (static_cast<float>(x) + 0.5f) / kW - 1.0f;
                const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / kH;
                const float dx = ndcX * kAspect * kTanHalfFov;
                const float dy = ndcY * kTanHalfFov;
                const float expectedT = kPlaneDistance * std::sqrt(1.0f + dx * dx + dy * dy);

                // origin=(0,0,0), plane at z=-planeDistance: t = -planeDistance / dir.z.
                // dir.z < 0 is guaranteed at this FOV, same as check 3.
                const float dz = dirZ[i];
                if (dz >= 0.0f) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: pixel (%u,%u) generated dir.z = %f, "
                                 "expected < 0\n",
                                 x, y, dz);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
                const float actualT = -kPlaneDistance / dz;
                const float err = std::fabs(actualT - expectedT);
                maxAbsError = std::max(maxAbsError, err);
                if (err > 1e-4f) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: pixel (%u,%u) generated-dir t = %f, "
                                 "closed form %f (|err| = %g)\n",
                                 x, y, actualT, expectedT, err);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }
        }
        std::printf("[diff_gpu_probe] OK: wf_generate directions reproduce closed-form plane "
                    "intersection over %u pixels (max |err| = %g)\n",
                    kCapacity, maxAbsError);

        // 9. Field round-trip.
        for (uint32_t i = 0; i < kCapacity; ++i) {
            if (originX[i] != camera.origin[0] || originY[i] != camera.origin[1] ||
                originZ[i] != camera.origin[2]) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u origin = (%f,%f,%f), expected "
                             "(%f,%f,%f)\n",
                             i, originX[i], originY[i], originZ[i], camera.origin[0],
                             camera.origin[1], camera.origin[2]);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (throughputR[i] != 1.0f || throughputG[i] != 1.0f || throughputB[i] != 1.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u throughput = (%f,%f,%f), expected "
                             "(1,1,1)\n",
                             i, throughputR[i], throughputG[i], throughputB[i]);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (radianceR[i] != 0.0f || radianceG[i] != 0.0f || radianceB[i] != 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u radiance = (%f,%f,%f), expected "
                             "(0,0,0)\n",
                             i, radianceR[i], radianceG[i], radianceB[i]);
                wf.destroy(ctx.allocator());
                return 1;
            }

            // The throughput tangent (Stage 1 Task 3). Exactly zero, as a
            // DERIVATIVE and not as a placeholder: generate writes a
            // throughput of exactly 1 for every parameter value, so
            // d(throughput)/d(theta) is 0 there for every theta.
            if (tangentR[i] != 0.0f || tangentG[i] != 0.0f || tangentB[i] != 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u throughput tangent = (%f,%f,%f), "
                             "expected (0,0,0)\n",
                             i, tangentR[i], tangentG[i], tangentB[i]);
                wf.destroy(ctx.allocator());
                return 1;
            }

            uint32_t pixelIndexBits = 0, sampleIndexBits = 0, bounceBits = 0, aliveBits = 0;
            std::memcpy(&pixelIndexBits, &pixelIndexField[i], sizeof(pixelIndexBits));
            std::memcpy(&sampleIndexBits, &sampleIndexField[i], sizeof(sampleIndexBits));
            std::memcpy(&bounceBits, &bounceField[i], sizeof(bounceBits));
            std::memcpy(&aliveBits, &aliveField[i], sizeof(aliveBits));

            if (pixelIndexBits != i) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: path %u pixelIndex = %u, expected %u\n",
                             i, pixelIndexBits, i);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (sampleIndexBits != 0u) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: path %u sampleIndex = %u, expected 0\n",
                             i, sampleIndexBits);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (bounceBits != 0u) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: path %u bounce = %u, expected 0\n", i,
                             bounceBits);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (aliveBits != 1u) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: path %u alive = %u, expected 1\n", i,
                             aliveBits);
                wf.destroy(ctx.allocator());
                return 1;
            }

            // Dir's exact components were already validated via the
            // closed-form distance check above; here just confirm it
            // round-tripped as a finite, unit-length vector -- catches e.g.
            // an offset error that silently aliased into the wrong field's
            // block without disturbing the z-only distance check.
            const float len2 = dirX[i] * dirX[i] + dirY[i] * dirY[i] + dirZ[i] * dirZ[i];
            if (std::fabs(len2 - 1.0f) > 1e-3f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u dir not unit length: |dir|^2 = %f\n",
                             i, len2);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: all %zu wf_generate-written PathStateFields round-trip "
                    "across %u paths (origin, dir, throughput=1, radiance=0, pixelIndex, "
                    "sampleIndex=0, bounce=0, alive=1)\n",
                    allFields.size(), kCapacity);

        // 10. Queue population.
        const std::uint32_t counter =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kCurrentCountSlot);
        if (counter != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: queue 0 counter = %u, expected %u\n",
                         counter, kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }
        if (queue0.size() != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: queue 0 readback size %zu, expected %u\n",
                         queue0.size(), kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }
        std::vector<uint32_t> sorted = queue0;
        std::sort(sorted.begin(), sorted.end());
        for (uint32_t i = 0; i < kCapacity; ++i) {
            if (sorted[i] != i) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: queue 0 sorted[%u] = %u, expected %u "
                             "(atomicAdd race: duplicate or missing path index)\n",
                             i, sorted[i], i);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: queue 0 counter = %u and contains each of %u path "
                    "indices exactly once\n",
                    counter, kCapacity);

        wf.destroy(ctx.allocator());
    }

    // 11. Layout-mapping probe (shaders/diff/wf_layout_probe.comp): proves
    // PathStateField enum order and path_state.glsl's PS_* constants agree,
    // field by field, in a way check 9 cannot. Check 9's values are
    // genuinely degenerate (origin (0,0,0), throughput (1,1,1), radiance
    // (0,0,0), sampleIndex and bounce both 0), so a transposition *within*
    // {OriginX,Y,Z}, within {ThroughputR,G,B}, within {RadianceR,G,B}, or
    // between SampleIndex and Bounce would round-trip there undetected.
    // Here every field gets a distinct value (1000+fieldIndex for floats,
    // 7000+fieldIndex for ints), so any permutation of the mapping mismatches
    // somewhere. Capacity 1000 is deliberately not a multiple of 64, so the
    // alignUp(capacity, 64) rounding path -- proven correct on paper for
    // capacity=1000 during review -- is exercised by actual execution here,
    // not just by the other checks' capacities (4096, all multiples of 64).
    {
        constexpr std::uint32_t kCapacity = 1000;

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: layout probe buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        if (!ctx.runWavefrontLayoutProbe(wf)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: layout probe dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        struct FieldExpect {
            ohao::diff::PathStateField field;
            const char* name;
            bool isInt;
            float expectedFloat;
            uint32_t expectedBits;
        };
        const FieldExpect expects[] = {
            {ohao::diff::PathStateField::OriginX, "OriginX", false, 1000.0f, 0u},
            {ohao::diff::PathStateField::OriginY, "OriginY", false, 1001.0f, 0u},
            {ohao::diff::PathStateField::OriginZ, "OriginZ", false, 1002.0f, 0u},
            {ohao::diff::PathStateField::DirX, "DirX", false, 1003.0f, 0u},
            {ohao::diff::PathStateField::DirY, "DirY", false, 1004.0f, 0u},
            {ohao::diff::PathStateField::DirZ, "DirZ", false, 1005.0f, 0u},
            {ohao::diff::PathStateField::ThroughputR, "ThroughputR", false, 1006.0f, 0u},
            {ohao::diff::PathStateField::ThroughputG, "ThroughputG", false, 1007.0f, 0u},
            {ohao::diff::PathStateField::ThroughputB, "ThroughputB", false, 1008.0f, 0u},
            {ohao::diff::PathStateField::RadianceR, "RadianceR", false, 1009.0f, 0u},
            {ohao::diff::PathStateField::RadianceG, "RadianceG", false, 1010.0f, 0u},
            {ohao::diff::PathStateField::RadianceB, "RadianceB", false, 1011.0f, 0u},
            {ohao::diff::PathStateField::PixelIndex, "PixelIndex", true, 0.0f, 7012u},
            {ohao::diff::PathStateField::SampleIndex, "SampleIndex", true, 0.0f, 7013u},
            {ohao::diff::PathStateField::Bounce, "Bounce", true, 0.0f, 7014u},
            {ohao::diff::PathStateField::Alive, "Alive", true, 0.0f, 7015u},
            {ohao::diff::PathStateField::HitT, "HitT", false, 1016.0f, 0u},
            {ohao::diff::PathStateField::NormalX, "NormalX", false, 1017.0f, 0u},
            {ohao::diff::PathStateField::NormalY, "NormalY", false, 1018.0f, 0u},
            {ohao::diff::PathStateField::NormalZ, "NormalZ", false, 1019.0f, 0u},
            {ohao::diff::PathStateField::TangentR, "TangentR", false, 1020.0f, 0u},
            {ohao::diff::PathStateField::TangentG, "TangentG", false, 1021.0f, 0u},
            {ohao::diff::PathStateField::TangentB, "TangentB", false, 1022.0f, 0u},
        };
        // This array must cover every PathStateField or the "all %u
        // PathStateFields" claim below is false -- exactly the coverage gap
        // a fix round caught here once already (HitT was added to the enum
        // in Task 5 but not to this array or to wf_layout_probe.comp until a
        // review found the mismatch). A build-time check so a future field
        // addition fails loudly here instead of silently narrowing what
        // "all" means.
        static_assert(sizeof(expects) / sizeof(expects[0]) ==
                          ohao::diff::PathStateLayout::kFieldCount,
                      "expects[] must have exactly kFieldCount entries -- add the new field's "
                      "row here AND its psSet* write in wf_layout_probe.comp");

        for (const FieldExpect& e : expects) {
            const std::vector<float> values = wf.readbackField(ctx.allocator(), e.field);
            if (values.size() != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: layout probe field %s readback size %zu, "
                             "expected %u\n",
                             e.name, values.size(), kCapacity);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (e.isInt) {
                uint32_t bits = 0;
                std::memcpy(&bits, &values[0], sizeof(bits));
                if (bits != e.expectedBits) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: layout probe field %s = %u, expected %u "
                                 "(field->offset mapping disagrees between "
                                 "path_state_layout.hpp and path_state.glsl)\n",
                                 e.name, bits, e.expectedBits);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            } else {
                if (values[0] != e.expectedFloat) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: layout probe field %s = %f, expected %f "
                                 "(field->offset mapping disagrees between "
                                 "path_state_layout.hpp and path_state.glsl)\n",
                                 e.name, values[0], e.expectedFloat);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }
        }

        wf.destroy(ctx.allocator());
        std::printf("[diff_gpu_probe] OK: layout probe -- all %u PathStateFields hold their "
                    "distinct expected value at capacity=%u (non-multiple-of-64)\n",
                    ohao::diff::PathStateLayout::kFieldCount, kCapacity);
    }

    // 12. Wavefront intersect stage (shaders/diff/wf_intersect.comp) with
    // compaction, driven through an indirect dispatch prepared by
    // shaders/diff/wf_prepare_indirect.comp. This is the check that catches
    // an atomicAdd compaction race: it works because, for this specific
    // scene, the surviving set is knowable analytically rather than just
    // measured.
    //
    // Reuses check 4's half-quad (quadMinY=0.0): only rays with ndcY > 0
    // hit, i.e. pixel rows y < kH/2. Because pixelIndex = y*kW+x (row-major,
    // same as wf_generate.comp), that set is exactly the contiguous range
    // [0, kW*kH/2) = [0, 1536) -- so queue ring 1, sorted, must equal
    // 0..1535 with nothing else, and counter slot kNextCountSlot must read
    // exactly 1536.
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 48;
        constexpr uint32_t kCapacity = kW * kH;  // 3072
        constexpr uint32_t kExpectedSurvivors = kCapacity / 2;  // 1536
        constexpr float kPlaneDistance = 2.0f;
        constexpr float kTanHalfFov = 0.2f;
        constexpr float kAspect = static_cast<float>(kW) / static_cast<float>(kH);

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_intersect buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        // Populate queue ring 0 / counter slot 0 first, as its own fully
        // queue-idle-separated submission (see runWavefrontGenerateProbe) --
        // this is not the barrier under test, so it stays maximally safe.
        ohao::diff::WavefrontGenerateCamera camera;
        camera.tanHalfFov = kTanHalfFov;
        std::vector<uint32_t> queue0;
        if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_intersect setup: wf_generate dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        // wf_prepare_indirect.comp + the indirectly-dispatched wf_intersect.comp,
        // all on one command buffer with the SHADER_WRITE -> INDIRECT_COMMAND_READ
        // barrier between them -- see gpu_probe_context.cpp's
        // runWavefrontIntersectProbe for exactly which barriers this records.
        std::vector<uint32_t> queue1;
        if (!ctx.runWavefrontIntersectProbe(wf, kPlaneDistance, /*quadMinY=*/0.0f, queue1)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_intersect dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        const std::uint32_t nextCount =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kNextCountSlot);
        if (nextCount != kExpectedSurvivors) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: queue ring 1 counter = %u, expected exactly %u "
                         "(compaction race: lost or duplicated an atomicAdd offset)\n",
                         nextCount, kExpectedSurvivors);
            wf.destroy(ctx.allocator());
            return 1;
        }

        // The canary must equal exactly the number of invocations the
        // indirect dispatch launched. kCapacity (3072) is an exact multiple
        // of wf_intersect.comp's local_size_x=64, so
        // groupCountX = kCapacity/64 with no rounding tail, and every
        // invocation runs (queue ring 0 holds all kCapacity paths). A wrong
        // kCanarySlot or a wrong push field would still leave check 13's
        // "canary == 0 on an empty queue" true, so that check alone cannot
        // catch this; asserting a specific *nonzero* expected value here is
        // what turns the canary into a real differential rather than a check
        // that only ever needs to prove absence.
        const std::uint32_t canary =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kCanarySlot);
        if (canary != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: canary = %u, expected exactly %u invocations "
                         "(wrong kCanarySlot or push field would read 0 here and only be caught "
                         "by this nonzero assertion, not check 13's empty-queue one)\n",
                         canary, kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }

        if (queue1.size() != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: queue ring 1 readback size %zu, expected %u\n",
                         queue1.size(), kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }

        // The written prefix, sorted, must be exactly [0, kExpectedSurvivors)
        // with no duplicates and nothing extra.
        std::vector<uint32_t> survivors(queue1.begin(), queue1.begin() + kExpectedSurvivors);
        std::sort(survivors.begin(), survivors.end());
        for (uint32_t i = 0; i < kExpectedSurvivors; ++i) {
            if (survivors[i] != i) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: queue ring 1 sorted[%u] = %u, expected %u "
                             "(compaction race: duplicate or missing path index)\n",
                             i, survivors[i], i);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        // The tail beyond what compaction actually wrote, [kExpectedSurvivors,
        // kCapacity), must still read as wf.zero()'s initial fill (0) --
        // never inspected until now. This is a "nothing extra was written"
        // sanity check, not a live exercise of the dstSlot < capacity guard
        // itself: at kCapacity=3072 with kExpectedSurvivors bounding dstSlot
        // well under capacity, that guard is provably unreachable in this
        // configuration. It still catches an off-by-one that spills the
        // compacted prefix past kExpectedSurvivors, a class the sorted-prefix
        // check above cannot see since that check only ever looks at
        // [0, kExpectedSurvivors).
        for (uint32_t i = kExpectedSurvivors; i < kCapacity; ++i) {
            if (queue1[i] != 0u) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: queue ring 1 tail[%u] = %u, expected 0 "
                             "(a write landed past the compacted prefix -- possible unclamped "
                             "dstSlot)\n",
                             i, queue1[i]);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }

        std::printf("[diff_gpu_probe] OK: wf_intersect compacted exactly %u survivors into queue "
                    "ring 1 (indices 0..%u, no duplicates, no dead paths, tail untouched)\n",
                    kExpectedSurvivors, kExpectedSurvivors - 1);

        // Bonus correctness beyond the brief's minimum: every survivor's
        // Alive flag stayed 1 and HitT matches check 8's closed form; every
        // dead path's Alive flag was cleared and HitT reads -1 (miss).
        const std::vector<float> aliveField =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::Alive);
        const std::vector<float> hitTField =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::HitT);
        if (aliveField.size() != kCapacity || hitTField.size() != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: wf_intersect Alive/HitT readback size mismatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }
        float maxAbsError = 0.0f;
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const uint32_t i = y * kW + x;
                uint32_t aliveBits = 0;
                std::memcpy(&aliveBits, &aliveField[i], sizeof(aliveBits));
                const bool expectAlive = i < kExpectedSurvivors;
                if (static_cast<bool>(aliveBits) != expectAlive) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: path %u Alive = %u, expected %u\n",
                                 i, aliveBits, expectAlive ? 1u : 0u);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
                if (expectAlive) {
                    const float ndcX = 2.0f * (static_cast<float>(x) + 0.5f) / kW - 1.0f;
                    const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / kH;
                    const float dx = ndcX * kAspect * kTanHalfFov;
                    const float dy = ndcY * kTanHalfFov;
                    const float expectedT = kPlaneDistance * std::sqrt(1.0f + dx * dx + dy * dy);
                    const float err = std::fabs(hitTField[i] - expectedT);
                    maxAbsError = std::max(maxAbsError, err);
                    if (err > 1e-4f) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: path %u HitT = %f, closed form %f "
                                     "(|err| = %g)\n",
                                     i, hitTField[i], expectedT, err);
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                } else if (hitTField[i] != -1.0f) {
                    std::fprintf(stderr, "[diff_gpu_probe] FAIL: path %u (dead) HitT = %f, expected -1\n",
                                 i, hitTField[i]);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }
        }
        std::printf("[diff_gpu_probe] OK: wf_intersect Alive/HitT match compaction membership and "
                    "closed-form plane intersection (max |err| = %g)\n", maxAbsError);

        wf.destroy(ctx.allocator());
    }

    // 13. Indirect dispatch of an empty queue costs nothing -- the property
    // that makes a dead path genuinely free rather than just skipped-but-
    // still-paid-for. Counter slot kCurrentCountSlot is left at 0 (no
    // wf_generate call), so wf_prepare_indirect.comp computes
    // groupCountX = (0+63)/64 = 0, and vkCmdDispatchIndirect with
    // groupCountX == 0 launches zero workgroups -- wf_intersect.comp's very
    // first instruction, an unconditional atomicAdd on a canary counter,
    // never executes. A stage that merely early-returned per-invocation
    // (rather than never launching) would still show this canary at 0, so
    // the real claim under test is the dispatch cost, not just the output --
    // this check is a necessary but not sufficient witness of that; it is
    // the best a black-box GPU probe can observe without a timing query.
    {
        constexpr uint32_t kCapacity = 64;

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: empty-queue buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });
        // Deliberately no wf_generate call: counter slot kCurrentCountSlot
        // stays at 0 from wf.zero(), which is exactly the input under test.

        std::vector<uint32_t> queue1;
        if (!ctx.runWavefrontIntersectProbe(wf, /*planeDistance=*/2.0f, /*quadMinY=*/0.0f, queue1)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: empty-queue wf_intersect dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        const std::uint32_t canary =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kCanarySlot);
        if (canary != 0) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: canary = %u, expected 0 -- an indirect dispatch "
                         "sized from a live-count of 0 ran %u invocation(s)\n",
                         canary, canary);
            wf.destroy(ctx.allocator());
            return 1;
        }
        const std::uint32_t nextCount =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kNextCountSlot);
        if (nextCount != 0) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: next-queue counter = %u, expected 0\n",
                         nextCount);
            wf.destroy(ctx.allocator());
            return 1;
        }
        std::printf("[diff_gpu_probe] OK: indirect dispatch from a live-count of 0 ran zero "
                    "invocations (canary = 0, next-queue counter = 0)\n");

        wf.destroy(ctx.allocator());
    }

    // 14-15. Wavefront scatter stage (shaders/diff/wf_scatter.comp), run for
    // 4 real bounces: generate -> intersect once (full quad, quadMinY=-1, so
    // every ray hits and seeds every path into scatter) -> scatter x4,
    // ping-ponging the SAME two physical queue rings (each scatter call
    // zeroes its own destination counter slot first -- see
    // GpuProbeContext::runWavefrontScatterProbe's doc comment for why that
    // is load-bearing once a ring/slot pair is reused).
    //
    // Check 14 is the analytic throughput check: with albedo p=0.5 and every
    // ray surviving every bounce, throughput after 4 bounces must be exactly
    // p^4 = 0.0625, compared with ==, not a tolerance. This proves Throughput
    // and Bounce survive 4 dispatch boundaries -- it says nothing about
    // Origin/Dir, which this loop's single intersect call leaves stale after
    // bounce 0 (see the file header's note on check 14 for why).
    //
    // Check 15 is the RNG-parity check: for one chosen path, the exact
    // (u1, u2, drawCount) wf_scatter.comp computed at each of the 4 real,
    // separate dispatches must match ohao::diff::PathRng::forPath(...)
    // replayed the same number of draws on the CPU -- proving the GPU
    // reconstructs the RNG from (pixelIndex, sampleIndex, bounce) correctly
    // across a dispatch boundary, not just within a single dispatch (which
    // check 6 already covers).
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 48;
        constexpr uint32_t kCapacity = kW * kH;  // 3072
        constexpr float kPlaneDistance = 2.0f;
        constexpr float kTanHalfFov = 0.2f;
        constexpr float kAlbedo = 0.5f;
        constexpr uint32_t kIterationSeed = 20260828u;
        constexpr uint32_t kBounces = 4;
        // Must match wf_scatter.comp's kDrawsPerBounce. It became 3 in Stage
        // 0b-2b Task 2 (the BSDF draws a 2-D direction sample AND a 1-D lobe
        // choice every bounce) and 5 in Task 3 (two more for the environment
        // importance sample). The two VALUES compared below are unchanged
        // through both: the shader still draws the direction sample FIRST and
        // appends new draws after the old ones, so the debug sink still
        // records the same two stream positions -- only the per-bounce stride
        // moved, which is precisely what this constant exists to pin. Get it
        // wrong and the fast-forward in the shader and the replay here walk
        // different streams, which is the failure this check exists for.
        constexpr uint32_t kDrawsPerBounce = 5;
        constexpr uint32_t kChosenPath = 1234;   // arbitrary, < kCapacity
        static_assert(kChosenPath < kCapacity, "kChosenPath must be a valid path index");

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_scatter buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        ohao::diff::WavefrontGenerateCamera camera;
        camera.tanHalfFov = kTanHalfFov;
        std::vector<uint32_t> queue0;
        if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_scatter setup: wf_generate dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        // Full quad (quadMinY=-1.0): every ray hits, so every one of
        // kCapacity paths survives into scatter ring1/slot(kNextCountSlot).
        std::vector<uint32_t> queue1;
        if (!ctx.runWavefrontIntersectProbe(wf, kPlaneDistance, /*quadMinY=*/-1.0f, queue1)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_scatter setup: wf_intersect dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }
        const std::uint32_t seededCount =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kNextCountSlot);
        if (seededCount != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: wf_scatter setup: %u of %u rays hit the full "
                         "quad, expected all of them (quadMinY=-1 should guarantee every ray "
                         "hits)\n",
                         seededCount, kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }

        // Ping-pong: scatter's first source is the ring/slot wf_intersect
        // just filled (ring1/kNextCountSlot); its destination is the other
        // ring (ring0/kCurrentCountSlot), which currently holds a stale
        // pre-intersect count and gets zeroed by runWavefrontScatterProbe.
        uint32_t srcQueueBase = kCapacity;  // ring 1
        uint32_t srcCountSlot = ohao::diff::WavefrontBuffers::kNextCountSlot;
        uint32_t dstQueueBase = 0;  // ring 0
        uint32_t dstCountSlot = ohao::diff::WavefrontBuffers::kCurrentCountSlot;

        std::vector<std::vector<float>> drawsPerBounce(kBounces);
        bool scatterOk = true;
        for (uint32_t b = 0; b < kBounces && scatterOk; ++b) {
            std::vector<uint32_t> outQueue;
            std::vector<float> outDraws;
            if (!ctx.runWavefrontScatterProbe(wf, srcQueueBase, srcCountSlot, dstQueueBase,
                                              dstCountSlot, kAlbedo, kIterationSeed, outQueue,
                                              outDraws)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_scatter dispatch at bounce %u\n", b);
                scatterOk = false;
                break;
            }
            const std::uint32_t survCount = wf.readbackCounter(ctx.allocator(), dstCountSlot);
            if (survCount != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: wf_scatter bounce %u re-queued %u paths, "
                             "expected all %u (every invocation must re-queue -- nothing "
                             "terminates in scatter yet)\n",
                             b, survCount, kCapacity);
                scatterOk = false;
                break;
            }
            // Inspect the re-queued ring itself, mirroring check 12's
            // sorted-prefix inspection of wf_intersect's compaction output:
            // survCount alone is a single scalar and cannot distinguish
            // "every path re-queued exactly once" from "some path's slot
            // got overwritten while another was dropped, but the atomicAdd
            // count still landed on kCapacity by coincidence." This is
            // precisely the ring GpuProbeContext::runWavefrontScatterProbe
            // was already paying to copy back as outQueue -- it was going
            // unread before this check existed, which is exactly the blind
            // spot that let wf_scatter.comp ship without wf_intersect.comp's
            // dstSlot < capacity guard (Important 1): a missing bounds guard
            // changes what lands in this ring without necessarily changing
            // the counter atomicAdd returns.
            if (outQueue.size() != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: wf_scatter bounce %u re-queued ring readback "
                             "size %zu, expected %u\n",
                             b, outQueue.size(), kCapacity);
                scatterOk = false;
                break;
            }
            std::vector<uint32_t> sortedQueue = outQueue;
            std::sort(sortedQueue.begin(), sortedQueue.end());
            for (uint32_t i = 0; i < kCapacity; ++i) {
                if (sortedQueue[i] != i) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: wf_scatter bounce %u re-queued ring "
                                 "sorted[%u] = %u, expected %u (duplicate or dead path index -- "
                                 "every survivor must appear exactly once)\n",
                                 b, i, sortedQueue[i], i);
                    scatterOk = false;
                    break;
                }
            }
            if (!scatterOk) break;
            // The stride is ohao::diff::kDebugDrawFloats, not a bare 3: that
            // constant is TIED to the shader's own write statements by
            // checkWfScatterSinkLayoutTie, and the two literal `3u`s that
            // used to sit here were the one place in this file where the
            // record's width was transcribed rather than referenced. Stage 1
            // Task 1 widened the record from (u1, u2, drawCount) to a full
            // per-vertex trace and these were what noticed -- correctly, and
            // for the wrong reason: they were asserting a size, not a
            // meaning. The values this check reads are still slots 0, 1 and
            // 2, unchanged.
            if (outDraws.size() !=
                static_cast<std::size_t>(kCapacity) * ohao::diff::kDebugDrawFloats) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: wf_scatter bounce %u debug-draws readback "
                             "size %zu, expected %zu\n",
                             b, outDraws.size(),
                             static_cast<std::size_t>(kCapacity) * ohao::diff::kDebugDrawFloats);
                scatterOk = false;
                break;
            }
            drawsPerBounce[b] = std::move(outDraws);
            std::swap(srcQueueBase, dstQueueBase);
            std::swap(srcCountSlot, dstCountSlot);
        }
        if (!scatterOk) {
            wf.destroy(ctx.allocator());
            return 1;
        }
        std::printf("[diff_gpu_probe] OK: wf_scatter re-queued ring contains each of %u path "
                    "indices exactly once at every one of %u bounces (no duplicates, no dead "
                    "paths)\n",
                    kCapacity, kBounces);

        // 14. Throughput decay -- exact, no tolerance. Hardcoded to the
        // literal 0.0625f rather than computed as kAlbedo^kBounces here: if
        // this were derived from kAlbedo, perturbing kAlbedo alone (as
        // task-6-report.md's required proof does) would perturb BOTH sides
        // of the comparison identically and the check could never fail --
        // exactly the kind of tautological check the brief warns about. The
        // expected value must come from an independent source (arithmetic
        // done by hand: 0.5*0.5*0.5*0.5 = 0.0625), not from the GLSL/CPU
        // path currently under test.
        constexpr float expectedThroughput = 0.0625f;

        const std::vector<float> throughputR =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputR);
        const std::vector<float> throughputG =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputG);
        const std::vector<float> throughputB =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputB);
        if (throughputR.size() != kCapacity || throughputG.size() != kCapacity ||
            throughputB.size() != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_scatter throughput readback size "
                                  "mismatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }
        for (uint32_t i = 0; i < kCapacity; ++i) {
            // Bit-exact on purpose -- p=0.5 keeps every intermediate product
            // exactly representable in float32, so an epsilon here would
            // defeat the entire point of the check.
            if (throughputR[i] != expectedThroughput || throughputG[i] != expectedThroughput ||
                throughputB[i] != expectedThroughput) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u throughput = (%.9g,%.9g,%.9g) after "
                             "%u bounces, expected exactly (%.9g,%.9g,%.9g) -- Throughput did not "
                             "survive every dispatch boundary intact\n",
                             i, static_cast<double>(throughputR[i]), static_cast<double>(throughputG[i]),
                             static_cast<double>(throughputB[i]), kBounces,
                             static_cast<double>(expectedThroughput),
                             static_cast<double>(expectedThroughput),
                             static_cast<double>(expectedThroughput));
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: wf_scatter throughput decay after %u bounces is exactly "
                    "%.9g (p=%.9g) for all %u paths\n",
                    kBounces, static_cast<double>(expectedThroughput), static_cast<double>(kAlbedo),
                    kCapacity);

        // 15. Per-bounce RNG parity for one chosen path.
        ohao::diff::PathRng cpuRng =
            ohao::diff::PathRng::forPath(kChosenPath, /*sampleIndex=*/0u, kIterationSeed);
        for (uint32_t b = 0; b < kBounces; ++b) {
            const float cpuU1 = cpuRng.next1D();
            const float cpuU2 = cpuRng.next1D();
            // Draws 3, 4 and 5 of the bounce: wf_scatter.comp's
            // lobe-selection sample and the environment sample's two
            // uniforms. Their values are not recorded in the debug sink
            // (which holds three floats per path and spends the third on the
            // draw count), but they MUST be consumed here or every later
            // bounce's u1/u2 comparison would be off by that many stream
            // positions.
            (void)cpuRng.next1D();
            (void)cpuRng.next1D();
            (void)cpuRng.next1D();
            const std::uint32_t cpuDrawCount = cpuRng.drawCount();

            const std::vector<float>& gpuDraws = drawsPerBounce[b];
            const float gpuU1 = gpuDraws[static_cast<std::size_t>(kChosenPath) * ohao::diff::kDebugDrawFloats + 0u];
            const float gpuU2 = gpuDraws[static_cast<std::size_t>(kChosenPath) * ohao::diff::kDebugDrawFloats + 1u];
            std::uint32_t gpuDrawCount = 0;
            std::memcpy(&gpuDrawCount, &gpuDraws[static_cast<std::size_t>(kChosenPath) * ohao::diff::kDebugDrawFloats + 2u],
                       sizeof(gpuDrawCount));

            if (cpuU1 != gpuU1 || cpuU2 != gpuU2) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u RNG diverges at bounce %u: CPU "
                             "(%.9g,%.9g), GPU (%.9g,%.9g) -- wf_scatter.comp's reconstruction "
                             "from (pixelIndex, sampleIndex, bounce) does not match "
                             "ohao::diff::PathRng replayed the same number of draws\n",
                             kChosenPath, b, static_cast<double>(cpuU1), static_cast<double>(cpuU2),
                             static_cast<double>(gpuU1), static_cast<double>(gpuU2));
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (cpuDrawCount != gpuDrawCount) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u drawCount diverges at bounce %u: CPU "
                             "%u, GPU %u (expected %u -- (bounce+1)*%u)\n",
                             kChosenPath, b, cpuDrawCount, gpuDrawCount, (b + 1) * kDrawsPerBounce,
                             kDrawsPerBounce);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (cpuDrawCount != (b + 1) * kDrawsPerBounce) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u drawCount at bounce %u = %u, expected "
                             "%u\n",
                             kChosenPath, b, cpuDrawCount, (b + 1) * kDrawsPerBounce);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: wf_scatter per-bounce RNG draws (values and drawCount) "
                    "match ohao::diff::PathRng exactly across %u dispatch boundaries for path %u\n",
                    kBounces, kChosenPath);

        wf.destroy(ctx.allocator());
    }

    // 16-18. THE FUSED BOUNCE LOOP (ohao::diff::WavefrontLoop, Stage 0b-2a
    // Task 3). Everything above this point runs one wavefront stage per
    // command-buffer submission, with a vkQueueWaitIdle between every stage.
    // That idle wait is a full device barrier: it silently satisfies every
    // ordering requirement the stages have on each other, which is why
    // checks 12-15 can pass with barriers that would be wrong in a real
    // integrator. This block removes it. generate, and then
    // prepare_indirect/intersect/prepare_indirect/scatter once per bounce,
    // are recorded into ONE command buffer, and ordering becomes the
    // barriers' job for the first time (see wavefront_loop.hpp).
    //
    // The assertions are deliberately the SAME two analytic properties
    // checks 14 and 15 already prove stage-by-stage -- throughput exactly
    // albedo^bounces, and per-bounce RNG draws bit-identical to
    // ohao::diff::PathRng -- not new, weaker ones. The whole point is to
    // re-run a property that is already known to hold under the idle wait,
    // against the same shaders, with only the synchronization changed.
    //
    // 16. Every path survives every bounce, and the live ring holds each
    //     path index exactly once -- the compaction offsets are not
    //     displaced. This is the check that catches a stale (unzeroed)
    //     destination counter slot: an atomicAdd based on a non-zero
    //     starting value hands out offsets past the end of the ring, whose
    //     writes wf_scatter.comp's `dstSlot < capacity` guard then drops,
    //     leaving holes.
    // 17. Throughput after 4 fused bounces is exactly 0.0625.
    // 18. Per-bounce RNG draws match ohao::diff::PathRng exactly.
    {
        // height MUST be 8 because every expected value below is
        // calibrated to exactly 512 paths at 64x8 -- the 0.0625 throughput,
        // the per-bounce PathRng parity for kChosenPath, the live counts.
        // It is NOT a dispatch limitation: WavefrontStage::Fixed carries
        // groupsY/groupsZ and can dispatch a genuine 3-D grid, so a stage
        // recorded through WavefrontLoop can cover any resolution just as
        // checks 8-10's hand-recorded 2-D generate dispatch does.
        // runWavefrontFusedLoopProbe leaves groupsY/groupsZ at 1 and
        // dispatches (width/8, 1, 1) x local_size (8,8), covering exactly 8
        // pixel rows. Changing the resolution means recomputing the
        // expectations here first.
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 8;
        constexpr uint32_t kCapacity = kW * kH;  // 512
        constexpr float kAlbedo = 0.5f;
        constexpr uint32_t kIterationSeed = 20260828u;
        constexpr uint32_t kBounces = 4;
        // Must match wf_scatter.comp's kDrawsPerBounce. It became 3 in Stage
        // 0b-2b Task 2 (the BSDF draws a 2-D direction sample AND a 1-D lobe
        // choice every bounce) and 5 in Task 3 (two more for the environment
        // importance sample). The two VALUES compared below are unchanged
        // through both: the shader still draws the direction sample FIRST and
        // appends new draws after the old ones, so the debug sink still
        // records the same two stream positions -- only the per-bounce stride
        // moved, which is precisely what this constant exists to pin. Get it
        // wrong and the fast-forward in the shader and the replay here walk
        // different streams, which is the failure this check exists for.
        constexpr uint32_t kDrawsPerBounce = 5;
        constexpr uint32_t kChosenPath = 333;    // arbitrary, < kCapacity
        static_assert(kChosenPath < kCapacity, "kChosenPath must be a valid path index");

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: fused loop buffers build\n");
            return 1;
        }

        std::vector<std::vector<float>> fusedDraws;
        std::vector<uint32_t> fusedLiveCounts;
        std::vector<uint32_t> fusedFinalQueue;
        if (!ctx.runWavefrontFusedLoopProbe(wf, kW, kH, kBounces, kAlbedo, kIterationSeed,
                                            fusedDraws, fusedLiveCounts, fusedFinalQueue)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: fused loop dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        // 16. Survivors and compaction integrity.
        //
        // A live count of exactly kCapacity at every bounce depth is not a
        // given: it is what the closed-box scene (see
        // runWavefrontFusedLoopProbe's doc comment) is built to guarantee,
        // and asserting it is what stops the throughput check below from
        // passing vacuously over an empty survivor set.
        if (fusedLiveCounts.size() != kBounces) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: fused loop returned %zu live counts, expected "
                         "%u\n",
                         fusedLiveCounts.size(), kBounces);
            wf.destroy(ctx.allocator());
            return 1;
        }
        for (uint32_t b = 0; b < kBounces; ++b) {
            if (fusedLiveCounts[b] != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop of %u bounces left %u live paths, "
                             "expected all %u -- survival here is conditional on wf_intersect.comp "
                             "writing the CORRECT stored normal (see the survival derivation's "
                             "\"Normal\" step in gpu_probe_context.cpp), so a wrong or missing "
                             "normal sending a path's scattered direction out through the wrong "
                             "face is the most likely cause; also consider a path that escaped the "
                             "closed box scene some other way, or a compaction counter slot not "
                             "zeroed before its atomicAdd\n",
                             b + 1u, fusedLiveCounts[b], kCapacity);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        // Same sorted-prefix inspection check 12/14 apply to the
        // stage-by-stage rings: the scalar count alone cannot distinguish
        // "every path re-queued exactly once" from "one slot overwritten and
        // another dropped, with the atomicAdd total landing on kCapacity
        // anyway."
        if (fusedFinalQueue.size() != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: fused loop final ring readback size %zu, expected "
                         "%u\n",
                         fusedFinalQueue.size(), kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }
        std::vector<uint32_t> sortedFused = fusedFinalQueue;
        std::sort(sortedFused.begin(), sortedFused.end());
        for (uint32_t i = 0; i < kCapacity; ++i) {
            if (sortedFused[i] != i) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop final ring sorted[%u] = %u, "
                             "expected %u (duplicate or missing path index -- compaction offsets "
                             "are displaced)\n",
                             i, sortedFused[i], i);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: fused loop kept all %u paths alive through %u bounces "
                    "and its final ring holds each path index exactly once\n",
                    kCapacity, kBounces);

        // 17. Throughput decay -- exact, no tolerance. Hardcoded to the
        // literal 0.0625f for the same reason check 14 hardcodes it:
        // deriving it from kAlbedo would perturb both sides of the
        // comparison identically and the check could never fail.
        // 0.5*0.5*0.5*0.5 = 0.0625, arithmetic done by hand.
        constexpr float expectedFusedThroughput = 0.0625f;

        const std::vector<float> fusedR =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputR);
        const std::vector<float> fusedG =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputG);
        const std::vector<float> fusedB =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputB);
        if (fusedR.size() != kCapacity || fusedG.size() != kCapacity ||
            fusedB.size() != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: fused loop throughput readback size "
                                  "mismatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }
        for (uint32_t i = 0; i < kCapacity; ++i) {
            // Bit-exact on purpose -- p=0.5 keeps every intermediate product
            // exactly representable in float32.
            if (fusedR[i] != expectedFusedThroughput || fusedG[i] != expectedFusedThroughput ||
                fusedB[i] != expectedFusedThroughput) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop path %u throughput = "
                             "(%.9g,%.9g,%.9g) after %u bounces, expected exactly "
                             "(%.9g,%.9g,%.9g) -- a bounce ran the wrong number of times, or "
                             "Throughput did not survive a dispatch boundary inside the fused "
                             "command buffer\n",
                             i, static_cast<double>(fusedR[i]), static_cast<double>(fusedG[i]),
                             static_cast<double>(fusedB[i]), kBounces,
                             static_cast<double>(expectedFusedThroughput),
                             static_cast<double>(expectedFusedThroughput),
                             static_cast<double>(expectedFusedThroughput));
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: fused loop throughput decay after %u bounces is exactly "
                    "%.9g (p=%.9g) for all %u paths\n",
                    kBounces, static_cast<double>(expectedFusedThroughput),
                    static_cast<double>(kAlbedo), kCapacity);

        // 18. Per-bounce RNG parity, identical in form to check 15.
        // fusedDraws[b] is what bounce b's scatter dispatch wrote; see
        // runWavefrontFusedLoopProbe's doc comment for how each bounce's
        // draws are observed despite wf_scatter.comp writing them at a fixed
        // per-path offset.
        if (fusedDraws.size() != kBounces) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: fused loop returned %zu draw sets, expected %u\n",
                         fusedDraws.size(), kBounces);
            wf.destroy(ctx.allocator());
            return 1;
        }
        ohao::diff::PathRng fusedCpuRng =
            ohao::diff::PathRng::forPath(kChosenPath, /*sampleIndex=*/0u, kIterationSeed);
        for (uint32_t b = 0; b < kBounces; ++b) {
            const float cpuU1 = fusedCpuRng.next1D();
            const float cpuU2 = fusedCpuRng.next1D();
            // wf_scatter.comp's lobe-selection sample and the environment
            // sample's two uniforms -- see check 15.
            (void)fusedCpuRng.next1D();
            (void)fusedCpuRng.next1D();
            (void)fusedCpuRng.next1D();
            const std::uint32_t cpuDrawCount = fusedCpuRng.drawCount();

            const std::vector<float>& gpuDraws = fusedDraws[b];
            // ohao::diff::kDebugDrawFloats, not a bare 3 -- see the identical
            // note on check 14's copy of this guard above.
            if (gpuDraws.size() !=
                static_cast<std::size_t>(kCapacity) * ohao::diff::kDebugDrawFloats) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop bounce %u debug-draws size %zu, "
                             "expected %zu\n",
                             b, gpuDraws.size(),
                             static_cast<std::size_t>(kCapacity) * ohao::diff::kDebugDrawFloats);
                wf.destroy(ctx.allocator());
                return 1;
            }
            const float gpuU1 = gpuDraws[static_cast<std::size_t>(kChosenPath) * ohao::diff::kDebugDrawFloats + 0u];
            const float gpuU2 = gpuDraws[static_cast<std::size_t>(kChosenPath) * ohao::diff::kDebugDrawFloats + 1u];
            std::uint32_t gpuDrawCount = 0;
            std::memcpy(&gpuDrawCount, &gpuDraws[static_cast<std::size_t>(kChosenPath) * ohao::diff::kDebugDrawFloats + 2u],
                       sizeof(gpuDrawCount));

            if (cpuU1 != gpuU1 || cpuU2 != gpuU2) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop path %u RNG diverges at bounce %u: "
                             "CPU (%.9g,%.9g), GPU (%.9g,%.9g) -- the fused loop replayed a "
                             "different random stream than ohao::diff::PathRng\n",
                             kChosenPath, b, static_cast<double>(cpuU1), static_cast<double>(cpuU2),
                             static_cast<double>(gpuU1), static_cast<double>(gpuU2));
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (cpuDrawCount != gpuDrawCount || cpuDrawCount != (b + 1) * kDrawsPerBounce) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop path %u drawCount at bounce %u: "
                             "CPU %u, GPU %u, expected %u ((bounce+1)*%u) -- the Bounce field the "
                             "fast-forward reads did not advance exactly once per fused bounce\n",
                             kChosenPath, b, cpuDrawCount, gpuDrawCount,
                             (b + 1) * kDrawsPerBounce, kDrawsPerBounce);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: fused loop per-bounce RNG draws (values and drawCount) "
                    "match ohao::diff::PathRng exactly across %u fused bounces for path %u\n",
                    kBounces, kChosenPath);

        wf.destroy(ctx.allocator());
    }

    // 19. GEOMETRIC NORMALS (Stage 0b-2b Task 1). wf_intersect.comp must
    // write the hit's real, forward-facing geometric normal into path state
    // (PathStateField::NormalX/Y/Z), and this asserts it against an oracle
    // that is pure analytic geometry computed here on the host -- the box's
    // face planes and the camera's closed-form ray -- with nothing the
    // shader computed anywhere in it. (The Dir field the GPU wrote is
    // deliberately NOT read: it is check 8's subject, not this check's
    // oracle.)
    //
    // WHY A BOX AND NOT THE QUAD every other intersect check uses: the
    // single quad at z = -planeDistance, seen from a camera at the origin
    // looking down -Z, has exactly one forward-facing normal, (0,0,1) --
    // which is precisely the value wf_scatter.comp used to hardcode. A
    // check built on that scene cannot tell a real normal from the constant.
    // A closed box entered from its centre reaches five of its six faces, so
    // five distinct analytic normals are asserted, and the box is wound
    // OUTWARD (see buildAxisAlignedBoxGeometry) so that every one of those
    // hits also has to go through the flip-to-oppose-the-ray step.
    //
    // GEOMETRY OF THE ORACLE. The camera sits at the box centre, so for a
    // unit direction d the exit distance through the face on axis k is
    // t_k = E / |d_k|; the face actually hit is the argmin over k, i.e. the
    // argmax of |d_k| (E is the same on all three axes). Its inward normal
    // -- which is the forward-facing one, since the ray leaves the box
    // through that face -- is -sign(d_k) * e_k.
    //
    // TIE-FREEDOM IS BY CONSTRUCTION, NOT BY LUCK. That argmax is only
    // well-defined if no two |d_k| are equal. With kW even and kH ODD:
    //   |d_x| ~ |2x + 1 - kW| * tanHalfFov / kH  (odd numerator)
    //   |d_y| ~ |kH - 2y - 1| * tanHalfFov / kH  (even numerator)
    // so |d_x| == |d_y| would need an odd integer to equal an even one, and
    // |d_x| == |d_z| (== 1 before normalisation) would need
    // |2x + 1 - kW| == kH / tanHalfFov == 24.5, not an integer. The closest
    // approach of any pair is therefore >= 0.5 * tanHalfFov / kH in
    // pre-normalisation units, and the loop below asserts a hard margin on
    // the normalised directions anyway, so a future change to kW/kH/FOV that
    // reintroduced a tie fails loudly here instead of silently comparing
    // against whichever face the GPU happened to pick.
    //
    // TOLERANCES. The two off-axis components are required to be BIT-EXACTLY
    // zero: the box's edge vectors are exactly axis-aligned, so
    // cross(v1 - v0, v2 - v0) is exactly zero on those two axes, and
    // multiplying an exact zero by any finite normalisation factor (or
    // negating it) stays zero. Only the remaining component passes through
    // normalize(), whose inversesqrt() GLSL permits to be up to 2 ULP off
    // (~2.4e-7 relative), so it is compared to +/-1 with a 1e-6 bound --
    // roughly 4x that spec limit, and six orders of magnitude tighter than
    // the distance to any other face's normal. HitT gets a relative bound of
    // 1e-4, loose enough for the ray-triangle solve and far tighter than the
    // gap between adjacent faces' distances.
    //
    // LIMITATION: this check CANNOT distinguish a face from its opposite.
    // The stored normal's sign comes entirely from wf_intersect.comp's flip
    // against the ray direction, not from which primitive was actually hit:
    // for any triangle whose cross product lies on axis k, the stored
    // normal is -sign(d_k) * e_k regardless of which triangle's index was
    // actually looked up. buildAxisAlignedBoxGeometry emits each face's two
    // triangles adjacently, axis by axis (tris 0,1 = +X; 2,3 = -X; 4,5 =
    // +Y; 6,7 = -Y; 8,9 = +Z; 10,11 = -Z), so a primitive-index bug of the
    // form `primitive ^ 1` (the OTHER triangle of the SAME face) or
    // `primitive ^ 2` (the OPPOSITE face, same axis) reads back a triangle
    // whose cross product still lies on the same axis with the same sign,
    // so it still normalize()s to the exact expected normal -- and HitT
    // (from rayQueryGetIntersectionTEXT, which never goes through the index
    // lookup at all) is unaffected by the bug in the first place. Both
    // assertions above would pass with the wrong triangle read. The
    // residual bug class this leaves uncaught is narrow -- an off-by-a-
    // different-amount indexing bug such as `primitive + 1` still fails
    // outright on 3 of the 6 faces, and a stride or scale error in the
    // index lookup fails everywhere -- but it is real, and this check does
    // not close it. It is not weakened to pretend otherwise; this is simply
    // what it does not cover.
    {
        // kH is ODD and kW EVEN on purpose -- see the tie-freedom note above.
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 49;
        constexpr uint32_t kCapacity = kW * kH;  // 3136
        static_assert(kW % 2 == 0 && kH % 2 == 1,
                      "the argmax oracle's tie-freedom argument needs kW even and kH odd");
        constexpr float kAspect = static_cast<float>(kW) / static_cast<float>(kH);
        // Wide on purpose: at the default 0.2 every ray would hit the -Z
        // face and the check would degenerate to the one normal the old
        // hardcoded constant already had.
        constexpr float kTanHalfFov = 2.0f;
        constexpr float kBoxHalfExtent = 4.0f;
        constexpr float kNormalTolerance = 1e-6f;
        constexpr float kHitTRelTolerance = 1e-4f;
        // Minimum separation required between the largest and second-largest
        // |d_k| for the argmin face to be unambiguous. The construction above
        // guarantees >= 0.5 * kTanHalfFov / kH / |d| ~ 0.006; this is an
        // order of magnitude below that and ~4 orders above float noise.
        constexpr float kFaceMargin = 1e-3f;

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: normal probe buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        // Camera at the box CENTRE, so every ray hits a face and the exit
        // distance is E / |d_k| with no origin offset term.
        ohao::diff::WavefrontGenerateCamera camera;
        camera.tanHalfFov = kTanHalfFov;
        std::vector<uint32_t> queue0;
        if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: normal probe wf_generate dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        std::vector<uint32_t> boxQueue1;
        if (!ctx.runWavefrontBoxIntersectProbe(wf, kBoxHalfExtent, boxQueue1)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: normal probe wf_intersect dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        // A ray from strictly inside a closed convex body always leaves it
        // through a face, so nothing may miss. If this trips, the readbacks
        // below would be comparing against normals for hits that never
        // happened.
        const std::uint32_t boxSurvivors =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kNextCountSlot);
        if (boxSurvivors != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: normal probe: %u of %u paths hit the closed box, "
                         "expected all of them (a ray from the interior of a convex body cannot "
                         "miss it)\n",
                         boxSurvivors, kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }

        const std::vector<float> nx =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::NormalX);
        const std::vector<float> ny =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::NormalY);
        const std::vector<float> nz =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::NormalZ);
        const std::vector<float> hitT =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::HitT);
        if (nx.size() != kCapacity || ny.size() != kCapacity || nz.size() != kCapacity ||
            hitT.size() != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: normal probe field readback size "
                                  "mismatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        // faceHits[2*k + (sign < 0)] -- six buckets, one per box face.
        uint32_t faceHits[6] = {0, 0, 0, 0, 0, 0};
        float maxNormalError = 0.0f;
        float maxHitTRelError = 0.0f;
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const uint32_t i = y * kW + x;

                // --- Analytic ray, host-side. Identical construction to
                // check 8's (and to camera_ray.glsl's), recomputed here
                // rather than read out of the Dir field so that nothing the
                // shader produced enters the oracle. ---
                const float ndcX = 2.0f * (static_cast<float>(x) + 0.5f) / kW - 1.0f;
                const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / kH;
                float d[3] = {ndcX * kAspect * kTanHalfFov, ndcY * kTanHalfFov, -1.0f};
                const float invLen =
                    1.0f / std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                d[0] *= invLen;
                d[1] *= invLen;
                d[2] *= invLen;

                // --- Analytic face: argmax |d_k|, with the tie margin
                // enforced rather than assumed. ---
                uint32_t axis = 0;
                for (uint32_t k = 1; k < 3u; ++k) {
                    if (std::fabs(d[k]) > std::fabs(d[axis])) axis = k;
                }
                float secondAbs = 0.0f;
                for (uint32_t k = 0; k < 3u; ++k) {
                    if (k != axis) secondAbs = std::max(secondAbs, std::fabs(d[k]));
                }
                if (std::fabs(d[axis]) - secondAbs < kFaceMargin) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: normal probe pixel (%u,%u): the box face "
                                 "this ray exits through is ambiguous -- |d| = (%.9g,%.9g,%.9g), "
                                 "largest exceeds runner-up by only %.9g < %.9g. The oracle's "
                                 "argmax is not well defined, so kW/kH/tanHalfFov must be chosen "
                                 "to keep every pair of |d_k| apart (see this check's "
                                 "tie-freedom note)\n",
                                 x, y, static_cast<double>(std::fabs(d[0])),
                                 static_cast<double>(std::fabs(d[1])),
                                 static_cast<double>(std::fabs(d[2])),
                                 static_cast<double>(std::fabs(d[axis]) - secondAbs),
                                 static_cast<double>(kFaceMargin));
                    wf.destroy(ctx.allocator());
                    return 1;
                }

                // Inward (== forward-facing) normal of the exit face, and
                // the exit distance, both straight from the box's algebra.
                float expected[3] = {0.0f, 0.0f, 0.0f};
                expected[axis] = (d[axis] > 0.0f) ? -1.0f : 1.0f;
                const float expectedT = kBoxHalfExtent / std::fabs(d[axis]);
                faceHits[2u * axis + ((d[axis] > 0.0f) ? 0u : 1u)] += 1u;

                const float actual[3] = {nx[i], ny[i], nz[i]};
                for (uint32_t k = 0; k < 3u; ++k) {
                    const float err = std::fabs(actual[k] - expected[k]);
                    maxNormalError = std::max(maxNormalError, err);
                    // Off-axis components: bit-exact zero (see this check's
                    // tolerance note). On-axis: within kNormalTolerance.
                    const bool bad = (k == axis) ? (err > kNormalTolerance)
                                                 : (actual[k] != 0.0f);
                    if (bad) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: normal probe pixel (%u,%u) path %u: "
                                     "stored normal = (%.9g,%.9g,%.9g), analytic box-face normal "
                                     "= (%.9g,%.9g,%.9g) (face: axis %u at %+.1f). Component %u "
                                     "differs by %.9g -- wf_intersect.comp is not writing the "
                                     "real geometric normal of the hit\n",
                                     x, y, i, static_cast<double>(actual[0]),
                                     static_cast<double>(actual[1]),
                                     static_cast<double>(actual[2]),
                                     static_cast<double>(expected[0]),
                                     static_cast<double>(expected[1]),
                                     static_cast<double>(expected[2]), axis,
                                     static_cast<double>(-expected[axis] * kBoxHalfExtent), k,
                                     static_cast<double>(err));
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                }

                const float relT = std::fabs(hitT[i] - expectedT) / expectedT;
                maxHitTRelError = std::max(maxHitTRelError, relT);
                if (relT > kHitTRelTolerance) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: normal probe pixel (%u,%u) hit distance "
                                 "%.9g, analytic box exit distance %.9g (rel err %.9g) -- the hit "
                                 "is not on the face the oracle's normal belongs to\n",
                                 x, y, static_cast<double>(hitT[i]),
                                 static_cast<double>(expectedT), static_cast<double>(relT));
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }
        }

        // Non-degeneracy: this scene reaches exactly FIVE of the box's six
        // faces, not "at least three" -- the sixth, +Z, is unreachable BY
        // CONSTRUCTION, not by bad luck on this run. The camera used above
        // is WavefrontGenerateCamera's default (only tanHalfFov is
        // overridden), whose forward is (0,0,-1); camera_ray.glsl's
        // dir = normalize(forward + right*(...) + up*(...)) only ever picks
        // up a z-component from `forward` (right and up are both z == 0
        // here), so every primary ray has d_z < 0 and none can exit through
        // +Z. faceHits[4] is +Z's bucket (2*axis + (d[axis]>0 ? 0 : 1) with
        // axis == 2, sign > 0 -- see the faceHits indexing comment above the
        // loop), so it must be exactly 0; the other five buckets (+X -X +Y
        // -Y -Z: faceHits[0,1,2,3,5]) must each be nonzero. Asserting each
        // face individually, rather than a floor on the count reached, is
        // what makes a future FOV or resolution change that collapsed
        // coverage to three faces fail here instead of passing silently --
        // and it also catches the oracle's own ray model being wrong: if
        // +Z ever came out nonzero, a sign got flipped somewhere and this
        // check's normals could no longer be trusted either.
        //
        // (-Z alone always totals exactly 600 at this kW/kH/tanHalfFov: the
        // argmax-of-|d_k| condition works out to |2x+1-kW| < kH/tanHalfFov,
        // i.e. |2x+1-64| < 24.5, giving x in [20,43] (24 columns), and
        // |kH-2y-1| < kH/tanHalfFov, i.e. |48-2y| < 24.5, giving y in
        // [12,36] (25 rows); 24*25 == 600. Not asserted as an exact count
        // here -- that arithmetic belongs in a comment, not baked into a
        // brittle assertion -- but it is why -Z's count in the OK: line
        // below is always 600.)
        static constexpr uint32_t kUnreachableFaceZPlus = 4u;
        static constexpr const char* kFaceNames[6] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
        bool faceCoverageOk = true;
        for (uint32_t f = 0; f < 6u; ++f) {
            const bool shouldBeReached = (f != kUnreachableFaceZPlus);
            const bool reached = faceHits[f] != 0u;
            if (reached != shouldBeReached) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: normal probe face %s (bucket %u) got %u hits, "
                             "expected %s -- +Z is unreachable by construction (camera forward is "
                             "(0,0,-1), so d_z < 0 for every primary ray) and the other five faces "
                             "must each be reached at least once (+X %u, -X %u, +Y %u, -Y %u, "
                             "+Z %u, -Z %u)\n",
                             kFaceNames[f], f, faceHits[f], shouldBeReached ? "nonzero" : "exactly 0",
                             faceHits[0], faceHits[1], faceHits[2], faceHits[3], faceHits[4],
                             faceHits[5]);
                faceCoverageOk = false;
            }
        }
        if (!faceCoverageOk) {
            wf.destroy(ctx.allocator());
            return 1;
        }
        uint32_t facesReached = 0;
        for (uint32_t f = 0; f < 6u; ++f) {
            if (faceHits[f] != 0u) ++facesReached;
        }

        std::printf("[diff_gpu_probe] OK: wf_intersect geometric normals match the analytic "
                    "box-face normals for all %u paths across %u distinct faces "
                    "(+X %u, -X %u, +Y %u, -Y %u, +Z %u, -Z %u; max |normal err| = %g, "
                    "max relative HitT err = %g)\n",
                    kCapacity, facesReached, faceHits[0], faceHits[1], faceHits[2], faceHits[3],
                    faceHits[4], faceHits[5], static_cast<double>(maxNormalError),
                    static_cast<double>(maxHitTRelError));

        wf.destroy(ctx.allocator());
    }

    // 20. THE BSDF ITSELF (Stage 0b-2b Task 2), term by term, against the
    // independent CPU oracle at the top of this file -- whose formulas come
    // from Walter et al. 2007, Heitz 2014, Heitz 2018, Schlick 1994 and
    // PBRT, NOT from the GLSL under test. See that section's header for what
    // is paper-derived (all of f; the physics inside pdf) and what is
    // instead the documented sampling contract (the lobe probability), and
    // for why the distinction matters to how much this check can prove.
    //
    // Three separate assertions per case, all against the oracle:
    //   (a) f(N,V,L)   -- the BSDF value at a host-chosen L.
    //   (b) pdf(N,V,L) -- the sampling density at that same L.
    //   (c) the SAMPLER's returned weight equals oracle_f(L') * (N.L') /
    //       oracle_pdf(L') at the direction L' the GPU actually sampled.
    // (c) is what ties the sampler to the evaluator: a sampler that draws
    // from one distribution and divides by another's density passes (a) and
    // (b) and fails here. The oracle recomputes f and pdf at L' itself -- it
    // never reuses the GPU's own f/pdf outputs -- so (c) cannot agree by
    // construction either.
    {
        // Tied to bsdf_probe.comp's own `pc.outIndex * <N>u` at startup by
        // checkBsdfShaderConstantTies -- not merely commented as matching it.
        constexpr uint32_t kFloatsPerCase = kBsdfProbeFloatsPerCase;

        struct MaterialSpec {
            const char* name;
            double roughness;
            double metallic;
            double specularWeight;
            double baseColor[3];
        };
        // Deliberately spans: the pure-Lambert configuration the wavefront
        // probes run with (so this check covers the exact material checks 14
        // and 17 depend on), a glossy dielectric, a sharp conductor, a rough
        // conductor, and a half-weight dielectric where the lobe mixture is
        // genuinely a mixture rather than degenerate at one end.
        const MaterialSpec kMaterials[] = {
            {"lambert", 1.00, 0.0, 0.0, {0.5, 0.5, 0.5}},
            {"dielectric-glossy", 0.35, 0.0, 1.0, {0.8, 0.6, 0.2}},
            {"conductor-sharp", 0.15, 1.0, 1.0, {0.9, 0.85, 0.5}},
            {"conductor-rough", 0.80, 1.0, 1.0, {0.3, 0.4, 0.9}},
            {"dielectric-half", 0.50, 0.0, 0.5, {0.2, 0.7, 0.3}},
        };
        // Two normals: the axis-aligned one every earlier probe sees, and a
        // tilted one, so a BSDF that silently assumed N = +Z (as the Stage
        // 0b-1 scatter placeholder effectively did) cannot pass.
        const OracleVec3 kNormals[] = {{0.0, 0.0, 1.0},
                                       oracleNormalize(OracleVec3{0.3, -0.5, 0.81})};
        const double kViewThetas[] = {0.20, 0.70, 1.20};
        const double kLightAngles[][2] = {{0.30, 0.0}, {0.90, 2.0}, {1.30, 4.5}};
        // The view's AZIMUTH about N, cycled independently of everything
        // else. An isotropic BSDF must be invariant to it, and until this was
        // a list the whole table shared one value (0.6) -- so nothing tested
        // that invariance, and a tangent-frame-dependent bug that only
        // showed up at some azimuths could not be seen. 5 is coprime with the
        // 7 sample triples below, so the pairing does not lock into a short
        // cycle over the 90 cases.
        const double kViewPhis[] = {0.6, 1.9, 3.3, 4.7, 5.9};
        // Sample values cycled through the cases so both lobes get chosen
        // somewhere in the table (uLobe below/above the specular probability)
        // and the VNDF sampler is exercised across its unit square. Seven
        // triples, not three: with three, 90 cases carried only three
        // distinct (u1, u2, uLobe) points, so the sampler was being asked the
        // same question thirty times over. The first three are the original
        // ones, kept so the coverage this table already had is not traded
        // away for the new coverage.
        const float kSamples[][3] = {{0.13f, 0.77f, 0.05f},
                                     {0.61f, 0.24f, 0.45f},
                                     {0.89f, 0.52f, 0.95f},
                                     {0.03f, 0.41f, 0.28f},
                                     {0.37f, 0.95f, 0.68f},
                                     {0.72f, 0.09f, 0.11f},
                                     {0.96f, 0.63f, 0.82f}};
        constexpr uint32_t kSampleCount = sizeof(kSamples) / sizeof(kSamples[0]);
        constexpr uint32_t kViewPhiCount = sizeof(kViewPhis) / sizeof(kViewPhis[0]);

        constexpr uint32_t kMaterialCount = sizeof(kMaterials) / sizeof(kMaterials[0]);
        constexpr uint32_t kNormalCount = 2;
        constexpr uint32_t kViewCount = 3;
        constexpr uint32_t kLightCount = 3;
        constexpr uint32_t kCaseCount = kMaterialCount * kNormalCount * kViewCount * kLightCount;

        ohao::diff::ArenaLayout bsdfLayout;
        const std::size_t bsdfBlock = bsdfLayout.add(kCaseCount * kFloatsPerCase);
        ohao::diff::GradientArena bsdfArena;
        if (!bsdfArena.build(ctx.allocator(), bsdfLayout)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: bsdf probe arena build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { bsdfArena.zero(cmd); });

        // Host-side record of each case, so the oracle can be evaluated
        // after every dispatch has landed.
        struct CaseRecord {
            const char* materialName;
            OracleVec3 N;
            OracleVec3 V;
            OracleVec3 L;
            OracleMaterial material;
            double u1;
            double u2;
            double uLobe;
        };
        std::vector<CaseRecord> records;
        records.reserve(kCaseCount);

        bool bsdfDispatchOk = true;
        uint32_t caseIndex = 0;
        for (uint32_t mi = 0; mi < kMaterialCount && bsdfDispatchOk; ++mi) {
            for (uint32_t ni = 0; ni < kNormalCount && bsdfDispatchOk; ++ni) {
                for (uint32_t vi = 0; vi < kViewCount && bsdfDispatchOk; ++vi) {
                    for (uint32_t li = 0; li < kLightCount && bsdfDispatchOk; ++li) {
                        const MaterialSpec& ms = kMaterials[mi];
                        const OracleVec3 N = kNormals[ni];
                        const double viewPhi = kViewPhis[caseIndex % kViewPhiCount];
                        const OracleVec3 V = oracleDirFromAngles(N, kViewThetas[vi], viewPhi);
                        const OracleVec3 L =
                            oracleDirFromAngles(N, kLightAngles[li][0], kLightAngles[li][1]);

                        OracleMaterial mat;
                        mat.baseColor = {ms.baseColor[0], ms.baseColor[1], ms.baseColor[2]};
                        mat.roughness = ms.roughness;
                        mat.metallic = ms.metallic;
                        mat.specularWeight = ms.specularWeight;

                        const float* smp = kSamples[caseIndex % kSampleCount];

                        ohao::diff::BsdfProbeCase probeCase;
                        probeCase.normal[0] = static_cast<float>(N.x);
                        probeCase.normal[1] = static_cast<float>(N.y);
                        probeCase.normal[2] = static_cast<float>(N.z);
                        probeCase.roughness = static_cast<float>(ms.roughness);
                        probeCase.view[0] = static_cast<float>(V.x);
                        probeCase.view[1] = static_cast<float>(V.y);
                        probeCase.view[2] = static_cast<float>(V.z);
                        probeCase.metallic = static_cast<float>(ms.metallic);
                        probeCase.light[0] = static_cast<float>(L.x);
                        probeCase.light[1] = static_cast<float>(L.y);
                        probeCase.light[2] = static_cast<float>(L.z);
                        probeCase.specularWeight = static_cast<float>(ms.specularWeight);
                        probeCase.baseColor[0] = static_cast<float>(ms.baseColor[0]);
                        probeCase.baseColor[1] = static_cast<float>(ms.baseColor[1]);
                        probeCase.baseColor[2] = static_cast<float>(ms.baseColor[2]);
                        probeCase.u1 = smp[0];
                        probeCase.u2 = smp[1];
                        probeCase.uLobe = smp[2];
                        probeCase.outIndex = caseIndex;

                        if (!ctx.runBsdfProbe(bsdfArena, probeCase)) {
                            std::fprintf(stderr,
                                         "[diff_gpu_probe] FAIL: bsdf probe dispatch for case %u\n",
                                         caseIndex);
                            bsdfDispatchOk = false;
                            break;
                        }
                        records.push_back(CaseRecord{ms.name, N, V, L, mat, smp[0], smp[1],
                                                     smp[2]});
                        ++caseIndex;
                    }
                }
            }
        }
        if (!bsdfDispatchOk) {
            bsdfArena.destroy(ctx.allocator());
            return 1;
        }

        const std::vector<float> bsdfOut = bsdfArena.readback(ctx.allocator(), bsdfBlock);
        if (bsdfOut.size() < static_cast<std::size_t>(kCaseCount) * kFloatsPerCase) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: bsdf probe readback returned %zu floats, expected "
                         "at least %u\n",
                         bsdfOut.size(), kCaseCount * kFloatsPerCase);
            bsdfArena.destroy(ctx.allocator());
            return 1;
        }

        // TOLERANCES, and why they are not one number. The GPU works in
        // float32 and the oracle in double, so some slack is unavoidable;
        // how much depends on the CONDITIONING of the quantity, which is
        // very different for the value and for the density.
        //
        //   f and the sampler weight tolerate 1e-4. Both are smooth in their
        //   inputs at every case in the table, and the measured worst case
        //   over the whole table is ~6e-6 -- a factor of 16 of headroom.
        //
        //   pdf tolerates 1e-3. At the sharpest material here (roughness
        //   0.15, alpha^2 ~ 5e-4) the GGX lobe's angular width is comparable
        //   to alpha, so d(ln D)/d(N.H) ~ 4/alpha^2 ~ 8e3: a float32
        //   direction carrying ~1e-7 of relative error comes back with
        //   ~8e-4 of relative error in D, and therefore in the density.
        //   That is arithmetic conditioning, not a modelling difference, and
        //   the measured worst case (~3.9e-4) sits where the estimate
        //   predicts.
        //
        // Neither number was tuned until it passed. Both are printed with
        // the observed maxima on the OK: line, so a tolerance that has
        // quietly started to absorb a real error is visible rather than
        // silent.
        //
        // For scale, the smallest modelling error this is meant to catch --
        // swapping Schlick's exponent 5 for 4 -- was measured by making that
        // edit: the run aborts at case 20 (dielectric-glossy) with a
        // deviation of 4.8e-4, i.e. 4.8x the f tolerance and ~140x the
        // observed float32 noise floor of ~3.4e-6. It is NOT true that every
        // case moves by that much: cases 18 and 19 are the same material and
        // moved by LESS than the tolerance, so they did not register at all.
        // What catches this class of error is the BREADTH of the table -- the
        // spread of view, light and normal geometry means some case is
        // sensitive -- not the sensitivity of any individual case. Shrinking
        // the table would weaken the check even with the tolerance untouched.
        constexpr double kBsdfValueRelTol = 1e-4;
        constexpr double kBsdfPdfRelTol = 1e-3;

        double maxFErr = 0.0;
        double maxPdfErr = 0.0;
        double maxWeightErr = 0.0;
        uint32_t specularSampledCount = 0;
        uint32_t diffuseSampledCount = 0;
        uint32_t rejectedSampleCount = 0;
        uint32_t grazingRejectionCount = 0;
        uint32_t branchAssertedCount = 0;
        // How close uLobe may sit to q before the branch-agreement assertion
        // below stands down. At |uLobe - q| under this the GPU's float32 q
        // and the oracle's double q can legitimately land on opposite sides
        // of the comparison, and asserting there would be asserting float
        // rounding. The count of cases actually asserted is printed, so a
        // margin that quietly disabled the assertion for the whole table
        // would be visible.
        constexpr double kLobeDecisionMargin = 1e-3;

        for (uint32_t i = 0; i < kCaseCount; ++i) {
            const CaseRecord& rec = records[i];
            const float* out = &bsdfOut[static_cast<std::size_t>(i) * kFloatsPerCase];

            OracleVec3 refF;
            double refPdf = 0.0;
            oracleBsdfEval(rec.N, rec.V, rec.L, rec.material, refF, refPdf);

            const double gpuF[3] = {out[0], out[1], out[2]};
            const double refFv[3] = {refF.x, refF.y, refF.z};
            for (int c = 0; c < 3; ++c) {
                const double e = oracleRelDiff(refFv[c], gpuF[c]);
                if (e > maxFErr) maxFErr = e;
                if (e > kBsdfValueRelTol) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: BSDF f mismatch, case %u (%s), channel "
                                 "%d: GPU %.9g, CPU oracle %.9g (relative %.3g > %.3g). N = "
                                 "(%.6f,%.6f,%.6f), V = (%.6f,%.6f,%.6f), L = (%.6f,%.6f,%.6f), "
                                 "roughness %.3f, metallic %.3f, specularWeight %.3f\n",
                                 i, rec.materialName, c, gpuF[c], refFv[c], e, kBsdfValueRelTol,
                                 rec.N.x, rec.N.y, rec.N.z, rec.V.x, rec.V.y, rec.V.z, rec.L.x,
                                 rec.L.y, rec.L.z, rec.material.roughness, rec.material.metallic,
                                 rec.material.specularWeight);
                    bsdfArena.destroy(ctx.allocator());
                    return 1;
                }
            }

            const double gpuPdf = out[3];
            const double pdfErr = oracleRelDiff(refPdf, gpuPdf);
            if (pdfErr > maxPdfErr) maxPdfErr = pdfErr;
            if (pdfErr > kBsdfPdfRelTol) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: BSDF pdf mismatch, case %u (%s): GPU %.9g, "
                             "CPU oracle %.9g (relative %.3g > %.3g). roughness %.3f, metallic "
                             "%.3f, specularWeight %.3f\n",
                             i, rec.materialName, gpuPdf, refPdf, pdfErr, kBsdfPdfRelTol,
                             rec.material.roughness, rec.material.metallic,
                             rec.material.specularWeight);
                bsdfArena.destroy(ctx.allocator());
                return 1;
            }

            // (c) The sampler's own weight, at the direction the GPU drew.
            // Slots 4..6 hold that direction EXACTLY as drawn -- including
            // the GGX VNDF's below-horizon tail -- because diffBsdfSample
            // deliberately does not substitute a usable direction for a
            // rejected sample. That is what lets the oracle confirm a zero
            // weight was legitimate instead of having to take it on trust.
            const OracleVec3 sampledL = oracleNormalize({out[4], out[5], out[6]});
            const double gpuWeight[3] = {out[7], out[8], out[9]};
            const double gpuSampPdf = out[10];
            OracleVec3 sampF;
            double sampPdf = 0.0;
            oracleBsdfEval(rec.N, rec.V, sampledL, rec.material, sampF, sampPdf);
            const double sampNdotL = oracleDot(rec.N, sampledL);

            // WHICH LOBE THE GPU ACTUALLY SAMPLED, decided from the returned
            // direction rather than predicted from the material table. The
            // diffuse branch draws diffCosineHemisphere(N, uDir); the
            // specular branch draws a VNDF half-vector and reflects. So if
            // the returned direction IS the cosine-hemisphere direction for
            // this case's uDir, the diffuse branch ran. (The two agreeing by
            // accident is a measure-zero coincidence, and the tolerance here
            // is float32 noise, not a window.) This previously classified by
            // oracleSpecProb(...) > 0.5, which reads only the hardcoded
            // material table and the hardcoded normals: no GPU output entered
            // it, so it was deterministic and could not fail while its FAIL
            // message claimed to describe which branch the sampler took.
            const OracleVec3 cosineL = oracleCosineHemisphere(rec.N, rec.u1, rec.u2);
            const bool tookDiffuseBranch = oracleDistance(sampledL, cosineL) < 1e-4;
            if (tookDiffuseBranch) {
                ++diffuseSampledCount;
            } else {
                ++specularSampledCount;
            }

            // And the branch it took must be the branch the documented
            // strategy asks for: uLobe < q, with q the lobe probability this
            // oracle computes independently. This is the only place the
            // lobe-selection rule itself is asserted per-case -- see the
            // oracle header's note on where q is guarded.
            const double caseQ = oracleSpecProb(rec.material, oracleDot(rec.N, rec.V));
            const bool expectSpecularBranch =
                (rec.uLobe < caseQ) && (oracleDot(rec.N, rec.V) > kShaderGrazingCos);
            if (std::abs(rec.uLobe - caseQ) > kLobeDecisionMargin) {
                ++branchAssertedCount;
                if (expectSpecularBranch == tookDiffuseBranch) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: BSDF sampler case %u (%s) took the %s "
                                 "branch, but the contract's lobe probability q = %.9g with "
                                 "uLobe = %.9g asks for the %s branch. The branch actually taken "
                                 "was read off the returned direction L = (%.6f,%.6f,%.6f)\n",
                                 i, rec.materialName, tookDiffuseBranch ? "diffuse" : "specular",
                                 caseQ, rec.uLobe, expectSpecularBranch ? "specular" : "diffuse",
                                 sampledL.x, sampledL.y, sampledL.z);
                    bsdfArena.destroy(ctx.allocator());
                    return 1;
                }
            }

            if (gpuSampPdf <= 0.0) {
                // The GPU says it rejected this sample. Two things have to
                // hold, or the rejection is a bug rather than a tail: the
                // ORACLE must independently agree the direction carries no
                // energy, and the weight must be exactly zero. A sampler
                // that rejected everything would fail the first of those on
                // its very first accepted-looking case.
                //
                // One documented exception, and it is a real threshold
                // difference rather than slack: bsdf.glsl refuses the
                // specular math at N.L <= DIFF_BSDF_MIN_COS (1e-4) while this
                // oracle refuses it at N.L <= 0, because 1e-4 is an
                // implementation guard against dividing by (N.L) twice and
                // has no place in the physics. A sample landing in the band
                // (0, 1e-4] is therefore rejected by the shader and accepted
                // by the oracle, legitimately. Such cases are counted and
                // reported rather than being allowed to fail the run; every
                // case in the current table sits orders of magnitude above
                // the band, so the count is expected to be 0 and a nonzero
                // one is worth looking at.
                if (sampNdotL > 0.0 && sampNdotL <= kShaderGrazingCos) {
                    ++grazingRejectionCount;
                } else if (sampNdotL > 0.0 && sampPdf > 0.0) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: BSDF sampler case %u (%s) rejected its "
                                 "own sample (pdf 0) at L = (%.6f,%.6f,%.6f), but the oracle says "
                                 "that direction is perfectly valid (N.L = %.9g, pdf = %.9g) and "
                                 "is not inside the shader's documented grazing band "
                                 "(0, %.0e]\n",
                                 i, rec.materialName, sampledL.x, sampledL.y, sampledL.z,
                                 sampNdotL, sampPdf, kShaderGrazingCos);
                    bsdfArena.destroy(ctx.allocator());
                    return 1;
                }
                for (int c = 0; c < 3; ++c) {
                    if (gpuWeight[c] != 0.0) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: BSDF sampler case %u (%s) reported a "
                                     "zero density but a non-zero weight %.9g -- a zero-BRDF "
                                     "sample must carry a zero weight\n",
                                     i, rec.materialName, gpuWeight[c]);
                        bsdfArena.destroy(ctx.allocator());
                        return 1;
                    }
                }
                ++rejectedSampleCount;
                continue;
            }

            if (sampPdf <= 0.0 || sampNdotL <= 0.0) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: BSDF sampler case %u (%s) accepted a sample "
                             "(pdf %.9g) at L = (%.6f,%.6f,%.6f) that the oracle says carries no "
                             "energy (N.L = %.9g)\n",
                             i, rec.materialName, gpuSampPdf, sampledL.x, sampledL.y, sampledL.z,
                             sampNdotL);
                bsdfArena.destroy(ctx.allocator());
                return 1;
            }

            // The density the sampler says it drew with must be the density
            // the evaluator assigns to that direction. This is what a
            // "samples one distribution, divides by another" bug shows up
            // as, and it is checked against the ORACLE's density, not
            // against the GPU's own eval output.
            const double sampPdfErr = oracleRelDiff(sampPdf, gpuSampPdf);
            if (sampPdfErr > maxPdfErr) maxPdfErr = sampPdfErr;
            if (sampPdfErr > kBsdfPdfRelTol) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: BSDF sampler case %u (%s) reported density "
                             "%.9g at its own sampled direction, CPU oracle %.9g (relative %.3g "
                             "> %.3g)\n",
                             i, rec.materialName, gpuSampPdf, sampPdf, sampPdfErr,
                             kBsdfPdfRelTol);
                bsdfArena.destroy(ctx.allocator());
                return 1;
            }

            const double refWeight[3] = {sampF.x * sampNdotL / sampPdf,
                                         sampF.y * sampNdotL / sampPdf,
                                         sampF.z * sampNdotL / sampPdf};
            for (int c = 0; c < 3; ++c) {
                const double e = oracleRelDiff(refWeight[c], gpuWeight[c]);
                if (e > maxWeightErr) maxWeightErr = e;
                if (e > kBsdfValueRelTol) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: BSDF sampler weight mismatch, case %u "
                                 "(%s), channel %d: GPU %.9g, CPU oracle f*cos/pdf %.9g "
                                 "(relative %.3g > %.3g). Sampled L = (%.6f,%.6f,%.6f), "
                                 "oracle pdf there %.9g -- the sampler and the evaluator "
                                 "disagree about which density the direction was drawn from\n",
                                 i, rec.materialName, c, gpuWeight[c], refWeight[c], e,
                                 kBsdfValueRelTol, sampledL.x, sampledL.y, sampledL.z, sampPdf);
                    bsdfArena.destroy(ctx.allocator());
                    return 1;
                }
            }
        }

        if (specularSampledCount == 0 || diffuseSampledCount == 0) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: over %u cases the sampler took only one of its "
                         "two branches (%u specular, %u diffuse, counted from the direction the "
                         "GPU returned) -- the table no longer covers the mixture it claims to\n",
                         kCaseCount, specularSampledCount, diffuseSampledCount);
            bsdfArena.destroy(ctx.allocator());
            return 1;
        }
        if (branchAssertedCount * 4u < kCaseCount) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: the lobe-branch agreement assertion stood down "
                         "on all but %u of %u cases (uLobe within %.0e of q) -- it is no longer "
                         "asserting the lobe-selection rule over a meaningful part of the "
                         "table\n",
                         branchAssertedCount, kCaseCount, kLobeDecisionMargin);
            bsdfArena.destroy(ctx.allocator());
            return 1;
        }

        // Non-vacuity guard on assertion (c): every rejected sample skips the
        // weight comparison, so a sampler that rejected most of the table
        // could pass (c) having compared almost nothing. The rejected cases
        // are still individually verified against the oracle above -- this
        // bounds how much of the table that verification is allowed to be.
        if (rejectedSampleCount * 4u >= kCaseCount) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: BSDF sampler rejected %u of %u cases -- the "
                         "weight comparison is no longer exercising most of the table\n",
                         rejectedSampleCount, kCaseCount);
            bsdfArena.destroy(ctx.allocator());
            return 1;
        }

        std::printf("[diff_gpu_probe] OK: BSDF f, pdf and sampler weight match an independent "
                    "CPU oracle (Walter 2007 / Heitz 2014 / Heitz 2018 / Schlick 1994) over %u "
                    "cases x %u materials x %u sample triples x %u view azimuths; the GPU took "
                    "the specular branch %u times and the diffuse branch %u times (measured from "
                    "the returned direction), and the branch matched the contract's uLobe < q on "
                    "all %u cases where the assertion applied; %u below-horizon rejections, %u "
                    "inside the shader's grazing band; max relative error f %.3g, weight %.3g "
                    "(tolerance %.3g), pdf %.3g (tolerance %.3g)\n",
                    kCaseCount, kMaterialCount, kSampleCount, kViewPhiCount,
                    specularSampledCount, diffuseSampledCount, branchAssertedCount,
                    rejectedSampleCount, grazingRejectionCount, maxFErr, maxWeightErr,
                    kBsdfValueRelTol, maxPdfErr, kBsdfPdfRelTol);

        bsdfArena.destroy(ctx.allocator());
    }

    // 21-23. THE FURNACE TEST, on its own deliberately trivial scene.
    //
    // Check 20 compares the BSDF term by term. It cannot catch an error in
    // how the terms are COMBINED into a path -- a missing cosine, a pdf
    // divided in the wrong place, a lobe probability that does not match the
    // branch it gates -- because every one of those is a property of the
    // whole sample-evaluate-weight loop rather than of any single term. The
    // furnace test is the global counterpart: it runs the real
    // wf_scatter.comp dispatch and asks whether energy is conserved.
    //
    // SCENE. Its own, not Task 1's box. Task 1's scene is geometry-bearing
    // and carries a provable four-bounce survival bound; a furnace scene is
    // supposed to be trivial, and making one scene serve both would force a
    // constraint on Task 1's that it was never designed for. This one is the
    // single full quad every intersect check already uses: generate a path
    // per pixel, trace it once so every path has a real hit point and a real
    // geometric normal, then run exactly ONE scatter dispatch. Throughput
    // starts at 1 (wf_generate.comp writes it), so after that dispatch the
    // Throughput field IS the BSDF estimator weight, path by path.
    //
    // ENVIRONMENT. Constant radiance L0 = 1 in every direction, evaluated
    // ANALYTICALLY here on the host -- no CDF, no importance sampling, no
    // env_sampling.glsl. Environment importance sampling is Task 3; a
    // furnace needs nothing more than a constant. Because L0 is constant,
    // the radiance an escaping path would deposit is throughput * L0 =
    // throughput, so reading Throughput back IS reading the furnace estimate
    // and no radiance-accumulation stage is needed to run this.
    //
    // ------------------------------------------------------------------
    // 21. WHITE FURNACE, and its error bound, derived
    // ------------------------------------------------------------------
    //
    // Material: base colour rho = 1, no absorption, specular lobe scaled out
    // (specularWeight = 0, metallic = 0), so f = rho/pi on the hemisphere.
    //
    // The quantity being estimated is the outgoing radiance
    //     Lo = integral over the hemisphere of f * L0 * cos(theta) dw
    //        = L0 * rho * integral of cos(theta)/pi dw
    //        = L0 * rho = 1.
    //
    // The estimator is one cosine-weighted sample, w ~ p(w) = cos(theta)/pi:
    //     X = f(w) * L0 * cos(theta) / p(w)
    //       = (rho/pi) * L0 * cos(theta) * pi / cos(theta)
    //       = rho * L0 = 1,   for EVERY w.
    //
    // X is therefore a CONSTANT random variable. Var[X] = 0, so the Monte
    // Carlo standard error sigma/sqrt(N) is exactly 0 at every sample count
    // -- there is no noise term to bound. This is not a weakness of the
    // test: perfect importance sampling of a constant integrand is precisely
    // what a white furnace is, and it means the check can be made TIGHT
    // rather than statistical.
    //
    // What remains is float32 rounding. The shader takes the analytic
    // cancellation (see diffBsdfSample's pure-Lambert branch), so the weight
    // is the single product baseColor * (1 - metallic) = 1.0 * 1.0, with no
    // division and no transcendental: exactly representable, exactly 1.
    // Allowing for a compiler that contracts that product differently, the
    // bound asserted is 4 units in the last place of 1.0f,
    //     4 * 2^-23 = 4.768e-7,
    // and the OBSERVED maximum deviation is printed on the OK: line, so a
    // bound that has quietly started absorbing a real error is visible
    // rather than silent. Nothing about this tolerance was tuned until it
    // passed: it was derived first and the observed value came in at 0.
    //
    // ------------------------------------------------------------------
    // 22. GLOSSY ENERGY BOUND -- why it is NOT also 1.0
    // ------------------------------------------------------------------
    //
    // Running the same furnace with a white ROUGH CONDUCTOR (base colour 1,
    // metallic 1) must NOT be asserted to give 1.0. This BSDF models
    // single-scattering GGX only: light that would have bounced a second
    // time between microfacets is dropped, and the resulting energy deficit
    // is a well-known, published property of the model (Heitz et al.,
    // "Multiple-Scattering Microfacet BSDFs with the Smith Model",
    // SIGGRAPH 2016), not a bug in this implementation. Asserting 1.0 here
    // would be asserting something false.
    //
    // What IS provable pointwise: with base colour 1 and metallic 1,
    // F0 = 1, so Schlick gives F = 1 + (1-1)(...) = 1 identically, and the
    // lobe probability q = mix(..., 1.0, metallic) = 1 exactly, so the
    // mixture density collapses to the pure VNDF density and the diffuse
    // lobe vanishes. The weight is then
    //     f*cos/pdf = [D G2 / (4 (N.V)(N.L))] (N.L) / [G1(V) D / (4 (N.V))]
    //               = G2(V,L) / G1(V),
    // and height-correlated Smith gives
    //     G2/G1 = (1 + Lambda(V)) / (1 + Lambda(V) + Lambda(L)) <= 1,
    // with equality only when Lambda(L) = 0, i.e. L exactly along N
    // (Heitz 2014 Eq. 43 and 99). So EVERY path's weight is in [0, 1], and
    // the mean is strictly below 1. Both halves are asserted: an upper bound
    // no sample may exceed (energy gain), and a mean strictly under it
    // (the deficit is really there rather than having been papered over).
    //
    // ------------------------------------------------------------------
    // 23. INTERMEDIATE q -- the run that actually exercises the mixture
    // ------------------------------------------------------------------
    //
    // WHY A THIRD RUN. Checks 21 and 22 sit at the two lobe probabilities
    // that cannot bias anything: 21 is {roughness 1, metallic 0,
    // specularWeight 0}, i.e. q = 0 exactly, and 22 is a conductor, i.e.
    // q = 1 exactly. Worse, at q = 0 diffBsdfSample takes an early-return
    // branch that never calls diffBsdfEval, never forms f, never forms pdf
    // and never divides -- so check 21's derivation above describes
    // arithmetic the GPU does not execute. It verifies that one
    // multiplication returns baseColor. That is worth having, and it is not
    // a statement about the mixture density or about f*cos/pdf.
    //
    // MATERIAL. baseColor 1, roughness 0.30, metallic 0.5, specularWeight 1.
    // A pure dielectric cannot reach an intermediate q in this model: its
    // F0 is 0.04, so q = specularWeight * F_max(|N.V|) * (1 - 0.9*roughness)
    // is capped near 0.04 at the near-normal incidence this scene provides.
    // Raising metallic instead is what moves q into the middle -- here to
    // about 0.69 -- while keeping BOTH lobes materially present in f
    // (kd = 1 - metallic = 0.5). Every path now goes through diffBsdfEval,
    // forms the full mixture density, and divides. (DESIGN CALL: the review
    // asked for "a dielectric with specularWeight > 0"; no dielectric in
    // this parameterisation has an intermediate q, so a half-metal is used
    // instead and the reason is recorded here.)
    //
    // WHAT IS ASSERTED, and how each bound is derived.
    //
    // (a) A POINTWISE upper bound on the weight. Unlike check 22 the weight
    //     is not G2/G1 and is not bounded by 1 -- it is a mixture estimator,
    //     and a mixture estimator's weight is not bounded by the material's
    //     albedo. It IS bounded, though, and the bound is elementary. With
    //     f = f_d + f_s and pdf = (1-q) p_d + q p_s, dropping one term from
    //     each denominator gives
    //         f cos / pdf <= f_d cos / ((1-q) p_d)  +  f_s cos / (q p_s)
    //                      = rho(1-metallic)/(1-q)  +  F (G2/G1) / q
    //                     <= rho(1-metallic)/(1-q)  +  specScale / q,
    //     using F <= 1 and G2/G1 <= 1 (Heitz 2014 Eq. 99), and p_d, p_s
    //     being the cosine and VNDF densities the two branches draw from.
    //     q varies across the frame only through |N.V|, which this narrow
    //     frustum confines to a small interval, so the bound is evaluated at
    //     the worst q in that interval and is a constant. A weight above it
    //     is energy created out of nothing.
    //
    // (b) The MEAN, against a numerically integrated reference. The
    //     estimator is unbiased for the directional albedo whatever the
    //     sampling strategy is,
    //         E[f cos / pdf] = INT f(N,V,L) (N.L) dL = rho_dir(V),
    //     and rho_dir is computable here: oracleDirectionalAlbedo integrates
    //     THIS FILE'S paper-derived f by midpoint quadrature in double
    //     precision. There is no closed form for it -- that is the point of
    //     Heitz 2016 -- but there does not need to be one.
    //
    //     An earlier version of this check bracketed the mean instead, using
    //     F0 <= F <= 1 with check 22's measurement of the single-scattering
    //     GGX albedo. That bracket is correct but useless in one direction:
    //     Schlick's grazing term is (1 - V.H)^5, and at this geometry -- a
    //     narrow frustum onto a facing quad, alpha = 0.09 -- V.H sits within
    //     about 0.02 of 1 across the whole lobe, so F is F0 to four decimal
    //     places and the F <= 1 end is slack by a third of the answer. A q
    //     perturbation that biased the mean upward by 14% was measured
    //     passing it. The quadrature reference is two-sided and tight.
    //
    //     rho_dir depends on the view angle, and each path has its own,
    //     so the reference is evaluated across the frame's whole |N.V| range
    //     and the min and max are taken. Two error terms are added to that
    //     interval, both MEASURED rather than chosen: the quadrature's own
    //     discretisation error, estimated by redoing one evaluation at half
    //     resolution and taking the difference, and 5 standard errors of the
    //     GPU sample mean, computed from the 3072 samples themselves. Both
    //     are printed, as is the distance from the reference in units of the
    //     standard error, so a run drifting toward the bound is visible long
    //     before it crosses one.
    //
    // WHAT THIS DOES AND DOES NOT GUARD ABOUT q. A consistent change to q --
    // one that moves the branch probability and the mixture weight together
    // -- leaves the estimator unbiased, and this check will correctly not
    // fire: any q in (0,1) that is nonzero wherever f is nonzero is a legal
    // strategy. What it catches is q's coverage failing (a lobe stops being
    // reachable while f still has energy there) and the branch and the
    // density disagreeing about q, which is the bug this arrangement is
    // actually exposed to.
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 48;
        constexpr uint32_t kCapacity = kW * kH;  // 3072
        constexpr float kPlaneDistance = 2.0f;
        constexpr float kTanHalfFov = 0.2f;
        constexpr uint32_t kIterationSeed = 424242u;
        // Derived above: 4 units in the last place of 1.0f. Not tuned.
        constexpr float kFurnaceUlpBound = 4.0f * 1.1920929e-7f;

        struct FurnaceRun {
            const char* name;
            ohao::diff::WavefrontScatterMaterial material;
        };
        const FurnaceRun kRuns[] = {
            {"lambert", ohao::diff::WavefrontScatterMaterial{1.0f, 0.0f, 0.0f}},
            {"white rough conductor", ohao::diff::WavefrontScatterMaterial{0.30f, 1.0f, 1.0f}},
            {"half-metal, intermediate q",
             ohao::diff::WavefrontScatterMaterial{0.30f, 0.5f, 1.0f}},
        };
        constexpr uint32_t kFurnaceRunCount = sizeof(kRuns) / sizeof(kRuns[0]);
        constexpr uint32_t kMixtureRun = 2;

        double furnaceMean[kFurnaceRunCount] = {0.0, 0.0, 0.0};
        double furnaceMax[kFurnaceRunCount] = {0.0, 0.0, 0.0};
        double furnaceMin[kFurnaceRunCount] = {0.0, 0.0, 0.0};
        double furnaceStdErr[kFurnaceRunCount] = {0.0, 0.0, 0.0};
        double lambertMaxDeviation = 0.0;

        // The mixture run's material, restated for the host so the bounds
        // below are derived from the contract rather than from constants
        // typed twice. oracleSpecProb / oracleSpecScale / oracleF0 are the
        // same independent implementations check 20 uses.
        OracleMaterial mixMat;
        mixMat.baseColor = {1.0, 1.0, 1.0};  // furnace albedo is 1
        mixMat.roughness = kRuns[kMixtureRun].material.roughness;
        mixMat.metallic = kRuns[kMixtureRun].material.metallic;
        mixMat.specularWeight = kRuns[kMixtureRun].material.specularWeight;

        // |N.V| over the frame. The quad faces the camera, so N.V for the
        // pixel at (x,y) is 1/sqrt(1 + dx^2 + dy^2) with the same dx, dy the
        // closed-form ray in check 3 uses; it is 1 dead centre and smallest
        // in a corner.
        constexpr double kFurnaceAspect = static_cast<double>(kW) / static_cast<double>(kH);
        const double dxMax = (1.0 - 1.0 / kW) * kFurnaceAspect * kTanHalfFov;
        const double dyMax = (1.0 - 1.0 / kH) * kTanHalfFov;
        const double cosMin = 1.0 / std::sqrt(1.0 + dxMax * dxMax + dyMax * dyMax);
        const double qAtNormal = oracleSpecProb(mixMat, 1.0);
        const double qAtCorner = oracleSpecProb(mixMat, cosMin);
        const double qMin = std::min(qAtNormal, qAtCorner);
        const double qMax = std::max(qAtNormal, qAtCorner);
        const double mixDiffuseAlbedo = mixMat.baseColor.x * (1.0 - mixMat.metallic);
        const double mixSpecScale = oracleSpecScale(mixMat);
        // (a), derived above: rho(1-metallic)/(1-q) + specScale/q, at the
        // worst q in the frame's interval (the first term grows with q, the
        // second shrinks, so the extremes are taken independently).
        const double mixPointwiseBound = mixDiffuseAlbedo / (1.0 - qMax) + mixSpecScale / qMin;

        for (uint32_t run = 0; run < kFurnaceRunCount; ++run) {
            ohao::diff::WavefrontBuffers wf;
            if (!wf.build(ctx.allocator(), kCapacity)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: furnace buffers build (%s)\n",
                             kRuns[run].name);
                return 1;
            }
            ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

            ohao::diff::WavefrontGenerateCamera camera;
            camera.tanHalfFov = kTanHalfFov;
            std::vector<uint32_t> queue0;
            if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: furnace setup: wf_generate (%s)\n",
                             kRuns[run].name);
                wf.destroy(ctx.allocator());
                return 1;
            }
            std::vector<uint32_t> queue1;
            if (!ctx.runWavefrontIntersectProbe(wf, kPlaneDistance, /*quadMinY=*/-1.0f, queue1)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: furnace setup: wf_intersect (%s)\n",
                             kRuns[run].name);
                wf.destroy(ctx.allocator());
                return 1;
            }
            const std::uint32_t seeded =
                wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kNextCountSlot);
            if (seeded != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: furnace setup (%s): %u of %u rays hit the "
                             "full quad, expected all of them -- the furnace estimate would be "
                             "averaged over paths that never scattered\n",
                             kRuns[run].name, seeded, kCapacity);
                wf.destroy(ctx.allocator());
                return 1;
            }

            std::vector<uint32_t> outQueue;
            std::vector<float> outDraws;
            if (!ctx.runWavefrontScatterProbe(
                    wf, /*srcQueueBase=*/kCapacity,
                    ohao::diff::WavefrontBuffers::kNextCountSlot, /*dstQueueBase=*/0u,
                    ohao::diff::WavefrontBuffers::kCurrentCountSlot, /*albedo=*/1.0f,
                    kIterationSeed, outQueue, outDraws, kRuns[run].material)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: furnace scatter dispatch (%s)\n",
                             kRuns[run].name);
                wf.destroy(ctx.allocator());
                return 1;
            }

            const std::vector<float> tR =
                wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputR);
            const std::vector<float> tG =
                wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputG);
            const std::vector<float> tB =
                wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputB);
            if (tR.size() != kCapacity || tG.size() != kCapacity || tB.size() != kCapacity) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: furnace throughput readback size "
                                      "mismatch (%s)\n",
                             kRuns[run].name);
                wf.destroy(ctx.allocator());
                return 1;
            }

            double sum = 0.0;
            double sumSq = 0.0;
            double maxV = -1.0;
            double minV = 2.0;
            for (uint32_t i = 0; i < kCapacity; ++i) {
                // Grey material and grey environment: the three channels must
                // agree exactly, and a check that looked at only one would
                // miss a per-channel error entirely.
                if (tR[i] != tG[i] || tG[i] != tB[i]) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: furnace (%s) path %u throughput is not "
                                 "grey: (%.9g,%.9g,%.9g) from a grey base colour\n",
                                 kRuns[run].name, i, static_cast<double>(tR[i]),
                                 static_cast<double>(tG[i]), static_cast<double>(tB[i]));
                    wf.destroy(ctx.allocator());
                    return 1;
                }
                const double v = tR[i];
                if (!std::isfinite(v)) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: furnace (%s) path %u throughput is not "
                                 "finite (%.9g)\n",
                                 kRuns[run].name, i, v);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
                sum += v;
                sumSq += v * v;
                if (v > maxV) maxV = v;
                if (v < minV) minV = v;

                if (run == 0) {
                    const double dev = std::abs(v - 1.0);
                    if (dev > lambertMaxDeviation) lambertMaxDeviation = dev;
                    if (dev > kFurnaceUlpBound) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: white furnace path %u returned %.9g, "
                                     "expected 1.0 within %.3g (4 ulp). With albedo 1 and no "
                                     "absorption the cosine-sampled Lambert estimator is a "
                                     "CONSTANT 1 for every direction -- zero variance -- so this "
                                     "is an energy-conservation error in the "
                                     "sample-evaluate-weight loop, not Monte Carlo noise\n",
                                     i, v, static_cast<double>(kFurnaceUlpBound));
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                } else if (run == 1) {
                    if (v > 1.0 + kFurnaceUlpBound) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: glossy furnace path %u returned "
                                     "%.9g > 1 -- with F identically 1 and the lobe probability "
                                     "exactly 1, the weight is G2/G1, which height-correlated "
                                     "Smith bounds at 1 (Heitz 2014 Eq. 99). A value above 1 is "
                                     "energy created out of nothing\n",
                                     i, v);
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                    if (v < 0.0) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: glossy furnace path %u returned a "
                                     "negative throughput %.9g\n",
                                     i, v);
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                } else {
                    // (a): the pointwise mixture bound derived above. NOT 1 --
                    // a mixture estimator's weight legitimately exceeds the
                    // albedo on directions one lobe's density under-covers.
                    if (v > mixPointwiseBound) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: mixture furnace path %u returned "
                                     "%.9g, above the derived pointwise bound %.9g = "
                                     "rho(1-metallic)/(1-q) + specScale/q with q in [%.6f, "
                                     "%.6f]. Every term in that bound comes from F <= 1 and "
                                     "G2/G1 <= 1, so exceeding it is energy created out of "
                                     "nothing\n",
                                     i, v, mixPointwiseBound, qMin, qMax);
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                    if (v < 0.0) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: mixture furnace path %u returned a "
                                     "negative throughput %.9g\n",
                                     i, v);
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                }
            }
            furnaceMean[run] = sum / static_cast<double>(kCapacity);
            furnaceMax[run] = maxV;
            furnaceMin[run] = minV;
            const double meanSq = sumSq / static_cast<double>(kCapacity);
            const double var =
                std::max(0.0, meanSq - furnaceMean[run] * furnaceMean[run]);
            furnaceStdErr[run] = std::sqrt(var / static_cast<double>(kCapacity));

            wf.destroy(ctx.allocator());
        }

        std::printf("[diff_gpu_probe] OK: white furnace (albedo 1, no absorption, constant "
                    "environment) returns 1.0 for all %u paths; mean %.9g, max |deviation| %.3g "
                    "(derived bound %.3g = 4 ulp; Monte Carlo variance is exactly 0 -- see the "
                    "derivation above this check)\n",
                    kCapacity, furnaceMean[0], lambertMaxDeviation,
                    static_cast<double>(kFurnaceUlpBound));

        // The deficit must be REAL, not just "<= 1". A mean of exactly 1
        // would mean the specular lobe silently degenerated to the Lambert
        // fast path; a mean at 0 would mean every sample was rejected.
        if (!(furnaceMean[1] < 1.0)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: glossy furnace mean is %.9g, expected strictly "
                         "below 1 -- single-scattering GGX loses energy by construction, so a "
                         "mean of exactly 1 means the specular lobe was not the one evaluated\n",
                         furnaceMean[1]);
            return 1;
        }
        if (!(furnaceMean[1] > 0.5)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: glossy furnace mean is %.9g. At roughness 0.3 "
                         "the single-scattering Smith deficit is a few percent, not half the "
                         "energy -- this size of loss means samples are being rejected or "
                         "weighted with the wrong density, not that multiple scattering is "
                         "missing\n",
                         furnaceMean[1]);
            return 1;
        }
        std::printf("[diff_gpu_probe] OK: glossy furnace (white conductor, roughness 0.30) "
                    "conserves energy without creating any: every one of %u paths in [0, 1], "
                    "mean %.9g (strictly below 1 -- the known single-scattering GGX deficit), "
                    "min %.9g, max %.9g\n",
                    kCapacity, furnaceMean[1], furnaceMin[1], furnaceMax[1]);

        // ------------------------------------------------------------------
        // 23. The intermediate-q run's mean, against a quadrature of the
        // oracle's own f. See the derivation above this block.
        // ------------------------------------------------------------------
        constexpr uint32_t kQuadTheta = 512;
        constexpr uint32_t kQuadPhi = 512;
        // rho_dir over the frame's |N.V| range. The range is narrow, so a
        // scan is enough to bracket it; the observed spread is printed, and
        // it is orders of magnitude under the Monte Carlo allowance.
        constexpr uint32_t kCosScan = 5;
        double rhoLo = 0.0;
        double rhoHi = 0.0;
        for (uint32_t k = 0; k < kCosScan; ++k) {
            const double t =
                static_cast<double>(k) / static_cast<double>(kCosScan - 1);
            const double c = cosMin + (1.0 - cosMin) * t;
            const double r = oracleDirectionalAlbedo(mixMat, c, kQuadTheta, kQuadPhi);
            if (k == 0 || r < rhoLo) rhoLo = r;
            if (k == 0 || r > rhoHi) rhoHi = r;
        }
        // Discretisation error, measured rather than assumed: the same
        // integral at half resolution in each dimension.
        const double rhoCoarse =
            oracleDirectionalAlbedo(mixMat, 1.0, kQuadTheta / 2u, kQuadPhi / 2u);
        const double rhoFine = oracleDirectionalAlbedo(mixMat, 1.0, kQuadTheta, kQuadPhi);
        const double quadError = std::abs(rhoFine - rhoCoarse);
        // 5 standard errors of the GPU sample mean, computed from the samples.
        const double mixSigma = furnaceStdErr[kMixtureRun];
        const double meanAllowance = 5.0 * mixSigma + quadError;
        const double mixLower = rhoLo - meanAllowance;
        const double mixUpper = rhoHi + meanAllowance;
        const double rhoMid = 0.5 * (rhoLo + rhoHi);
        const double sigmasOff =
            mixSigma > 0.0 ? (furnaceMean[kMixtureRun] - rhoMid) / mixSigma : 0.0;
        if (!(furnaceMean[kMixtureRun] >= mixLower) || !(furnaceMean[kMixtureRun] <= mixUpper)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: mixture furnace mean is %.9g, outside [%.9g, "
                         "%.9g]. The reference is rho_dir = INT f cos dL over the upper "
                         "hemisphere, integrated in double precision from the CPU oracle's own f "
                         "(not from the GLSL), and evaluated across this frame's |N.V| range: "
                         "[%.9g, %.9g]. The allowance is %.3g = 5 x the sample standard error "
                         "%.3g plus the measured quadrature error %.3g. The estimator is "
                         "unbiased for rho_dir for ANY lobe probability in (0,1) that covers f's "
                         "support, so landing outside means the mixture density and the branch "
                         "that was actually taken disagree, or a lobe has stopped being "
                         "reachable -- it is %+.1f sigma off\n",
                         furnaceMean[kMixtureRun], mixLower, mixUpper, rhoLo, rhoHi,
                         meanAllowance, mixSigma, quadError, sigmasOff);
            return 1;
        }
        std::printf("[diff_gpu_probe] OK: mixture furnace (roughness 0.30, metallic 0.50, "
                    "specularWeight 1.00 -- q in [%.4f, %.4f], so both branches run and every "
                    "path forms the full mixture density and divides): all %u paths in [0, %.6f] "
                    "(derived pointwise bound), mean %.9g matches the quadrature of the CPU "
                    "oracle's own f, rho_dir in [%.6f, %.6f] over this frame's |N.V| range, to "
                    "%+.2f sigma (allowance %.3g = 5 x sigma %.3g + quadrature error %.3g at "
                    "%ux%u); min %.9g, max %.9g\n",
                    qMin, qMax, kCapacity, mixPointwiseBound, furnaceMean[kMixtureRun], rhoLo,
                    rhoHi, sigmasOff, meanAllowance, mixSigma, quadError, kQuadTheta, kQuadPhi,
                    furnaceMin[kMixtureRun], furnaceMax[kMixtureRun]);
    }

    // 24-26. ENVIRONMENT IMPORTANCE SAMPLING (Stage 0b-2b Task 3).
    //
    // wf_scatter.comp now draws a direction from the environment's
    // sin(theta)-weighted luminance every bounce, through
    // shaders/includes/rt/env_sampling.glsl's sampleEnvMap -- the same header
    // the RT pipeline's raygen shaders call -- and writes the (direction,
    // pdf) pair to its binding-6 sink. Nothing consumes it yet (NEE and MIS
    // are Task 4), so it is checked directly rather than through an image.
    //
    // WHAT IS AND IS NOT UNDER TEST. The CDF arrays uploaded to the GPU are
    // built by ohao::EnvCDF (ohao/render/rt/env_cdf.cpp), the SAME builder
    // that feeds the RT pipeline. Under test are: the GPU's two binary
    // searches over those arrays, the texel -> direction map, the CDF ->
    // solid-angle pdf conversion, and the binding/push-constant path that
    // gets the arrays to the shader THROUGH THIS PROBE'S OWN CALL SITE --
    // GpuProbeContext::runWavefrontScatterProbe, which fills ScatterPush's
    // envWidth/envHeight/envIntegral BY HAND (gpu_probe_context.cpp). It is
    // NOT a test of ohao::diff::WavefrontLoop::record's OWN fill of those
    // same three fields at its own, separate call site
    // (wavefront_loop.cpp) -- record() is what the production wavefront
    // loop actually calls, and until check 27 below, nothing exercised it
    // with an environment where a mistake in that fill would be visible.
    //
    // THE ORACLE IS NOT THE CDF. It would have been easy to bin the samples
    // and compare against differences of the uploaded CDF arrays, but that
    // oracle shares EnvCDF's own normalisation with the thing under test: a
    // builder that (say) forgot the sin(theta) weight would produce a CDF the
    // GPU sampled faithfully and the check would pass. The expected texel
    // probabilities below are instead computed here, in double precision,
    // straight from the luminance image and the analytic solid-angle weight:
    //
    //     p(x,y) = L(x,y) sin(theta_y) / sum over all texels of the same,
    //     theta_y = pi (y + 0.5) / H,
    //
    // which is the distribution the whole pipeline -- builder included -- is
    // SUPPOSED to realise. That makes EnvCDF part of what check 24 tests, not
    // part of its oracle.
    //
    // ------------------------------------------------------------------
    // 24. The chi-squared bound, derived
    // ------------------------------------------------------------------
    //
    // N samples are drawn and binned into the K = envW * envH texels of the
    // map. Under the null hypothesis (the sampler realises p exactly, and the
    // samples are independent), the count vector is multinomial(N, p) and
    //
    //     X^2 = sum over k of (O_k - E_k)^2 / E_k,   E_k = N p_k
    //
    // converges to chi-squared with K - 1 degrees of freedom (one constraint:
    // the counts sum to N). Pearson's approximation is conventionally taken
    // as adequate once every E_k >= 5; that condition is ASSERTED below
    // rather than assumed, and the observed minimum is printed, so a later
    // change to the environment or the sample count that quietly invalidates
    // the approximation fails loudly instead of weakening the test.
    //
    // The rejection threshold is the (1 - alpha) quantile of chi-squared with
    // K - 1 = 127 degrees of freedom at alpha = 1e-6. alpha is that small on
    // purpose: this probe is deterministic (fixed seeds, fixed geometry), so
    // there is no run-to-run flake to trade against, and the failures worth
    // catching -- a wrong search, a wrong row stride, a wrong pdf -- move X^2
    // by orders of magnitude, not by a factor of two. Choosing 1e-6 over,
    // say, 0.01 costs almost no power against those and removes any argument
    // that a pass was luck.
    //
    // The quantile is computed, not tabulated, via the Wilson-Hilferty
    // transform (Wilson & Hilferty, PNAS 17 (1931) 684): (X^2/df)^(1/3) is
    // approximately normal with mean 1 - 2/(9 df) and variance 2/(9 df), so
    //
    //     chi2_{df, 1-alpha} ~= df * (1 - 2/(9df) + z_alpha sqrt(2/(9df)))^3
    //
    // with z_alpha = 4.753424 the standard normal 1 - 1e-6 quantile. At
    // df = 127 that gives ~217.9. The transform's accuracy at this df was
    // checked against a published table at a quantile tables actually carry:
    // it returns 149.49 for chi2_{100, 0.999} against the tabulated 149.449,
    // an error of 0.03% -- three orders of magnitude smaller than the margin
    // between a passing and a failing run here.
    //
    // NOTHING BELOW WAS TUNED. The bound was derived first; the observed X^2
    // is printed on the OK: line so that a value creeping up toward it is
    // visible rather than silent.
    //
    // ------------------------------------------------------------------
    // 26. Why the pdfs must sum to exactly 1, and to what tolerance
    // ------------------------------------------------------------------
    //
    // sampleEnvMap returns pdfUV / (2 pi^2 sin theta) with
    // pdfUV = condDiff * margDiff * W * H. The midpoint solid angle of texel
    // (x,y) is dOmega = (2pi/W)(pi/H) sin theta_y with the SAME theta_y, so
    //
    //     pdf(x,y) * dOmega(x,y) = condDiff * margDiff = p_CDF(x,y),
    //
    // and summing over every texel must give exactly 1 -- the sine cancels,
    // leaving the CDF's own total mass. This is an identity, not a quadrature
    // approximation, so the only error is float32 arithmetic:
    //
    //   * condDiff and margDiff are differences of CDF entries near 1, each
    //     with absolute error at most one ulp of 1.0f (2^-24), so at most
    //     2^-23 = 1.19e-7 per difference. Their contribution to the total is
    //     1.19e-7 * (sum over texels of margDiff + sum over texels of
    //     condDiff) = 1.19e-7 * (W * 1 + H * 1) = 1.19e-7 * 24 = 2.9e-6.
    //   * The remaining float32 operations (the W*H product, the division by
    //     2 pi^2 sin theta, storing the result) contribute a few ulp of
    //     RELATIVE error each; against a total of 1 that is under 1e-6.
    //   * The GPU divides by a float32 sin(theta) and the host multiplies by
    //     a double sin(theta); the two differ by ~1e-7 relative, under 1e-7
    //     against a probability-weighted total of 1.
    //
    // Total: about 4e-6. The asserted tolerance is 1e-5, roughly 2.5x that,
    // and the observed deviation is printed.
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 48;
        constexpr uint32_t kCapacity = kW * kH;  // 3072
        constexpr float kPlaneDistance = 2.0f;
        constexpr float kTanHalfFov = 0.2f;
        constexpr uint32_t kIterationSeed = 909090u;

        constexpr uint32_t kEnvW = 16;
        constexpr uint32_t kEnvH = 8;
        constexpr uint32_t kEnvTexels = kEnvW * kEnvH;  // 128 bins, df = 127
        // Eight scatter dispatches over the same 3072 paths. The bounce
        // counter advances with each, so wf_scatter.comp's fast-forward puts
        // every dispatch at a different position in each path's stream and
        // the 8 * 3072 = 24576 draws are 24576 DIFFERENT PCG outputs, not
        // eight repeats. Keeping one iterationSeed throughout is deliberate:
        // that is the configuration the integrator actually runs in, so the
        // stream this check exercises is the production one.
        constexpr uint32_t kEnvDispatches = 8;
        constexpr uint32_t kSampleCount = kCapacity * kEnvDispatches;

        // Pearson's rule of thumb, asserted rather than assumed.
        constexpr double kMinExpectedPerBin = 5.0;
        // Standard normal 1 - 1e-6 quantile, for the Wilson-Hilferty
        // transform above.
        constexpr double kChiSqZ = 4.753424;
        constexpr double kChiSqAlpha = 1e-6;
        // Derived above: the identity's float32 error budget is ~4e-6.
        constexpr double kPdfSumTolerance = 1e-5;
        // Texel-centre round-trip slack. equirectPixelToDir emits the CENTRE
        // of the chosen texel, so inverting it must land within half a texel
        // of an integer + 0.5. The float32 error in that round trip is under
        // 2e-6 (acos is worst at the polar rows, where its derivative is
        // 1/sin(theta) = 5.13 at H = 8); 1e-3 is three orders of magnitude
        // of slack and still 500x tighter than the half-texel that would
        // make the binning ambiguous.
        constexpr double kCentreSlack = 1e-3;

        constexpr double kPi = 3.14159265358979323846;

        // --- The environment. Strictly positive in every texel (so every
        // bin has non-zero expected probability and every returned pdf must
        // be > 0), asymmetric in BOTH axes (so a transposed or reversed
        // index cannot coincide with the truth), and with a small bright
        // block so that importance sampling has something to concentrate on
        // and a uniform sampler is not accidentally close. ---
        std::vector<float> envRgba(static_cast<std::size_t>(kEnvTexels) * 4u, 0.0f);
        std::vector<double> envLum(kEnvTexels, 0.0);
        for (uint32_t y = 0; y < kEnvH; ++y) {
            for (uint32_t x = 0; x < kEnvW; ++x) {
                double L = 1.0 + 0.1 * static_cast<double>(x) + 0.3 * static_cast<double>(y);
                if (x >= 10 && x <= 11 && y >= 2 && y <= 3) L += 20.0;
                const std::size_t k = static_cast<std::size_t>(y) * kEnvW + x;
                envLum[k] = L;
                envRgba[k * 4u + 0u] = static_cast<float>(L);
                envRgba[k * 4u + 1u] = static_cast<float>(L);
                envRgba[k * 4u + 2u] = static_cast<float>(L);
                envRgba[k * 4u + 3u] = 1.0f;
            }
        }

        // --- The oracle: p(x,y) proportional to L * sin(theta), in double,
        // from the luminance image above and nothing else. EnvCDF's grey
        // response (0.2126 + 0.7152 + 0.0722 = 1) is applied here too so the
        // two agree on the scalar being distributed, but the normalisation,
        // the sine weight and the row/column structure are all recomputed
        // independently of the CDF that was uploaded. ---
        std::vector<double> expectedP(kEnvTexels, 0.0);
        std::vector<double> sinThetaRow(kEnvH, 0.0);
        double envTotal = 0.0;
        for (uint32_t y = 0; y < kEnvH; ++y) {
            const double theta = kPi * (static_cast<double>(y) + 0.5) / static_cast<double>(kEnvH);
            sinThetaRow[y] = std::sin(theta);
            for (uint32_t x = 0; x < kEnvW; ++x) {
                const std::size_t k = static_cast<std::size_t>(y) * kEnvW + x;
                const double grey = (0.2126 + 0.7152 + 0.0722) * envLum[k];
                expectedP[k] = grey * sinThetaRow[y];
                envTotal += expectedP[k];
            }
        }
        for (double& p : expectedP) p /= envTotal;

        double minExpectedCount = 1e300;
        for (uint32_t k = 0; k < kEnvTexels; ++k) {
            minExpectedCount = std::min(minExpectedCount,
                                        expectedP[k] * static_cast<double>(kSampleCount));
        }
        if (!(minExpectedCount >= kMinExpectedPerBin)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: env chi-squared setup: the least likely of the "
                         "%u texels has expected count %.4g, below the %.1f Pearson's "
                         "approximation needs. The chi-squared distribution would not be the "
                         "right reference distribution for this test -- raise kEnvDispatches or "
                         "reduce the environment's dynamic range rather than lowering this\n",
                         kEnvTexels, minExpectedCount, kMinExpectedPerBin);
            return 1;
        }

        // --- The CDF actually uploaded: ohao::EnvCDF, the RT pipeline's own
        // builder, so the diff pipeline cannot drift from it. ---
        ohao::EnvCDF envCdf;
        envCdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
        if (!envCdf.valid()) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: ohao::EnvCDF::build produced no CDF for "
                                  "a %ux%u strictly-positive environment\n",
                         kEnvW, kEnvH);
            return 1;
        }

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: env sampling buffers build\n");
            return 1;
        }
        if (!wf.uploadEnvironment(envCdf.marginalSpan(), envCdf.conditionalSpan(),
                                  envCdf.integral())) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: env CDF upload rejected (%zu marginal, "
                                  "%zu conditional floats for %ux%u)\n",
                         envCdf.marginalCDF().size(), envCdf.conditionalCDF().size(), kEnvW,
                         kEnvH);
            wf.destroy(ctx.allocator());
            return 1;
        }

        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        ohao::diff::WavefrontGenerateCamera camera;
        camera.tanHalfFov = kTanHalfFov;
        std::vector<uint32_t> queue0;
        if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: env sampling setup: wf_generate\n");
            wf.destroy(ctx.allocator());
            return 1;
        }
        // One trace, so every path has a real hit point and a real geometric
        // normal and the scatter dispatches below take their surface branch
        // rather than the miss guard. The environment sample itself does not
        // depend on either -- it is drawn before the guard -- but running the
        // stage in its degenerate configuration would be a weaker test of the
        // dispatch as a whole.
        std::vector<uint32_t> queue1;
        if (!ctx.runWavefrontIntersectProbe(wf, kPlaneDistance, /*quadMinY=*/-1.0f, queue1)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: env sampling setup: wf_intersect\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        std::vector<uint32_t> binCount(kEnvTexels, 0u);
        // The pdf the GPU reported for each texel, and whether it reported
        // one at all. Every sample of the same texel must return the SAME
        // pdf bit for bit: it is a pure function of the two CDF arrays and
        // the texel index, evaluated by the same instructions on the same
        // device, so anything else means the shader read something that
        // varies per invocation.
        std::vector<float> texelPdf(kEnvTexels, 0.0f);
        std::vector<uint8_t> texelSeen(kEnvTexels, 0u);
        double maxCentreError = 0.0;
        float minPdf = std::numeric_limits<float>::infinity();
        float maxPdf = 0.0f;

        uint32_t srcQueueBase = kCapacity;
        uint32_t srcCountSlot = ohao::diff::WavefrontBuffers::kNextCountSlot;
        uint32_t dstQueueBase = 0u;
        uint32_t dstCountSlot = ohao::diff::WavefrontBuffers::kCurrentCountSlot;

        for (uint32_t d = 0; d < kEnvDispatches; ++d) {
            std::vector<uint32_t> outQueue;
            std::vector<float> outDraws;
            std::vector<float> envSamples;
            if (!ctx.runWavefrontScatterProbe(wf, srcQueueBase, srcCountSlot, dstQueueBase,
                                              dstCountSlot, /*albedo=*/1.0f, kIterationSeed,
                                              outQueue, outDraws, /*material=*/{}, &envSamples)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: env sampling scatter dispatch %u\n",
                             d);
                wf.destroy(ctx.allocator());
                return 1;
            }
            const std::uint32_t requeued = wf.readbackCounter(ctx.allocator(), dstCountSlot);
            if (requeued != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: env sampling dispatch %u re-queued %u paths, "
                             "expected all %u -- the sample count the chi-squared bound is "
                             "derived for would not be the count actually drawn\n",
                             d, requeued, kCapacity);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (envSamples.size() != static_cast<std::size_t>(kCapacity) * ohao::diff::kEnvSampleFloats) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: env sampling dispatch %u returned %zu env "
                             "sample floats, expected %u\n",
                             d, envSamples.size(), kCapacity * ohao::diff::kEnvSampleFloats);
                wf.destroy(ctx.allocator());
                return 1;
            }

            for (uint32_t i = 0; i < kCapacity; ++i) {
                const double dx = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 0u];
                const double dy = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 1u];
                const double dz = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 2u];
                const float pdf = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 3u];

                const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (!std::isfinite(len) || std::abs(len - 1.0) > 1e-4) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: env sample (dispatch %u, path %u) "
                                 "direction (%.9g,%.9g,%.9g) has length %.9g, expected a unit "
                                 "vector\n",
                                 d, i, dx, dy, dz, len);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
                if (!(pdf > 0.0f) || !std::isfinite(pdf)) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: env sample (dispatch %u, path %u) "
                                 "returned pdf %.9g. Every texel of this environment has strictly "
                                 "positive luminance, so no sampled direction may have zero or "
                                 "non-finite density -- a zero here is a division waiting to "
                                 "happen in Task 4's estimator\n",
                                 d, i, static_cast<double>(pdf));
                    wf.destroy(ctx.allocator());
                    return 1;
                }
                minPdf = std::min(minPdf, pdf);
                maxPdf = std::max(maxPdf, pdf);

                // Invert equirectPixelToDir. This is the same inverse
                // pdfEnvMap performs, written from the forward map's
                // definition rather than copied out of it -- and written ONCE,
                // in oracleEnvTexelOf, rather than here and again in checks
                // 27, 31 and 33/34. All four rest on the binning agreeing.
                const OracleEnvTexel texel = oracleEnvTexelOf(dx, dy, dz, kEnvW, kEnvH);
                const double fx = texel.fx;
                const double fy = texel.fy;
                const int ix = texel.ix;
                const int iy = texel.iy;
                // A direction that is NOT a texel centre means the forward
                // map and this inverse disagree, and every bin index below
                // would then be meaningless -- so this is checked before the
                // count is taken, not after.
                const double centreError = texel.centreError;
                if (!(centreError <= kCentreSlack)) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: env sample (dispatch %u, path %u) "
                                 "direction (%.9g,%.9g,%.9g) inverts to (%.6f, %.6f) in texel "
                                 "units, which is %.3g away from the centre of texel (%d, %d). "
                                 "equirectPixelToDir emits texel CENTRES, so this is a "
                                 "disagreement between the forward map and its inverse, not "
                                 "rounding\n",
                                 d, i, dx, dy, dz, fx, fy, centreError, ix, iy);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
                maxCentreError = std::max(maxCentreError, centreError);

                const std::size_t k = texel.index;
                binCount[k] += 1u;
                if (texelSeen[k] == 0u) {
                    texelSeen[k] = 1u;
                    texelPdf[k] = pdf;
                } else if (texelPdf[k] != pdf) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: texel (%d, %d) was returned with two "
                                 "different pdfs, %.9g and %.9g. The pdf is a pure function of "
                                 "the two CDF arrays and the texel index, so it cannot vary "
                                 "between invocations unless the shader read something that "
                                 "does\n",
                                 ix, iy, static_cast<double>(texelPdf[k]),
                                 static_cast<double>(pdf));
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }

            std::swap(srcQueueBase, dstQueueBase);
            std::swap(srcCountSlot, dstCountSlot);
        }

        wf.destroy(ctx.allocator());

        // Checks 24, 25 and 26 are three INDEPENDENT verdicts computed from
        // this same run's 24576 samples: 24 from binCount, 25 from
        // minPdf/maxPdf (collected per-sample above, before any binning),
        // 26 from texelPdf/texelSeen. Nothing below depends on an earlier
        // one of the three having passed, so a perturbation that fails one
        // must not stop the other two from being computed and reported --
        // otherwise a bug that also happens to trip the chi-squared can
        // never be shown to be (or not be) independently caught by the
        // pdf-ratio and integrate-to-1 identities too, which is exactly the
        // comparison Step 5's perturbation report depends on. Each verdict
        // is therefore evaluated and printed unconditionally; failures
        // accumulate in checksFailed, and the group returns non-zero once,
        // at the very end, only after all three have had their say. Every
        // assertion below is exactly as strong as it was before this
        // restructuring -- only the control flow between them changed.
        bool checksFailed = false;

        // --- 24. Pearson's chi-squared against the independent oracle. ---
        double chiSq = 0.0;
        uint32_t totalBinned = 0;
        uint32_t worstBin = 0;
        double worstTerm = -1.0;
        for (uint32_t k = 0; k < kEnvTexels; ++k) {
            const double expected = expectedP[k] * static_cast<double>(kSampleCount);
            const double diff = static_cast<double>(binCount[k]) - expected;
            const double term = diff * diff / expected;
            chiSq += term;
            totalBinned += binCount[k];
            if (term > worstTerm) {
                worstTerm = term;
                worstBin = k;
            }
        }
        const double df = static_cast<double>(kEnvTexels) - 1.0;
        const double whT = 2.0 / (9.0 * df);
        const double chiSqCritical =
            df * std::pow(1.0 - whT + kChiSqZ * std::sqrt(whT), 3.0);
        if (totalBinned != kSampleCount) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: binned %u env samples, expected %u -- the "
                         "chi-squared statistic is only multinomial if every sample landed in "
                         "exactly one bin\n",
                         totalBinned, kSampleCount);
            checksFailed = true;
        } else if (!(chiSq <= chiSqCritical)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: env importance sampling chi-squared = %.4f over "
                         "%u samples in %u bins (df %.0f) exceeds the %.4f rejection threshold "
                         "(alpha = %.0e, Wilson-Hilferty). The empirical distribution of "
                         "sampleEnvMap's directions does not match the sin(theta)-weighted "
                         "luminance of the environment. Worst bin %u (texel %u, %u): observed "
                         "%u, expected %.3f, contributing %.3f\n",
                         chiSq, kSampleCount, kEnvTexels, df, chiSqCritical, kChiSqAlpha, worstBin,
                         worstBin % kEnvW, worstBin / kEnvW, binCount[worstBin],
                         expectedP[worstBin] * static_cast<double>(kSampleCount), worstTerm);
            checksFailed = true;
        } else {
            std::printf("[diff_gpu_probe] OK: env importance sampling matches an independent "
                        "sin(theta)-weighted-luminance oracle -- chi-squared %.4f over %u samples "
                        "in %u bins (df %.0f), below the derived %.4f threshold (alpha %.0e via "
                        "Wilson-Hilferty; least likely bin expected %.2f >= %.1f, so Pearson's "
                        "approximation holds); worst bin %u observed %u vs expected %.2f\n",
                        chiSq, kSampleCount, kEnvTexels, df, chiSqCritical, kChiSqAlpha,
                        minExpectedCount, kMinExpectedPerBin, worstBin, binCount[worstBin],
                        expectedP[worstBin] * static_cast<double>(kSampleCount));
        }

        // --- 25. Every returned pdf strictly positive (asserted per sample
        // above) and every direction a texel centre (likewise), plus the
        // pdf's SHAPE: it must be proportional to luminance alone.
        //
        // This is the sin(theta) Jacobian stated as an equality rather than
        // as an integral. The CDF's texel probability is proportional to
        // L * sin(theta_y); sampleEnvMap divides by
        // 2 pi^2 sin(theta_y) to get a solid-angle density, so the sine
        // cancels EXACTLY and
        //
        //     pdf(x,y) proportional to L(x,y),   independent of the row.
        //
        // The ratio of the largest returned pdf to the smallest must
        // therefore equal the luminance range of the map, a number this test
        // knows from the image it built and never from the shader. A sine
        // applied once too often or once too few -- in the builder or in the
        // shader -- breaks this while leaving both positivity and the
        // integral-to-1 identity of check 26 intact.
        //
        // TOLERANCE. The dominant error is float32 cancellation in the CDF
        // differences: the least likely texel has condDiff ~ 0.036 and
        // margDiff ~ 0.018 formed by subtracting values near 1, each with
        // absolute error up to 2^-23, giving relative errors of ~3.4e-6 and
        // ~6.6e-6. The ratio compounds two such pdfs, so ~2e-5. Asserted at
        // 1e-4, five times that; the observed value is printed.
        constexpr double kPdfRatioTolerance = 1e-4;
        double lumMin = 1e300;
        double lumMax = 0.0;
        for (uint32_t k = 0; k < kEnvTexels; ++k) {
            lumMin = std::min(lumMin, envLum[k]);
            lumMax = std::max(lumMax, envLum[k]);
        }
        const double lumRatio = lumMax / lumMin;
        const double pdfRatio = static_cast<double>(maxPdf) / static_cast<double>(minPdf);
        if (!(std::abs(pdfRatio / lumRatio - 1.0) <= kPdfRatioTolerance)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: the returned env pdfs span a ratio of %.9f, but "
                         "the environment's luminance spans %.9f. The solid-angle pdf must be "
                         "proportional to luminance ALONE -- the sin(theta) in the CDF's texel "
                         "probability is cancelled exactly by the sin(theta) in the "
                         "UV-to-solid-angle Jacobian -- so a different ratio means the sine was "
                         "applied a different number of times on the two sides (relative "
                         "difference %.3g, tolerance %.3g)\n",
                         pdfRatio, lumRatio, std::abs(pdfRatio / lumRatio - 1.0),
                         kPdfRatioTolerance);
            checksFailed = true;
        } else {
            std::printf("[diff_gpu_probe] OK: all %u returned env pdfs are finite and strictly "
                        "positive (min %.9g, max %.9g), their %.6f:1 range matches the map's own "
                        "%.6f:1 luminance range to %.3g (tolerance %.3g -- the sin(theta) "
                        "Jacobian cancels exactly), and every sampled direction inverts to a "
                        "texel centre within %.3g (slack %.3g)\n",
                        kSampleCount, static_cast<double>(minPdf), static_cast<double>(maxPdf),
                        pdfRatio, lumRatio, std::abs(pdfRatio / lumRatio - 1.0),
                        kPdfRatioTolerance, maxCentreError, kCentreSlack);
        }

        // --- 26. The returned pdfs integrate to 1 over the sphere. ---
        uint32_t unseen = 0;
        for (uint32_t k = 0; k < kEnvTexels; ++k) {
            if (texelSeen[k] == 0u) ++unseen;
        }
        if (unseen != 0) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: %u of %u texels were never sampled, so the pdf "
                         "sum below would be missing their contribution and could not be "
                         "compared against 1. Every texel's expected count is at least %.2f, so "
                         "this is not chance\n",
                         unseen, kEnvTexels, minExpectedCount);
            checksFailed = true;
        } else {
            const double dOmegaScale = (2.0 * kPi / static_cast<double>(kEnvW)) *
                                       (kPi / static_cast<double>(kEnvH));
            double pdfIntegral = 0.0;
            for (uint32_t y = 0; y < kEnvH; ++y) {
                for (uint32_t x = 0; x < kEnvW; ++x) {
                    const std::size_t k = static_cast<std::size_t>(y) * kEnvW + x;
                    pdfIntegral += static_cast<double>(texelPdf[k]) * dOmegaScale * sinThetaRow[y];
                }
            }
            if (!(std::abs(pdfIntegral - 1.0) <= kPdfSumTolerance)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: the pdfs sampleEnvMap returned integrate to "
                             "%.9f over the sphere, not 1 within %.3g. pdf * dOmega is condDiff * "
                             "margDiff exactly (the sin(theta) cancels), so the total is the "
                             "CDF's own mass and any departure beyond float32 rounding is a "
                             "normalisation error, not quadrature\n",
                             pdfIntegral, kPdfSumTolerance);
                checksFailed = true;
            } else {
                std::printf("[diff_gpu_probe] OK: the env pdfs integrate to %.9f over the sphere "
                            "(|deviation| %.3g, derived float32 budget ~4e-6, asserted %.3g) "
                            "across all %u texels\n",
                            pdfIntegral, std::abs(pdfIntegral - 1.0), kPdfSumTolerance,
                            kEnvTexels);
            }
        }

        // All three verdicts are computed and printed above regardless of
        // one another's outcome; only now, after all three have reported,
        // does the group return non-zero if any failed.
        if (checksFailed) {
            return 1;
        }
    }

    // ------------------------------------------------------------------
    // 27. WavefrontLoop::record's OWN push-constant fill (Stage 0b-2b Task 3
    //     fix, finding 2).
    // ------------------------------------------------------------------
    //
    // ohao/diff/wavefront/wavefront_loop.cpp fills ScatterPush's
    // envWidth/envHeight/envIntegral tail from `buffers` at record()'s own
    // call site. Checks 24-26 above never exercise that line: they run
    // through GpuProbeContext::runWavefrontScatterProbe, which fills
    // ScatterPush ITSELF (gpu_probe_context.cpp), at a different call site
    // entirely. The only caller of record() anywhere in this file is
    // runWavefrontFusedLoopProbe, which -- until this check was added --
    // built its WavefrontBuffers at the default 1x1 environment and never
    // read binding 6 back, so a transposed envWidth/envHeight inside
    // record() was UNOBSERVABLE: at 1x1 the two dimensions are
    // interchangeable and every direction still lands on the same one
    // texel.
    //
    // This check gives the fused loop a genuinely NON-SQUARE environment
    // (kEnvW != kEnvH) and reads the env-sample sink back after running
    // record() once, so a W<->H swap is no longer symmetric.
    //
    // THE ORACLE. wf.build(allocator, capacity, kEnvW, kEnvH) with no
    // uploadEnvironment() call afterward seeds the UV-uniform CDF
    // wavefront_buffers.cpp documents: cond[y][x] = (x+1)/W, marg[y] =
    // (y+1)/H (the SAME sampler task-3-report.md's Step 2 used to show the
    // chi-squared test has discriminating power the ratio/integral checks
    // alone do not -- see task-3-fix-report.md for that correction). Its CDF
    // texel probability is uniform in UV, p_CDF(x,y) = margDiff * condDiff =
    // (1/H)(1/W), so sampleEnvMap's pdf collapses to a CLOSED FORM that
    // depends on nothing but which row y the texel is in:
    //
    //     pdfUV = condDiff * margDiff * W * H = 1
    //     pdf = pdfUV / (2 pi^2 sin(theta_y)) = 1 / (2 pi^2 sin(theta_y))
    //
    // -- no CDF builder, no luminance image, just kEnvW, kEnvH and
    // elementary trigonometry, computed here independently of anything the
    // GPU did. If record() ever swaps envWidth and envHeight in the push
    // constants it fills, sampleEnvMap's two binary searches run against the
    // WRONG bound for each CDF array (the marginal array actually holds
    // envHeight() entries; searching it as if it held envWidth() either
    // walks off the array or stops short), so the returned direction stops
    // being the centre of any texel under the buffers' REAL (kEnvW, kEnvH)
    // -- caught by the texel-centre check below, the same technique check 25
    // uses -- and the closed-form pdf above stops matching.
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 8;  // wf_generate's 1-D dispatch requires this exactly.
        constexpr uint32_t kCapacity = kW * kH;  // 512
        constexpr float kAlbedo = 0.5f;
        constexpr uint32_t kIterationSeed = 20260828u;
        constexpr uint32_t kBounces = 1;  // One dispatch through record() is enough.

        // Non-square on purpose -- see the comment above. A W<->H swap
        // inside record() is invisible whenever kEnvW == kEnvH.
        constexpr uint32_t kEnvW = 16;
        constexpr uint32_t kEnvH = 4;
        static_assert(kEnvW != kEnvH,
                     "check 27 needs a non-square environment to detect a W<->H swap");

        constexpr double kPi = 3.14159265358979323846;
        // Same basis as check 25's texel-centre slack: three orders of
        // magnitude of headroom over the float32 round trip, five hundred
        // times tighter than the half-texel that would make the bin
        // ambiguous.
        constexpr double kCentreSlack = 1e-3;
        // Same float32-cancellation budget as checks 25/26 (differences of
        // CDF entries, a W*H product, a division by a float32 sin(theta));
        // asserted at the same order of magnitude check 25 uses for its own
        // pdf comparison.
        constexpr double kPdfRelTolerance = 1e-4;

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 27 buffers build\n");
            return 1;
        }

        std::vector<std::vector<float>> drawsPerBounce;
        std::vector<uint32_t> liveCountPerRun;
        std::vector<uint32_t> finalQueue;
        std::vector<float> envSamples;
        // The same run also produces check 28's evidence -- see below; one
        // dispatch, two independent verdicts.
        std::vector<float> neeSamples;
        if (!ctx.runWavefrontFusedLoopProbe(wf, kW, kH, kBounces, kAlbedo, kIterationSeed,
                                            drawsPerBounce, liveCountPerRun, finalQueue,
                                            &envSamples, &neeSamples)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 27 fused loop dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }
        wf.destroy(ctx.allocator());

        if (envSamples.size() != static_cast<std::size_t>(kCapacity) * ohao::diff::kEnvSampleFloats) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 27 env samples readback returned %zu "
                         "floats, expected %u\n",
                         envSamples.size(), kCapacity * ohao::diff::kEnvSampleFloats);
            return 1;
        }

        double maxCentreError = 0.0;
        double maxPdfRelError = 0.0;
        for (uint32_t i = 0; i < kCapacity; ++i) {
            const double dx = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 0u];
            const double dy = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 1u];
            const double dz = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 2u];
            const float pdf = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 3u];

            const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(len) || std::abs(len - 1.0) > 1e-4) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 27 path %u env sample direction "
                             "(%.9g,%.9g,%.9g) has length %.9g, expected a unit vector\n",
                             i, dx, dy, dz, len);
                return 1;
            }
            if (!(pdf > 0.0f) || !std::isfinite(pdf)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 27 path %u env sample returned pdf "
                             "%.9g -- the UV-uniform CDF wf.build seeds by default has strictly "
                             "positive probability everywhere\n",
                             i, static_cast<double>(pdf));
                return 1;
            }

            // Invert equirectPixelToDir exactly as check 25 does -- through
            // the same host-side function, so "exactly as" is enforced rather
            // than asserted in a comment.
            const OracleEnvTexel texel = oracleEnvTexelOf(dx, dy, dz, kEnvW, kEnvH);
            const double fx = texel.fx;
            const double fy = texel.fy;
            const int ix = texel.ix;
            const int iy = texel.iy;
            const double centreError = texel.centreError;
            if (!(centreError <= kCentreSlack)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 27 (WavefrontLoop::record's "
                             "envWidth/envHeight fill): path %u's env sample direction "
                             "(%.9g,%.9g,%.9g) inverts to (%.6f, %.6f) in %ux%u texel units, "
                             "%.3g away from the nearest texel centre (%d, %d). record() fills "
                             "ScatterPush's envWidth/envHeight from `buffers`, and this "
                             "environment is non-square (%u != %u) specifically so a transposed "
                             "fill cannot land on a valid texel by symmetry\n",
                             i, dx, dy, dz, fx, fy, kEnvW, kEnvH, centreError, ix, iy, kEnvW,
                             kEnvH);
                return 1;
            }
            maxCentreError = std::max(maxCentreError, centreError);

            const double thetaY =
                kPi * (static_cast<double>(iy) + 0.5) / static_cast<double>(kEnvH);
            const double expectedPdf = 1.0 / (2.0 * kPi * kPi * std::sin(thetaY));
            const double relErr =
                std::abs(static_cast<double>(pdf) - expectedPdf) / expectedPdf;
            if (!(relErr <= kPdfRelTolerance)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 27 (WavefrontLoop::record's "
                             "envWidth/envHeight fill): path %u's env pdf is %.9g, expected "
                             "%.9g (1/(2 pi^2 sin(theta)) for the UV-uniform CDF wf.build seeds "
                             "by default) -- relative error %.3g, tolerance %.3g\n",
                             i, static_cast<double>(pdf), expectedPdf, relErr, kPdfRelTolerance);
                return 1;
            }
            maxPdfRelError = std::max(maxPdfRelError, relErr);
        }
        std::printf("[diff_gpu_probe] OK: check 27 -- WavefrontLoop::record's OWN "
                    "envWidth/envHeight fill of ScatterPush (the production call site, not "
                    "runWavefrontScatterProbe's hand-filled one behind checks 24-26) reaches "
                    "wf_scatter.comp intact: all %u env samples from a %ux%u NON-SQUARE "
                    "environment invert to a texel centre (max error %.3g, slack %.3g) and match "
                    "the closed-form UV-uniform pdf 1/(2 pi^2 sin(theta)) to %.3g relative "
                    "(tolerance %.3g)\n",
                    kCapacity, kEnvW, kEnvH, maxCentreError, kCentreSlack, maxPdfRelError,
                    kPdfRelTolerance);

        // ------------------------------------------------------------------
        // 28. The shadow ray is actually traced (Stage 0b-2b Task 4).
        // ------------------------------------------------------------------
        //
        // Check 27's run is the CLOSED BOX, entered from its centre. A ray
        // leaving any point strictly inside a closed convex body through any
        // direction hits a face -- that is the same geometric fact the
        // fused-loop survival theorem rests on -- so EVERY shadow ray
        // wf_scatter.comp's next-event estimator traces here is occluded,
        // and every direct-lighting contribution in the binding-7 record
        // must be EXACTLY zero. Not "small": zero, bit for bit, because
        // diffMisTerm multiplies by the visibility term rather than
        // attenuating by it.
        //
        // This is the check that the shadow ray EXISTS. A visibility term
        // stuck at 1 -- the shape a missing or mis-flagged ray query takes
        // -- produces a perfectly plausible unoccluded estimate that checks
        // 29-31 (which run in an unoccluded scene, where the right answer IS
        // visibility 1) could never distinguish from the truth. The two
        // scenes are complementary on purpose: one where the answer must be
        // 1 everywhere and one where it must be 0 everywhere.
        //
        // NON-VACUITY. "All contributions are zero" is also what a zeroed
        // buffer looks like. So the recovered environment radiance is
        // asserted STRICTLY POSITIVE on the same samples: with radiance > 0,
        // a nonzero BSDF and directions above the horizon, the visibility
        // term is the only factor that can be zeroing the product. (The
        // default UV-uniform CDF this check runs against has integral 1 and
        // strictly positive density in every texel -- see
        // wavefront_buffers.cpp's seeding -- so a zero radiance here would
        // itself be a failure.)
        if (neeSamples.size() !=
            static_cast<std::size_t>(kCapacity) * ohao::diff::kNeeSampleFloats) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 28 NEE samples readback returned %zu "
                         "floats, expected %u\n",
                         neeSamples.size(),
                         kCapacity * ohao::diff::kNeeSampleFloats);
            return 1;
        }
        {
            uint32_t litSamples = 0;
            uint32_t surfaceSamples = 0;
            double minEnvRadiance = std::numeric_limits<double>::infinity();
            for (uint32_t i = 0; i < kCapacity; ++i) {
                const std::size_t b =
                    static_cast<std::size_t>(i) * ohao::diff::kNeeSampleFloats;
                if (neeSamples[b + ohao::diff::kNeeSlotSurfaceBranch] == 0.0f) continue;
                ++surfaceSamples;
                const float visLight = neeSamples[b + ohao::diff::kNeeSlotVisLight];
                const float visBsdf = neeSamples[b + ohao::diff::kNeeSlotVisBsdf];
                if (visLight != 0.0f || visBsdf != 0.0f) ++litSamples;
                minEnvRadiance = std::min(
                    minEnvRadiance,
                    static_cast<double>(neeSamples[b + ohao::diff::kNeeSlotEnvRadiance]));
                for (uint32_t c = 0; c < 3u; ++c) {
                    const float nee =
                        neeSamples[b + ohao::diff::kNeeSlotNeeUnweighted + c];
                    const float bsdf =
                        neeSamples[b + ohao::diff::kNeeSlotBsdfUnweighted + c];
                    if (nee != 0.0f || bsdf != 0.0f) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: check 28 -- path %u reported a "
                                     "nonzero direct-lighting contribution (nee %.9g, bsdf %.9g, "
                                     "channel %u) from INSIDE a closed box, where every shadow "
                                     "ray must be occluded. Its visibility terms are %.9g and "
                                     "%.9g\n",
                                     i, static_cast<double>(nee), static_cast<double>(bsdf), c,
                                     static_cast<double>(visLight),
                                     static_cast<double>(visBsdf));
                        return 1;
                    }
                }
            }
            if (surfaceSamples != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 28 -- only %u of %u paths took "
                             "wf_scatter.comp's surface branch. Every ray from the centre of a "
                             "closed box hits a face, so a miss here means the scene or the "
                             "trace is not what this check assumes and the zero contributions "
                             "below would be vacuous\n",
                             surfaceSamples, kCapacity);
                return 1;
            }
            if (litSamples != 0) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 28 -- %u of %u paths reported an "
                             "UNOCCLUDED shadow ray from inside a closed box\n",
                             litSamples, kCapacity);
                return 1;
            }
            if (!(minEnvRadiance > 0.0)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 28 -- the least of the recovered "
                             "environment radiances is %.9g. The zero contributions above would "
                             "then be zero because there is no light, not because the shadow ray "
                             "found geometry, and this check would prove nothing\n",
                             minEnvRadiance);
                return 1;
            }
            std::printf("[diff_gpu_probe] OK: check 28 -- every one of %u paths inside a CLOSED "
                        "box reports visibility exactly 0 for both the light sample and the BSDF "
                        "sample, and every direct-lighting contribution is exactly 0.0 (not "
                        "merely small), while the recovered environment radiance is strictly "
                        "positive (min %.6g) -- so the zeros are the shadow ray's doing and not "
                        "an absence of light\n",
                        kCapacity, minEnvRadiance);
        }
    }

    // ------------------------------------------------------------------
    // 29-31. NEXT-EVENT ESTIMATION AND MIS (Stage 0b-2b Task 4).
    // ------------------------------------------------------------------
    //
    // wf_scatter.comp now estimates the direct-lighting integral at each hit
    // point TWICE, by two different sampling strategies, and combines them
    // with the balance heuristic:
    //
    //   I = integral over the sphere of f(N,V,w) max(0, N.w) L(w) V(w) dw
    //
    //   strategy E ("next event"): w ~ p_E, the environment's
    //       sin(theta)-weighted luminance (env_sampling.glsl's sampleEnvMap
    //       -- the SAME call, on the SAME sample, that binding 6 records and
    //       check 24 chi-squares; see the routing tie in check 31)
    //   strategy B ("BSDF sampling"): w ~ p_B, diffBsdfSample's own mixture
    //
    // ------------------------------------------------------------------
    // THE ORACLE: strategy agreement, not a re-implemented integrand
    // ------------------------------------------------------------------
    //
    // Each strategy's single-sample estimator f*cos*L*V/p_own is unbiased
    // for I on its own. So are their MIS combination and, separately, each
    // half of it. Three estimators of one truth -- and the truth is never
    // computed here. A host oracle that re-evaluated f, L and V would share
    // whatever misconception the shader has about any of them; two
    // independent SAMPLERS of the same integral share nothing but the
    // integral itself.
    //
    // THE ONE THING THEIR EXPECTATIONS DO NOT SHARE, and it is derived, not
    // waved away. env_sampling.glsl returns TEXEL CENTRES (checks 25/27
    // pin that), so strategy E is a midpoint quadrature of the piecewise-
    // constant environment, while strategy B draws continuously and
    // integrates the same piecewise-constant map EXACTLY. Those two numbers
    // are not equal, and pretending they were would be a bound that passes
    // its own perturbation. (site/content/units/sampling/env-cdf.md states
    // the same thing about the RT pipeline's env-NEE block: sampleEnvMap
    // "returns equirectPixelToDir(x, y, W, H) unmodified -- the exact texel
    // centre, with no intra-texel jitter", so the strategy's expectation is
    // a midpoint quadrature "rather than the integral". The derivation
    // below is what that costs, in closed form, for this scene.)
    //
    // WHICH RADIANCE STRATEGY B MULTIPLIES IN IS PART OF THIS DERIVATION,
    // and it was wrong for one commit. The BSDF side has no radiance image
    // either; it recovers L by inverting a density. env_sampling.glsl's
    // pdfEnvMap is NOT the texel density off a texel centre -- it is that
    // density times sin(theta_centre)/sin(theta_query) -- so inverting IT
    // yields L*sin(theta_centre)/sin(theta_query), and the stray sin(theta)
    // then cancels against the solid-angle measure. The estimator that
    // results integrates a HALF-WIDTH midpoint rule, whose closed-form
    // ratio is sinc(pi/(2*envH)), not sinc(pi/envH). At envH = 64 the two
    // constants differ by 3.0e-4 relative -- 0.06 of one standard error of
    // D1 below, i.e. invisible -- while at envH = 8 they differ by 1.9%,
    // about 3.6 standard errors, and this check would have failed for a
    // correct-looking reason. wf_scatter.comp now calls pdfEnvMapTexel for
    // the radiance and keeps pdfEnvMap for the MIS weight (where the sin
    // ratio cancels out of the balance heuristic anyway), so Y_i is
    // albedo * L_texel -- BOUNDED, which also trims the heavy right tail
    // the z-score discussion below assumes away. Check 31 asserts the
    // recovered value against the environment image per sample, so the
    // choice is measured rather than derived-and-hoped.
    //
    // The scene is chosen so the gap has a CLOSED FORM. The surface normal
    // is +Y, the equirectangular pole, so the cosine factor is
    // max(0, cos theta): a function of the ROW alone, with no azimuthal
    // dependence, and (for even envH) a horizon that falls exactly on a row
    // boundary rather than cutting through a row. Over one row
    // [theta1, theta2] of width dtheta, with L constant on the row,
    //
    //   exact   = L dphi * integral of cos(theta) sin(theta) dtheta
    //           = L dphi * cos(theta_c) sin(theta_c) sin(dtheta)
    //   midpoint= L dphi * dtheta cos(theta_c) sin(theta_c)
    //
    // using cos(theta)sin(theta) = sin(2 theta)/2 and
    // cos(2 theta_1) - cos(2 theta_2) = 2 sin(2 theta_c) sin(dtheta). Their
    // ratio is sin(dtheta)/dtheta -- INDEPENDENT of the row, of L, and of
    // the map's azimuthal structure -- so summing over every texel,
    //
    //   E[strategy B] = kappa * E[strategy E],   kappa = sinc(pi/envH).
    //
    // At envH = 64 that is 0.99959845, a 4.02e-4 relative offset. Nothing is
    // fitted: kappa is a trigonometric identity, and it is the ONLY host
    // input the agreement check takes.
    //
    // ------------------------------------------------------------------
    // 29. The bound, derived
    // ------------------------------------------------------------------
    //
    // Every path shades the same normal (+Y) with the same Lambertian BSDF
    // (specularWeight 0, metallic 0 -- f = albedo/pi, independent of the
    // view direction, so the per-pixel view variation does not make the
    // samples non-identically-distributed) and the same unoccluded upper
    // hemisphere. The N = 49152 per-path records are therefore i.i.d. draws
    // of one scalar estimator each, and the comparisons are PAIRED per
    // sample:
    //
    //   D1_i = Y_i - kappa X_i        E[D1] = 0 exactly
    //                                 (exactly, for any even envH, ONLY
    //                                  because Y multiplies in the texel
    //                                  radiance -- see above)
    //   D2_i = Z_i - Y_i              E[D2] = delta   (below)
    //   D3_i = Z_i - X_i              E[D3] = delta - (1-kappa) E[X]
    //
    // with X the next-event-only estimator, Y the BSDF-only one and Z their
    // MIS combination. The bound on each is z * s_D / sqrt(N) using THAT
    // difference's own sample standard deviation -- exact whatever the
    // correlation between X_i and Y_i, which is why the comparison is
    // paired rather than a difference of two independent means.
    //
    // delta is the MIS estimator's own share of the same midpoint-vs-exact
    // gap: its next-event half is the same midpoint sum damped by
    // w_E in [0,1], its BSDF half is exact, so
    // delta = Mid[w_E g] - Exact[w_E g]. It is allowed for at
    // (1 - kappa) * mean(X) -- the UNDAMPED gap, i.e. the same integral with
    // w_E replaced by 1. Measured by numerical quadrature at four
    // resolutions (envH = 8, 16, 32, 64) the true ratio delta/((1-kappa)A)
    // is 0.70, 0.77, 0.80, 0.81 -- always below 1, so the allowance is
    // conservative by about 20%. At envH = 64 the allowance is 4.02e-4
    // relative (0.001571 absolute), against a D2 standard error of 0.014480
    // and a total D2 bound of 0.088453: the systematic term is a TENTH OF
    // THE STANDARD ERROR and under 2% of the bound. (It is not "a tenth of
    // the bound"; that is what this comment used to say, and the report
    // said 2%. The bound is what z*se makes it, and the systematic is a
    // small correction on top.)
    //
    // z = 6. The nominal two-sided Gaussian rate is 2e-9; the HONEST rate is
    // Berry-Esseen's, and for this estimator's third absolute moment that
    // bound is 2.7e-3 whatever z is, so z buys margin against the
    // perturbation rather than against the tail. The empirical justification
    // is the one that matters: over 60 independent replications of exactly
    // these estimators at exactly this N, simulated on the host before any
    // of this was written, the largest |D| observed was 2.5 standard errors
    // -- comfortably under half the threshold -- while the Step 5
    // perturbation (inverting ONE misBalanceHeuristic call's arguments)
    // pushes D2 to 1.114377 against a bound of 0.088453 and D3 to 1.150805
    // against 0.048983. The bound rejects the perturbation by 12.6x on D2
    // and 23.4x on D3, and admits the truth by a factor of 2.4. (Neither
    // factor is 26; that number was in this comment and in the report and
    // matched neither measurement.)
    //
    // NON-VACUITY. All three estimators are asserted strictly positive, and
    // the count of samples that contributed anything is printed. An
    // unwritten (all-zero) sink would make all three agree perfectly at 0,
    // which is exactly the failure mode a pure agreement check cannot see.
    {
        // 256x192 = 49152 paths, ONE scatter dispatch. Every path is an
        // independent RNG stream (streams are keyed by pixel index), so the
        // sample count comes from the image rather than from repeating
        // dispatches -- which is also the configuration the integrator
        // actually runs in.
        constexpr uint32_t kW = 256;
        constexpr uint32_t kH = 192;
        constexpr uint32_t kCapacity = kW * kH;  // 49152
        constexpr uint32_t kIterationSeed = 4040404u;
        constexpr float kAlbedo = 1.0f;

        // envH must be EVEN for the horizon to land on a row boundary (see
        // the kappa derivation), and 64 is where the derived systematic
        // offset drops an order of magnitude below the Monte Carlo error.
        constexpr uint32_t kEnvW = 128;
        constexpr uint32_t kEnvH = 64;
        static_assert(kEnvH % 2u == 0u,
                     "the closed-form midpoint/exact ratio needs the +Y horizon to fall on a "
                     "row boundary, which requires an even envH");
        // RESOLUTION GUARD. Evenness is what the kappa IDENTITY needs and
        // it is not what this CHECK needs: three separate things below stop
        // holding outside a band of envH, none of them visible at 64, and
        // the failure mode of each is a spurious FAIL rather than a silent
        // pass. Guarding the band is cheaper than rediscovering them.
        //
        //   * UPPER BOUND, 128. The smallest |cos(theta)| any row CENTRE
        //     takes is sin(pi/(2*envH)) -- 0.0245 at envH = 64, 0.0123 at
        //     128. Two things are compared against a 1e-4 floor at that
        //     scale: check 31 compares diffBsdfEval's pdf at the light
        //     sample against max(0, d.y)/pi UNCONDITIONALLY (valid only
        //     while no texel centre reaches bsdf.glsl's DIFF_BSDF_MIN_COS
        //     grazing branch), and env_sampling.glsl clamps its own
        //     sin(theta) at 1e-4 (which must never engage at a texel
        //     centre, or pdfEnvMapTexel stops being the texel density and
        //     check 31's radiance tie starts failing near the poles). At
        //     128 both keep two orders of magnitude of margin; past it the
        //     margin erodes and these become conditional checks that this
        //     code does not make conditional.
        //   * LOWER BOUND, 8. D2/D3 allow for delta at (1 - kappa)*mean(X).
        //     That allowance is justified ONLY by numerical quadrature at
        //     envH = 8, 16, 32, 64 (ratios 0.70, 0.77, 0.80, 0.81), not by
        //     proof. Below 8 nothing has measured it, and (1 - kappa) grows
        //     like envH^-2, so it stops being a small correction and starts
        //     being the bound.
        //
        // Note what this guard is NOT for: kappa itself is exact for every
        // even envH now that strategy B recovers texel radiance. Before
        // that fix the correct constant was sinc(pi/(2*envH)) and this
        // check passed at envH = 64 only because the two agree to 3.0e-4
        // there -- 0.06 sigma. At envH = 8 the same code would have failed
        // at 3.6 sigma. That is precisely the class of latency an evenness
        // assert cannot see, which is why this one names its band.
        static_assert(kEnvH >= 8u && kEnvH <= 128u,
                     "checks 29-31 are derived for env heights in [8, 128]: below 8 the "
                     "delta <= (1-kappa)*mean(X) allowance is outside every resolution it was "
                     "measured at, and above 128 the coarsest row centre's cosine approaches "
                     "the 1e-4 grazing/sin floors that check 31 compares against "
                     "unconditionally");
        static_assert(kEnvW >= kEnvH,
                     "the azimuthal resolution must not be coarser than the polar one: the "
                     "kappa derivation integrates each row exactly in phi and only quadratures "
                     "in theta");
        constexpr uint32_t kEnvTexels = kEnvW * kEnvH;

        // A floor at y = 0 seen from directly above. The camera basis is any
        // orthonormal triple with forward = -Y; the footprint at this fov
        // and height is under +/-0.6 in x and z, so a half-size of 2 leaves
        // the quad's edges nowhere near a primary ray.
        constexpr float kFloorY = 0.0f;
        constexpr float kFloorHalfSize = 2.0f;
        constexpr float kCameraHeight = 2.0f;
        constexpr float kTanHalfFov = 0.2f;

        constexpr double kPi = 3.14159265358979323846;
        constexpr double kZ = 6.0;

        // --- The environment. Strictly positive everywhere (so every
        // returned density is positive and every recovered radiance is
        // finite), asymmetric in both axes, brighter towards the +Y pole so
        // that most of the environment's energy is in the hemisphere the
        // floor can actually see, and with a bright block ALSO in that
        // hemisphere so the two strategies genuinely disagree about where to
        // put their samples. A block in the lower hemisphere would make
        // next-event estimation spend most of its samples on directions the
        // cosine kills, which weakens the check for no reason. ---
        std::vector<float> envRgba(static_cast<std::size_t>(kEnvTexels) * 4u, 0.0f);
        std::vector<double> envLum(kEnvTexels, 0.0);
        constexpr double kBlockBoost = 8.0;
        const uint32_t kBlockY0 = static_cast<uint32_t>(kEnvH * 0.15);
        const uint32_t kBlockX0 = static_cast<uint32_t>(kEnvW * 0.60);
        const uint32_t kBlockH = kEnvH / 8u;
        const uint32_t kBlockW = kEnvW / 8u;
        for (uint32_t y = 0; y < kEnvH; ++y) {
            for (uint32_t x = 0; x < kEnvW; ++x) {
                double L = 1.0 + 0.10 * (static_cast<double>(x) * 16.0 / kEnvW) +
                           0.30 * (static_cast<double>(kEnvH - 1u - y) * 8.0 / kEnvH);
                if (y >= kBlockY0 && y < kBlockY0 + kBlockH && x >= kBlockX0 &&
                    x < kBlockX0 + kBlockW) {
                    L += kBlockBoost;
                }
                const std::size_t k = static_cast<std::size_t>(y) * kEnvW + x;
                envLum[k] = L;
                envRgba[k * 4u + 0u] = static_cast<float>(L);
                envRgba[k * 4u + 1u] = static_cast<float>(L);
                envRgba[k * 4u + 2u] = static_cast<float>(L);
                envRgba[k * 4u + 3u] = 1.0f;
            }
        }
        static_assert(kBlockBoost > 1.0,
                     "the bright block is what makes the two sampling densities disagree; "
                     "without it MIS has nothing to combine");

        ohao::EnvCDF envCdf;
        envCdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
        if (!envCdf.valid()) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: checks 29-31: EnvCDF::build produced no "
                                  "CDF for a %ux%u strictly-positive environment\n",
                         kEnvW, kEnvH);
            return 1;
        }

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: checks 29-31 buffers build\n");
            return 1;
        }
        if (!wf.uploadEnvironment(envCdf.marginalSpan(), envCdf.conditionalSpan(),
                                  envCdf.integral())) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: checks 29-31 env CDF upload rejected\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        ohao::diff::WavefrontGenerateCamera camera;
        camera.origin[0] = 0.0f;
        camera.origin[1] = kCameraHeight;
        camera.origin[2] = 0.0f;
        camera.forward[0] = 0.0f;
        camera.forward[1] = -1.0f;
        camera.forward[2] = 0.0f;
        camera.right[0] = 1.0f;
        camera.right[1] = 0.0f;
        camera.right[2] = 0.0f;
        camera.up[0] = 0.0f;
        camera.up[1] = 0.0f;
        camera.up[2] = -1.0f;
        camera.tanHalfFov = kTanHalfFov;

        std::vector<uint32_t> queue0;
        if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: checks 29-31 setup: wf_generate\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        // The floor, as ONE triangle soup handed to BOTH the primary trace
        // and the shadow rays. Passing the same span to both is the point:
        // a shadow ray tested against different geometry than the primary
        // ray is a visibility term that means nothing.
        const float e = kFloorHalfSize;
        const std::array<float, 12> floorPositions = {
            -e, kFloorY, -e,
             e, kFloorY, -e,
             e, kFloorY,  e,
            -e, kFloorY,  e,
        };
        const std::array<uint32_t, 6> floorIndices = {0, 1, 2, 0, 2, 3};

        std::vector<uint32_t> queue1;
        if (!ctx.runWavefrontIntersectOnGeometry(wf, std::span<const float>(floorPositions),
                                                 std::span<const uint32_t>(floorIndices),
                                                 queue1)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: checks 29-31 setup: wf_intersect\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        ohao::diff::WavefrontShadowScene shadowScene;
        shadowScene.positions = std::span<const float>(floorPositions);
        shadowScene.indices = std::span<const uint32_t>(floorIndices);

        std::vector<uint32_t> outQueue;
        std::vector<float> outDraws;
        std::vector<float> envSamples;
        std::vector<float> neeSamples;
        if (!ctx.runWavefrontScatterProbe(
                wf, /*srcQueueBase=*/kCapacity,
                ohao::diff::WavefrontBuffers::kNextCountSlot, /*dstQueueBase=*/0u,
                ohao::diff::WavefrontBuffers::kCurrentCountSlot, kAlbedo, kIterationSeed, outQueue,
                outDraws, /*material=*/{}, &envSamples, shadowScene, &neeSamples)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: checks 29-31 scatter dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }
        const float envIntegral = wf.envIntegral();
        wf.destroy(ctx.allocator());

        if (envSamples.size() != static_cast<std::size_t>(kCapacity) * ohao::diff::kEnvSampleFloats ||
            neeSamples.size() !=
                static_cast<std::size_t>(kCapacity) * ohao::diff::kNeeSampleFloats) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: checks 29-31 readback returned %zu env floats "
                         "and %zu NEE floats, expected %u and %u\n",
                         envSamples.size(), neeSamples.size(), kCapacity * ohao::diff::kEnvSampleFloats,
                         kCapacity * ohao::diff::kNeeSampleFloats);
            return 1;
        }

        // --- Host-side accumulation. Deliberately NOT the GPU's: Task 4's
        // estimators are formed here from per-sample readback, which makes
        // the accumulator an oracle independent of whatever Task 5 does to
        // film accumulation, and keeps these verdicts valid across that
        // change.
        double sumX = 0.0, sumY = 0.0, sumZ = 0.0;
        double sumD1 = 0.0, sumD2 = 0.0, sumD3 = 0.0;
        double sumD1Sq = 0.0, sumD2Sq = 0.0, sumD3Sq = 0.0;
        double sumXSq = 0.0, sumYSq = 0.0, sumZSq = 0.0;
        uint32_t contributing = 0;
        uint32_t surfaceSamples = 0;
        // 29's kappa: the closed-form midpoint/exact ratio derived above.
        const double kappa = std::sin(kPi / kEnvH) / (kPi / kEnvH);

        // 30's accumulators.
        double worstPartitionError = 0.0;
        uint32_t worstPartitionSample = 0;

        // 31's accumulators.
        double worstRadianceRelError = 0.0;
        double worstBsdfRadianceRelError = 0.0;
        double worstPdfBsdfAbsError = 0.0;
        double worstNeeTieRelError = 0.0;
        double worstPdfEnvNormalisedError = 0.0;
        double worstPdfEnvRelError = 0.0;
        uint32_t pdfEnvAtBsdfRejected = 0;
        uint32_t pdfEnvAtBsdfSkipped = 0;
        double minPdfEnvAtBsdf = std::numeric_limits<double>::infinity();

        // The host's own texel densities, computed from the luminance image
        // and EnvCDF's integral and nothing the GPU produced:
        //
        //     p(x,y) = L(x,y) * W * H / (integral * 2 pi^2)
        //
        // WHAT pdfEnvMap ACTUALLY RETURNS, which is not that. Its texel mass
        // condDiff*margDiff carries sin(theta) of the texel CENTRE (the CDF
        // is built with that weight), and it then divides by sin(theta) of
        // the QUERY direction. So
        //
        //     pdfEnvMap(w) = p(x,y) * sin(theta_centre) / sin(theta_w),
        //
        // equal to p only when w IS the texel centre -- which is exactly
        // where sampleEnvMap puts every one of its samples, so the two agree
        // on the environment strategy's entire support and the MIS weights
        // still partition unity (check 30 measures that directly). Off a
        // centre the factor can reach several: the smallest sin(theta_w) a
        // cosine-weighted sample about +Y reaches in 49152 draws is around
        // 0.005, against a first-row centre at sin(theta) = 0.0245.
        //
        // NOT A NEW FINDING, and this file should not imply it is.
        // site/content/units/sampling/env-cdf.md already says the two
        // "differ by sin(theta_y)/sin(theta(w))", that it is "worst at the
        // poles", and works a 2048-high map to "a factor of 7.7 between the
        // two sides of one weight". What IS new here is that pdfEnvMap has
        // a caller under test at all -- check 24 deliberately writes its own
        // inverse rather than calling it -- and that the factor is asserted
        // per sample rather than described.
        //
        // This check asserts the factor rather than ignoring it. Asserting
        // p alone was tried first and rejected 1019 of 49152 samples; a
        // check written to the convenient formula instead would have been
        // the weaker one. Note the direction of the lesson: the ratio is
        // harmless in a WEIGHT (it cancels) and a bias in a RADIANCE (it
        // does not), which is why wf_scatter.comp now reads BOTH densities
        // at the BSDF direction and why the radiance it recovered is
        // asserted separately below.
        const double kTwoPiSq = 2.0 * kPi * kPi;
        std::vector<double> hostTexelPdf(kEnvTexels, 0.0);
        for (uint32_t k = 0; k < kEnvTexels; ++k) {
            hostTexelPdf[k] = envLum[k] * static_cast<double>(kEnvW) *
                              static_cast<double>(kEnvH) /
                              (static_cast<double>(envIntegral) * kTwoPiSq);
        }

        // Float32 budget for the per-sample ties: the recovered radiance is
        // two CDF differences (each up to 2^-23 absolute, ~7e-6 relative on
        // the least likely texel here), a W*H product, a multiply by a
        // float32 integral and a divide -- a few parts in 1e-5. Asserted at
        // 1e-3, two orders of magnitude of slack, and the observed maxima
        // are printed so a value creeping towards the bound is visible.
        constexpr double kTieRelTolerance = 1e-3;
        // ABSOLUTE, and separate, because the quantity it bounds is not a
        // ratio. `worstPdfBsdfAbsError` is |pdfBsdfAtLight - max(0,d.y)/pi|,
        // a difference of two numbers in [0, 1/pi]; comparing it against a
        // RELATIVE constant (which is what this used to do, reusing
        // kTieRelTolerance) is a category error that happened to be
        // harmless only because the observed value is 2.3e-8. Derived
        // instead: the shader's dot(vec3(0,1,0), envDir) is exactly d.y,
        // both sides then divide the SAME float32 by pi, so the difference
        // is a couple of ulp of 1/pi = 0.3183 -- one ulp there is 3.0e-8.
        // 1e-6 is about 32 ulp: loose enough not to be a hardware lottery,
        // tight enough that a genuinely different direction (the failure it
        // exists to catch) is nowhere near it.
        constexpr double kPdfBsdfAbsTolerance = 1e-6;
        // The MIS partition is two float32 divisions by the same
        // denominator; their sum is 1 to within a couple of ulp.
        constexpr double kPartitionTolerance = 1e-6;

        for (uint32_t i = 0; i < kCapacity; ++i) {
            const std::size_t nb = static_cast<std::size_t>(i) * ohao::diff::kNeeSampleFloats;
            if (neeSamples[nb + ohao::diff::kNeeSlotSurfaceBranch] == 0.0f) continue;
            ++surfaceSamples;

            const double X = neeSamples[nb + ohao::diff::kNeeSlotNeeUnweighted];
            const double Y = neeSamples[nb + ohao::diff::kNeeSlotBsdfUnweighted];
            const double wEnvAtLight = neeSamples[nb + ohao::diff::kNeeSlotWEnvAtLight];
            const double wBsdfAtLight = neeSamples[nb + ohao::diff::kNeeSlotWBsdfAtLight];
            const double wBsdfAtBsdf = neeSamples[nb + ohao::diff::kNeeSlotWBsdfAtBsdf];
            const double wEnvAtBsdf = neeSamples[nb + ohao::diff::kNeeSlotWEnvAtBsdf];
            const double Z = wEnvAtLight * X + wBsdfAtBsdf * Y;

            if (!std::isfinite(X) || !std::isfinite(Y) || X < 0.0 || Y < 0.0) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 29 -- path %u reported a non-finite or "
                             "negative estimator (nee %.9g, bsdf %.9g). A radiance estimator is a "
                             "non-negative real; anything else is a division by a density the "
                             "direction was not drawn from\n",
                             i, X, Y);
                return 1;
            }
            if (X > 0.0 || Y > 0.0) ++contributing;

            sumX += X; sumY += Y; sumZ += Z;
            sumXSq += X * X; sumYSq += Y * Y; sumZSq += Z * Z;
            const double d1 = Y - kappa * X;
            const double d2 = Z - Y;
            const double d3 = Z - X;
            sumD1 += d1; sumD2 += d2; sumD3 += d3;
            sumD1Sq += d1 * d1; sumD2Sq += d2 * d2; sumD3Sq += d3 * d3;

            // --- 30. Both MIS partitions, per sample.
            const double e1 = std::abs(wEnvAtLight + wBsdfAtLight - 1.0);
            const double e2 = std::abs(wBsdfAtBsdf + wEnvAtBsdf - 1.0);
            if (std::max(e1, e2) > worstPartitionError) {
                worstPartitionError = std::max(e1, e2);
                worstPartitionSample = i;
            }

            // --- 31. Ties back to what binding 6 recorded.
            const double dx = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 0u];
            const double dy = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 1u];
            const double dz = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 2u];
            // Same host-side binning as checks 25/27 and the parity
            // reference -- one function, not a fourth transcription.
            const std::size_t texel = oracleEnvTexelOf(dx, dy, dz, kEnvW, kEnvH).index;

            const double envRadiance = neeSamples[nb + ohao::diff::kNeeSlotEnvRadiance];
            const double radRelError = std::abs(envRadiance / envLum[texel] - 1.0);
            worstRadianceRelError = std::max(worstRadianceRelError, radRelError);

            // The surface normal is +Y, so the density diffBsdfSample would
            // have drawn the LIGHT sample's direction with is max(0, d.y)/pi
            // -- computable from binding 6's direction and nothing else.
            // diffBsdfEval reports 0 below its 1e-4 grazing floor; the
            // coarsest row of this map sits at cos(theta) = 0.0245, two
            // orders of magnitude above it, so that branch is unreachable
            // here and the comparison is unconditional.
            const double expectedPdfBsdf = std::max(0.0, dy) / kPi;
            const double pdfBsdfAtLight = neeSamples[nb + ohao::diff::kNeeSlotPdfBsdfAtLight];
            worstPdfBsdfAbsError =
                std::max(worstPdfBsdfAbsError, std::abs(pdfBsdfAtLight - expectedPdfBsdf));

            // And the estimator itself, recomputed from binding 6's
            // (direction, pdf) pair: f = albedo/pi, cos = max(0, d.y),
            // L = the map's own luminance at the texel that direction lands
            // in, V = 1 (nothing occludes the upper hemisphere above a
            // planar floor). If next-event estimation had drawn its own
            // direction rather than consuming the one binding 6 records,
            // this would not match.
            const double envPdf = envSamples[static_cast<std::size_t>(i) * ohao::diff::kEnvSampleFloats + 3u];
            const double expectedNee = (envPdf > 0.0)
                                           ? (static_cast<double>(kAlbedo) / kPi) *
                                                 std::max(0.0, dy) * envLum[texel] / envPdf
                                           : 0.0;
            if (expectedNee > 0.0) {
                worstNeeTieRelError =
                    std::max(worstNeeTieRelError, std::abs(X / expectedNee - 1.0));
            } else if (X != 0.0) {
                worstNeeTieRelError = std::max(worstNeeTieRelError, 1.0);
            }

            // --- pdfEnvMap, at the BSDF sample's own direction.
            const double pdfEnvAtBsdf = neeSamples[nb + ohao::diff::kNeeSlotPdfEnvAtBsdf];
            minPdfEnvAtBsdf = std::min(minPdfEnvAtBsdf, pdfEnvAtBsdf);
            const double bx = neeSamples[nb + ohao::diff::kNeeSlotBsdfDir + 0u];
            const double by = neeSamples[nb + ohao::diff::kNeeSlotBsdfDir + 1u];
            const double bz = neeSamples[nb + ohao::diff::kNeeSlotBsdfDir + 2u];
            const OracleEnvTexel bTexelInfo = oracleEnvTexelOf(bx, by, bz, kEnvW, kEnvH);
            const double bTheta = bTexelInfo.theta;
            const double bu = bTexelInfo.fx;
            const double bv = bTexelInfo.fy;
            // A direction landing within a thousandth of a texel of a
            // boundary may be binned into a different texel by the shader's
            // float32 arithmetic than by this double one, which would make
            // L (and therefore the expected density) come from the wrong
            // texel. Those samples are counted and excluded rather than
            // asserted; the count is reported so a scene that started
            // producing many of them would be visible.
            const double buFrac = std::abs(bu - std::floor(bu) - 0.5);
            const double bvFrac = std::abs(bv - std::floor(bv) - 0.5);
            if (buFrac > 0.5 - 1e-3 || bvFrac > 0.5 - 1e-3) {
                ++pdfEnvAtBsdfSkipped;
                continue;
            }
            const int biy = bTexelInfo.iy;
            const std::size_t bTexel = bTexelInfo.index;
            const double bThetaCentre = kPi * (static_cast<double>(biy) + 0.5) / kEnvH;
            const double bSinQuery = std::max(std::sin(bTheta), 1e-4);
            const double expectedPdfEnv =
                hostTexelPdf[bTexel] * std::sin(bThetaCentre) / bSinQuery;

            // THE RADIANCE STRATEGY B ACTUALLY MULTIPLIED IN, against the
            // environment image at the texel its own direction lands in.
            //
            // This is the check that separates the two candidate recoveries
            // and it is EXACT per sample, not statistical. Inverting
            // pdfEnvMap's answer (which is what this shader did for one
            // commit) yields L * sin(theta_centre)/sin(theta_query): equal
            // to L at a centre, and up to ~5x L in the tail of a
            // cosine-weighted draw about +Y, where the smallest
            // sin(theta_query) reached in 49152 draws is around 0.005
            // against a first-row centre at 0.0245. Averaged over an
            // estimator that also divides by the same measure it is a
            // 3.0e-4 shift in E[Y] at envH = 64 -- 0.06 of one standard
            // error, which is why check 29 could not see it -- but it is a
            // per-sample energy error of up to a factor of five, which is a
            // firefly source the moment Task 5 accumulates these
            // contributions into a film. Inverting pdfEnvMapTexel instead
            // yields L exactly, and the comparison below is then the same
            // float32 CDF round trip the light-sample radiance tie above
            // makes, at the same tolerance.
            const double bsdfRadiance = neeSamples[nb + ohao::diff::kNeeSlotBsdfRadiance];
            worstBsdfRadianceRelError = std::max(worstBsdfRadianceRelError,
                                                 std::abs(bsdfRadiance / envLum[bTexel] - 1.0));
            const double pdfEnvRelError = std::abs(pdfEnvAtBsdf / expectedPdfEnv - 1.0);
            worstPdfEnvRelError = std::max(worstPdfEnvRelError, pdfEnvRelError);

            // TOLERANCE, derived from Vulkan's own precision guarantees
            // rather than from what this machine happens to produce. The
            // quantity divides by a float32 sin(acos(y)):
            //
            //  * sin() is guaranteed only to ABSOLUTE error 2^-11 inside
            //    [-pi, pi] (Vulkan, Precision and Operation of SPIR-V
            //    Instructions), so its RELATIVE error is 2^-11 / sin(theta)
            //    -- unbounded as the direction approaches the pole, which is
            //    why this term is per-sample and not a constant.
            //  * acos is specified as inherited from atan2, 4096 ULP, so
            //    theta carries relative error 4096 * 2^-24; sin's own
            //    argument error contributes cos(theta) * theta * that,
            //    again divided by sin(theta) to become relative.
            //  * The two CDF differences contribute ~2e-5 relative (the
            //    same float32 cancellation checks 25/26 budget), and the
            //    remaining float32 products/divisions a few ulp.
            //
            // The bound is loose near the pole and tight (7e-4) at the
            // sin(theta) ~ 0.7 where most cosine-weighted samples land. Both
            // the worst raw relative error and the worst error as a FRACTION
            // of its own sample's bound are printed, so hardware that beats
            // the specification by orders of magnitude -- as it does -- is
            // visible rather than hidden behind the guarantee.
            const double allowed =
                2.0e-5 + (4.8828125e-4 + std::cos(bTheta) * bTheta * 4096.0 * 5.9604645e-8) /
                             bSinQuery;
            worstPdfEnvNormalisedError =
                std::max(worstPdfEnvNormalisedError, pdfEnvRelError / allowed);
            if (!(pdfEnvRelError <= allowed)) ++pdfEnvAtBsdfRejected;
        }

        if (surfaceSamples != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: checks 29-31 -- only %u of %u paths took the "
                         "surface branch. Every primary ray from a camera above the floor hits "
                         "it, so the estimators below would be averaging over a set this check "
                         "did not choose\n",
                         surfaceSamples, kCapacity);
            return 1;
        }

        const double n = static_cast<double>(kCapacity);
        const double meanX = sumX / n;
        const double meanY = sumY / n;
        const double meanZ = sumZ / n;
        auto stdErr = [n](double sum, double sumSq) {
            const double mean = sum / n;
            const double var = std::max(0.0, (sumSq - n * mean * mean) / (n - 1.0));
            return std::sqrt(var / n);
        };
        const double seD1 = stdErr(sumD1, sumD1Sq);
        const double seD2 = stdErr(sumD2, sumD2Sq);
        const double seD3 = stdErr(sumD3, sumD3Sq);
        const double meanD1 = sumD1 / n;
        const double meanD2 = sumD2 / n;
        const double meanD3 = sumD3 / n;
        const double systematic = (1.0 - kappa) * meanX;

        bool task4Failed = false;

        // --- 29. Strategy agreement. ---
        if (!(meanX > 0.0) || !(meanY > 0.0) || !(meanZ > 0.0) || contributing == 0) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 29 -- the three estimators average "
                         "%.9g (next-event), %.9g (BSDF) and %.9g (MIS) over %u contributing "
                         "samples. Three estimators that are all ZERO agree perfectly and prove "
                         "nothing; this scene is an unoccluded floor under a strictly positive "
                         "environment, so every one of them must be positive\n",
                         meanX, meanY, meanZ, contributing);
            task4Failed = true;
        } else if (!(std::abs(meanD1) <= kZ * seD1) ||
                   !(std::abs(meanD2) <= kZ * seD2 + systematic) ||
                   !(std::abs(meanD3) <= kZ * seD3 + 2.0 * systematic)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 29 -- the three estimators of ONE "
                         "direct-lighting integral do not agree. next-event %.6f, BSDF %.6f, MIS "
                         "%.6f over %u samples.\n"
                         "  D1 = bsdf - kappa*nee : %+.6f, bound %.6f (%.2f z, z = %.1f, se "
                         "%.6f)\n"
                         "  D2 = mis  - bsdf      : %+.6f, bound %.6f (se %.6f + systematic "
                         "%.6f)\n"
                         "  D3 = mis  - nee       : %+.6f, bound %.6f (se %.6f + 2*systematic)\n"
                         "  kappa = sinc(pi/%u) = %.9f is the CLOSED-FORM midpoint/exact ratio "
                         "between the two strategies' expectations, not a fitted correction\n",
                         meanX, meanY, meanZ, kCapacity, meanD1, kZ * seD1,
                         seD1 > 0.0 ? std::abs(meanD1) / seD1 : 0.0, kZ, seD1, meanD2,
                         kZ * seD2 + systematic, seD2, systematic, meanD3,
                         kZ * seD3 + 2.0 * systematic, seD3, kEnvH, kappa);
            task4Failed = true;
        } else {
            std::printf("[diff_gpu_probe] OK: check 29 -- next-event-only (%.6f), BSDF-only "
                        "(%.6f) and their MIS combination (%.6f) estimate ONE direct-lighting "
                        "integral from %u samples and agree: |bsdf - kappa*nee| = %.6f at %.2f "
                        "z (bound %.6f, z = %.1f), |mis - bsdf| = %.6f at %.2f z (bound %.6f, "
                        "incl. derived systematic %.6f), |mis - nee| = %.6f at %.2f z (bound "
                        "%.6f). kappa = sinc(pi/%u) = %.9f, the closed-form midpoint/exact ratio. "
                        "%u of %u samples contributed\n",
                        meanX, meanY, meanZ, kCapacity, std::abs(meanD1),
                        seD1 > 0.0 ? std::abs(meanD1) / seD1 : 0.0, kZ * seD1, kZ,
                        std::abs(meanD2), seD2 > 0.0 ? std::abs(meanD2) / seD2 : 0.0,
                        kZ * seD2 + systematic, systematic, std::abs(meanD3),
                        seD3 > 0.0 ? std::abs(meanD3) / seD3 : 0.0, kZ * seD3 + 2.0 * systematic,
                        kEnvH, kappa, contributing, kCapacity);
        }

        // --- 30. The MIS partition, per sample. ---
        //
        // Exact and cheap where check 29 is statistical: at ONE direction
        // the two strategies' balance-heuristic weights are p_A/(p_A+p_B)
        // and p_B/(p_A+p_B), so they sum to 1 identically.
        //
        // WHAT IT CAN AND CANNOT SEE, stated precisely, because "an entire
        // class of weighting bug" is what this comment used to claim and it
        // is more than the identity supports. nee.glsl's diffMisTerm forms
        // wOwn = misBalance(a, b) and wOther = misBalance(b, a) from the
        // SAME pair (a, b), so wOwn + wOther = (a+b)/max(a+b, 1e-6) is
        // identically 1 in exact arithmetic WHATEVER a and b are. This
        // check is therefore a WITHIN-CALL identity, and it sees exactly
        // three things:
        //
        //   1. one of the two calls having its arguments swapped (Step 5's
        //      perturbation -- both weights then come from the same
        //      ordering and no longer complement),
        //   2. a balance heuristic on one side and a power heuristic on the
        //      other,
        //   3. the 1e-6 floor engaging, i.e. a sample where both densities
        //      are ~0 and the "weights" are not a partition at all.
        //
        // It CANNOT see the bug class that actually biases MIS: the two
        // strategies evaluating DIFFERENT environment densities at the same
        // direction. Both weights would still be formed from one pair and
        // would still sum to 1. That failure is check 29's to catch,
        // statistically, and check 31's to catch per sample.
        if (!(worstPartitionError <= kPartitionTolerance)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 30 -- the two MIS weights at one sampled "
                         "direction do not sum to 1: worst deviation %.3g at path %u (tolerance "
                         "%.3g, which is a couple of ulp of two float32 divisions by the same "
                         "denominator). The estimator is unbiased only if the weights partition "
                         "unity POINTWISE\n",
                         worstPartitionError, worstPartitionSample, kPartitionTolerance);
            task4Failed = true;
        } else {
            std::printf("[diff_gpu_probe] OK: check 30 -- for all %u samples, BOTH MIS partitions "
                        "sum to exactly 1 (w_env + w_bsdf at the light sample, and again at the "
                        "BSDF sample): worst deviation %.3g, tolerance %.3g\n",
                        kCapacity, worstPartitionError, kPartitionTolerance);
        }

        // --- 31. The three things nothing tested before this task. ---
        //
        // (a) envIntegral reaching the GPU intact. It is the entire scale of
        //     the recovered radiance, and a wrong value rescales ALL THREE
        //     estimators together -- so check 29 is blind to it by
        //     construction, and only an absolute comparison against the
        //     environment image can see it.
        // (b) pdfEnvMap, which had no caller under test anywhere: check 24
        //     deliberately writes its own inverse rather than calling it.
        // (c) The ROUTING claim this task rests on -- that next-event
        //     estimation consumes the very sample binding 6 records rather
        //     than drawing its own.
        //
        //     WHICH TIE ACTUALLY CLOSES THAT LOOP, stated exactly, because
        //     "a second draw could satisfy neither" is what this used to
        //     say and it is too strong. The pdf tie (pdfBsdfAtLight ==
        //     max(0, d.y)/pi) constrains d.y alone. So does the estimator
        //     tie: X = f * d.y * envRadiance / envPdf with envRadiance =
        //     envPdf * integral * 2*pi^2 / (W*H), so envPdf CANCELS OUT OF
        //     X entirely and X = f * d.y * integral * 2*pi^2 / (W*H) --
        //     again a constraint on d.y. A re-draw that shared the marginal
        //     row and re-drew only the conditional column would satisfy
        //     both.
        //
        //     The tie that closes the loop is the RADIANCE tie:
        //     envRadiance, recovered from envPdf alone, is compared against
        //     envLum at the texel binding 6's FULL 3-D direction bins into.
        //     That is the one identity a second draw could not satisfy --
        //     it pins the pdf and the direction to the same texel, which is
        //     to say to the same draw.
        // A sample count this check would rather not lose: if boundary
        // ambiguity ever excluded a large share, the pdfEnvMap verdict would
        // quietly be about a shrinking subset.
        constexpr double kMaxSkippedFraction = 0.02;
        const double skippedFraction =
            static_cast<double>(pdfEnvAtBsdfSkipped) / static_cast<double>(kCapacity);
        if (!(worstRadianceRelError <= kTieRelTolerance) ||
            !(worstBsdfRadianceRelError <= kTieRelTolerance) ||
            !(worstPdfBsdfAbsError <= kPdfBsdfAbsTolerance) ||
            !(worstNeeTieRelError <= kTieRelTolerance) || pdfEnvAtBsdfRejected != 0 ||
            !(skippedFraction <= kMaxSkippedFraction)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 31 --\n"
                         "  recovered radiance vs the environment image: worst relative error "
                         "%.3g (tolerance %.3g). This is the ONLY observable that depends on "
                         "ScatterPush::envIntegral, which nothing verified before this task.\n"
                         "  the BSDF strategy's OWN recovered radiance vs the environment image "
                         "at the texel its direction lands in: worst relative error %.3g (same "
                         "tolerance). This is what separates inverting pdfEnvMapTexel (which "
                         "gives L) from inverting pdfEnvMap (which gives "
                         "L*sin(theta_centre)/sin(theta_query), up to ~5x L off a centre).\n"
                         "  diffBsdfEval's pdf at the LIGHT sample vs max(0, d.y)/pi computed "
                         "from binding 6's direction: worst absolute error %.3g (absolute "
                         "tolerance %.3g). A mismatch means next-event estimation is not using "
                         "the direction binding 6 records.\n"
                         "  next-event estimator vs f*cos*L/p recomputed from binding 6: worst "
                         "relative error %.3g.\n"
                         "  pdfEnvMap vs p(texel)*sin(theta_centre)/sin(theta_query): %u of %u "
                         "samples outside their own derived bound (worst raw relative error "
                         "%.3g, worst as a fraction of its bound %.3g, min returned %.6g); %u "
                         "samples (%.3f%%) excluded as texel-boundary-ambiguous\n",
                         worstRadianceRelError, kTieRelTolerance, worstBsdfRadianceRelError,
                         worstPdfBsdfAbsError, kPdfBsdfAbsTolerance,
                         worstNeeTieRelError, pdfEnvAtBsdfRejected, kCapacity,
                         worstPdfEnvRelError, worstPdfEnvNormalisedError, minPdfEnvAtBsdf,
                         pdfEnvAtBsdfSkipped, 100.0 * skippedFraction);
            task4Failed = true;
        } else {
            std::printf("[diff_gpu_probe] OK: check 31 -- across %u samples: the radiance "
                        "recovered from the density and ScatterPush::envIntegral matches the "
                        "environment image to %.3g relative (tolerance %.3g -- envIntegral's "
                        "first end-to-end check anywhere), and the BSDF strategy's own recovered "
                        "radiance matches the image at ITS texel to %.3g relative, which is what "
                        "pins that side to L rather than to "
                        "L*sin(theta_centre)/sin(theta_query); the BSDF pdf at the light sample "
                        "matches max(0,d.y)/pi from binding 6's OWN direction to %.3g absolute "
                        "(tolerance %.3g) and the next-event estimator matches f*cos*L/p "
                        "recomputed from binding 6's (direction, pdf) to %.3g relative, and the "
                        "radiance tie binds that pdf to that direction, so NEE consumes the "
                        "sample check 24 bins rather than a second draw; and pdfEnvMap (min "
                        "%.6g) matches p(texel)*sin(theta_centre)/sin(theta_query) for every one "
                        "of its first tested calls -- worst raw relative error %.3g, worst %.3g "
                        "of its own Vulkan-precision-derived bound (%u samples, %.3f%%, excluded "
                        "as texel-boundary-ambiguous)\n",
                        kCapacity, worstRadianceRelError, kTieRelTolerance,
                        worstBsdfRadianceRelError, worstPdfBsdfAbsError, kPdfBsdfAbsTolerance,
                        worstNeeTieRelError, minPdfEnvAtBsdf, worstPdfEnvRelError,
                        worstPdfEnvNormalisedError, pdfEnvAtBsdfSkipped, 100.0 * skippedFraction);
        }

        if (task4Failed) return 1;
    }

    // ------------------------------------------------------------------
    // 32. RADIANCE ACCUMULATION INTO THE FILM (Stage 0b-2b Task 5).
    // ------------------------------------------------------------------
    //
    // wf_scatter.comp now adds
    //
    //     T_k * (w_E,k * E_k + w_B,k * B_k)
    //
    // into film[pixelIndex] by atomicAdd on EVERY bounce -- T_k the path's
    // throughput on arrival at bounce k's vertex, E_k/B_k the two
    // single-strategy direct-lighting estimators and w_E/w_B their MIS
    // weights. This check asserts that what ends up in that buffer after a
    // FUSED B-bounce run really is the sum of the B per-bounce
    // contributions, reconstructed on the host from primitives read back
    // INDEPENDENTLY of the accumulator.
    //
    // WHAT MAKES THE ORACLE INDEPENDENT. The film is not compared against a
    // copy of itself. wf_scatter.comp records T_k, E_k, w_E, B_k, w_B and
    // the pixel index as SEPARATE floats in the binding-7 sink; the host
    // multiplies and sums them in double. So this rejects an accumulator
    // that drops a bounce, that overwrites instead of adding, that resets,
    // that double-counts, that applies the post-decay throughput instead of
    // the arrival one, that uses one strategy instead of the MIS
    // combination, or that lands in the wrong pixel. It deliberately does
    // NOT re-derive E_k and B_k from the BSDF and the environment -- checks
    // 29-31 own the estimators, and Task 4's report records why those stay
    // host-accumulated and independent of this buffer.
    //
    // WHY "APPLIES THE POST-DECAY THROUGHPUT" IS ACTUALLY REJECTED. The
    // other failure modes above are backed by construction: an overwrite,
    // reset, drop, double-count, wrong-pixel or single-strategy edit changes
    // the RELATIONSHIP between what the sink records and what the film
    // holds, so the reconstruction stops matching. A post-decay-throughput
    // bug is different in kind -- wf_scatter.comp's sink write (slots 21-23)
    // and its film term read the SAME local variable (`arrivalThroughput`),
    // so an edit that moved that variable's value from the arrival
    // throughput to the post-decay one would move the sink and the film
    // TOGETHER, and a check that only compared them against each other could
    // not see it move. What actually closes that seam is the per-path
    // assertion below pinning slots 21-23 to kAlbedo^k -- a value computed
    // from nothing in the shader at all. It is sound only because checks
    // 14/17 already establish, bit-exactly, that this pure-Lambert scene's
    // per-bounce estimator weight is exactly `albedo`, which is what lets
    // "arrival throughput at bounce k" reduce to a closed-form constant
    // instead of another shader-derived quantity.
    //
    // WHERE THE PER-BOUNCE VALUES COME FROM. The binding-7 sink is written
    // at a fixed per-path offset every bounce, so only the LAST bounce's
    // record survives one fused run. runWavefrontFusedLoopProbe already runs
    // the loop once per bounce count (1, 2, ... B) for exactly this reason,
    // so the run of k+1 bounces exposes bounce k. Every run restarts from
    // zeroed buffers with the same seed, wf_scatter.comp rebuilds its RNG
    // from (pixelIndex, sampleIndex, iterationSeed, storedBounce) rather
    // than carrying it, and every stage indexes path state by path index --
    // so bounce k is bit-identical across runs. That is not assumed here: if
    // it did not hold, the sums below would not match the films, and this
    // check is what would say so.
    //
    // THE SCENE IS A RIG, NOT A SCENE. It runs the closed box (so every path
    // is alive at every bounce and every bounce therefore contributes) with
    // wf_scatter.comp's shadow rays pointed at a DIFFERENT, empty
    // acceleration structure, so those contributions are nonzero. Inside the
    // closed box every direct-lighting contribution is exactly zero -- that
    // is check 28 -- and a film check there would be comparing 0 against 0
    // and could not fail. Nothing about the estimator is concluded from this
    // run; see runWavefrontFusedLoopProbe's `unoccludedShadowRays` doc.
    //
    // WHAT THE FILM DOES NOT CONTAIN (informational; read this before Task 6
    // compares it against a PathTracer image). This check, like the shader,
    // sums ONLY MIS direct lighting at surface vertices. There is no escape
    // term: wf_intersect.comp compacts only survivors, so a path that misses
    // everything is dropped from the next bounce's queue and contributes
    // nothing further. There is, as of Stage 1 Task 4, a uniform
    // emissive-surface term gated by `pc.emission` -- but this check leaves
    // it at its 0.0 default, like every check that predates Task 4, so it
    // is still absent from THIS film by construction, not by pipeline
    // limitation (see check 42, which exercises the nonzero case). A
    // PathTracer parity comparison will therefore differ from this film by
    // exactly the directly-visible environment plus any PER-OBJECT
    // emissive term a real scene's materials carry (this subsystem still
    // has no per-object material id) -- see the doc on wf_scatter.comp's
    // binding-9 Film declaration for the full argument.
    // The closed-box rig above makes that gap unobservable from inside this
    // check, which is precisely why it has to be written down here instead.
    //
    // THE BOUND, DERIVED. Let C_k(p,c) be the exact (real-arithmetic)
    // contribution of bounce k to pixel p, channel c, formed from the
    // float32 values the sink recorded. Every factor is NONNEGATIVE
    // (throughput, f*cos, radiance, visibility in {0,1}, balance-heuristic
    // weights in [0,1]), so there is no cancellation anywhere and the
    // classic Higham bound |fl(x) - x| <= gamma_n |x| applies with
    // gamma_n = n*u/(1 - n*u), u = 2^-24 the float32 unit roundoff.
    //
    //   * The shader evaluates T*(w_E*E + w_B*B) per channel. As written
    //     that is 4 roundings (two products, one sum, one product); a
    //     compiler that distributes it to T*w_E*E + T*w_B*B uses 5. Take the
    //     larger: n_eval = 5. (FMA contraction can only reduce the error, so
    //     it stays inside this bound.)
    //   * The GPU then accumulates k such values into a zeroed float32 film
    //     by atomicAdd. 0 + C_0 is exact, so that is k-1 roundings:
    //     n_sum = k - 1.
    //   * The host recomputes the same expression in float64 from the same
    //     float32 inputs; at u_64 = 2^-53 its own error is 2^29 times
    //     smaller and is neglected.
    //
    // Total n = n_eval + n_sum = k + 4, giving relTol(k) = gamma_{k+4}.
    // At k = 4 bounces that is 8u/(1-8u) = 4.77e-7 relative.
    //
    // Plus an absolute floor of 1e-30, which exists only for the underflow
    // corner: a contribution below the float32 subnormal threshold (1.2e-38)
    // can flush to zero on the GPU while the double reconstruction keeps it,
    // and a purely relative bound would call that a 100% error. It is thirty
    // orders of magnitude below the contributions this scene actually
    // produces (~1e-2), so it cannot absorb a real discrepancy. The
    // non-vacuity gate below states that margin as a number rather than
    // leaving it to this comment.
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 8;  // wf_generate's 1-D dispatch requires this exactly.
        constexpr uint32_t kCapacity = kW * kH;  // 512
        constexpr uint32_t kPixels = kW * kH;    // one sample per pixel here
        constexpr uint32_t kBounces = 4;
        constexpr float kAlbedo = 0.5f;
        constexpr uint32_t kIterationSeed = 20260829u;
        // Non-square, for the same reason check 27 is: it costs nothing and
        // a W<->H swap anywhere in the film path would stop being symmetric.
        constexpr uint32_t kEnvW = 16;
        constexpr uint32_t kEnvH = 4;

        constexpr double kUnitRoundoff = 1.0 / 16777216.0;  // 2^-24
        constexpr double kAbsFloor = 1e-30;
        // How many times larger than the tolerance the smallest single
        // bounce's total contribution must be for this check to be able to
        // reject a dropped bounce. 1e3 is not a physical constant -- it is
        // the margin this check refuses to run below, so that "it passed"
        // is never compatible with "there was nothing to detect".
        constexpr double kMinDiscriminationMargin = 1.0e3;

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 32 buffers build\n");
            return 1;
        }

        std::vector<std::vector<float>> drawsPerBounce;
        std::vector<uint32_t> liveCountPerRun;
        std::vector<uint32_t> finalQueue;
        std::vector<std::vector<float>> neePerRun;
        std::vector<std::vector<float>> filmPerRun;
        if (!ctx.runWavefrontFusedLoopProbe(wf, kW, kH, kBounces, kAlbedo, kIterationSeed,
                                            drawsPerBounce, liveCountPerRun, finalQueue,
                                            /*outEnvSamples=*/nullptr,
                                            /*outNeeSamples=*/nullptr,
                                            /*unoccludedShadowRays=*/true, &neePerRun,
                                            &filmPerRun)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 32 fused loop dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }
        wf.destroy(ctx.allocator());

        if (neePerRun.size() != kBounces || filmPerRun.size() != kBounces) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 32 got %zu NEE runs and %zu film runs, "
                         "expected %u of each\n",
                         neePerRun.size(), filmPerRun.size(), kBounces);
            return 1;
        }

        // Running sum of the reconstructed contributions, in double.
        std::vector<double> hostFilm(static_cast<std::size_t>(kPixels) * 3u, 0.0);
        // Per-bounce total over all pixels and channels, for the
        // discrimination margin below.
        double bounceTotal[kBounces] = {};
        double worstRelError = 0.0;
        double worstAbsError = 0.0;
        uint32_t worstBounce = 0;
        uint32_t litLightSamples = 0;
        uint32_t litBsdfSamples = 0;

        for (uint32_t k = 0; k < kBounces; ++k) {
            if (neePerRun[k].size() != static_cast<std::size_t>(kCapacity) *
                                           ohao::diff::kNeeSampleFloats ||
                filmPerRun[k].size() != static_cast<std::size_t>(kPixels) * 3u) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 32 bounce %u readback sizes are %zu "
                             "NEE floats and %zu film floats, expected %u and %u\n",
                             k, neePerRun[k].size(), filmPerRun[k].size(),
                             kCapacity * ohao::diff::kNeeSampleFloats, kPixels * 3u);
                return 1;
            }
            if (liveCountPerRun[k] != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 32 run of %u bounces left %u live "
                             "paths, expected all %u. Every bounce must contribute for the "
                             "accumulation across bounces to be what is under test\n",
                             k + 1u, liveCountPerRun[k], kCapacity);
                return 1;
            }

            // Review Finding 1 (Task 5): pin the sink's arrival-throughput
            // slots (21-23) to a constant computed independently of
            // wf_scatter.comp's own `arrivalThroughput` variable, rather
            // than letting them stand in for it unchecked. This is a pure
            // Lambert material (specularWeight = 0, metallic = 0) with
            // kAlbedo = 0.5, and checks 14/17 already establish -- bit-
            // exactly, not approximately -- that this shader's per-bounce
            // estimator weight f*cos/pdf is exactly `albedo`. So the
            // throughput arriving at bounce k (0-based: k=0 is the first
            // bounce, where the path has undergone zero decays yet) is
            // exactly kAlbedo^k. kAlbedo = 0.5 is a dyadic rational, so
            // every one of these powers -- 1, 0.5, 0.25, 0.125 for the
            // k in {0,1,2,3} this check reaches -- is exactly representable
            // in float32 AND float64 with no rounding at any step; computed
            // here by repeated multiplication (not std::pow, whose result
            // for a non-trivial exponent is not guaranteed bit-exact) so the
            // comparison below is a bit-exact `!=`, not a tolerance.
            //
            // WHY THIS CLOSES A SEAM. Without it, slots 21-23 were checked
            // only for internal consistency with the film's own
            // accumulation (both read wf_scatter.comp's `arrivalThroughput`
            // local): a shader edit that moved the sink write and the film
            // term onto the SAME wrong value (e.g. the post-decay
            // throughput instead of the arrival one) would move both
            // together and this check would still pass. Comparing slots
            // 21-23 against a value that does not come from the shader at
            // all is what makes that edit detectable. See this task's
            // report for a demonstration: recording the post-decay
            // throughput here fails this exact assertion.
            double expectedArrivalThroughput = 1.0;
            for (uint32_t decays = 0; decays < k; ++decays) {
                expectedArrivalThroughput *= static_cast<double>(kAlbedo);
            }

            // Fold bounce k's per-path contributions into hostFilm, and
            // check that every pixel is written exactly once (the film is
            // indexed by PIXEL; the mapping is read from the sink rather
            // than assumed to be the identity).
            std::vector<uint32_t> pixelHits(kPixels, 0);
            for (uint32_t i = 0; i < kCapacity; ++i) {
                const std::size_t b =
                    static_cast<std::size_t>(i) * ohao::diff::kNeeSampleFloats;
                if (neePerRun[k][b + ohao::diff::kNeeSlotSurfaceBranch] != 1.0f) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: check 32 path %u did not take "
                                 "wf_scatter.comp's surface branch at bounce %u. Inside a closed "
                                 "box every ray hits a face, so a miss means the scene is not "
                                 "what this check assumes and the contributions it sums would "
                                 "be silently short\n",
                                 i, k);
                    return 1;
                }
                const float pixF = neePerRun[k][b + ohao::diff::kNeeSlotPixelIndex];
                if (!(pixF >= 0.0f) || pixF >= static_cast<float>(kPixels) ||
                    pixF != std::floor(pixF)) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: check 32 path %u reported pixel index "
                                 "%.9g at bounce %u, which is not an integer in [0, %u)\n",
                                 i, static_cast<double>(pixF), k, kPixels);
                    return 1;
                }
                const uint32_t pix = static_cast<uint32_t>(pixF);
                ++pixelHits[pix];

                if (neePerRun[k][b + ohao::diff::kNeeSlotVisLight] != 0.0f) ++litLightSamples;
                if (neePerRun[k][b + ohao::diff::kNeeSlotVisBsdf] != 0.0f) ++litBsdfSamples;

                const double wEnv = neePerRun[k][b + ohao::diff::kNeeSlotWEnvAtLight];
                const double wBsdf = neePerRun[k][b + ohao::diff::kNeeSlotWBsdfAtBsdf];
                for (uint32_t c = 0; c < 3u; ++c) {
                    const double thr =
                        neePerRun[k][b + ohao::diff::kNeeSlotArrivalThroughput + c];
                    if (thr != expectedArrivalThroughput) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: check 32 path %u channel %u at "
                                     "bounce %u recorded arrival throughput %.17g at binding-7 "
                                     "slot %u, but this pure-Lambert scene's arrival throughput "
                                     "at bounce %u is exactly kAlbedo^%u = %.17g, bit-exactly "
                                     "(checks 14/17 already establish the per-bounce estimator "
                                     "weight is exactly `albedo`). This pins slots 21-23 to a "
                                     "value independent of wf_scatter.comp's own "
                                     "`arrivalThroughput` variable -- see this check's header, "
                                     "'the bolded claim', and the task-5 fix report's "
                                     "demonstration: recording the POST-decay throughput here "
                                     "instead of the arrival one fails this exact assertion\n",
                                     i, c, k, thr,
                                     static_cast<unsigned>(ohao::diff::kNeeSlotArrivalThroughput) +
                                         c,
                                     k, k, expectedArrivalThroughput);
                        return 1;
                    }
                    const double nee = neePerRun[k][b + ohao::diff::kNeeSlotNeeUnweighted + c];
                    const double bsdf = neePerRun[k][b + ohao::diff::kNeeSlotBsdfUnweighted + c];
                    const double contribution = thr * (wEnv * nee + wBsdf * bsdf);
                    hostFilm[static_cast<std::size_t>(pix) * 3u + c] += contribution;
                    bounceTotal[k] += contribution;
                }
            }
            // STRUCTURALLY HARD-CODED TO 1 SAMPLE PER PIXEL (review Finding
            // 4, Task 6 will want more). `pixelHits[p] != 1u` demands
            // exactly one path per pixel; it cannot be re-pointed at >1 spp
            // without editing this loop (and kCapacity/kPixels above, which
            // are tied 1:1 here). That is a documented constraint for
            // whoever wires more samples in, not a bug in this check as
            // written for 1 spp.
            //
            // A second fact matters for that future work and does NOT show
            // up as a failure today: at >1 spp, multiple paths sharing a
            // pixel hit `atomicAdd(counters...)` for the SAME destination
            // slot, so which path's record ends up at which queue slot
            // becomes an intra-dispatch race -- nondeterministic run to run.
            // That weakens (does not break) the cross-run bit-identity
            // runWavefrontFusedLoopProbe's bounce-by-bounce reconstruction
            // rests on: it only requires bounce k to be bit-identical FOR A
            // GIVEN PATH across runs, and per-path RNG reconstruction from
            // (pixelIndex, sampleIndex, iterationSeed, bounce) still gives
            // that regardless of queue order -- so the reconstruction itself
            // is fine, but a check written the way this one is (indexing by
            // PATH, then reading which pixel it landed on) would need to sum
            // per-pixel across paths rather than assume a 1:1 path<->pixel
            // map. The gamma_{k+4} bound is unaffected either way: it is
            // derived for a nonnegative sum and is order-independent by
            // construction.
            for (uint32_t p = 0; p < kPixels; ++p) {
                if (pixelHits[p] != 1u) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: check 32 pixel %u was written by %u "
                                 "paths at bounce %u, expected exactly 1 (this probe runs one "
                                 "sample per pixel)\n",
                                 p, pixelHits[p], k);
                    return 1;
                }
            }

            // gamma_{k+4} -- see the derivation in this check's header. k is
            // 0-based here, so the run summed k+1 bounces and n = (k+1) + 4.
            const double n = static_cast<double>(k + 1u) + 4.0;
            const double relTol = (n * kUnitRoundoff) / (1.0 - n * kUnitRoundoff);
            for (std::size_t idx = 0; idx < hostFilm.size(); ++idx) {
                const double gpu = filmPerRun[k][idx];
                const double host = hostFilm[idx];
                const double absErr = std::abs(gpu - host);
                const double allowed = relTol * std::abs(host) + kAbsFloor;
                if (!(absErr <= allowed) || !std::isfinite(gpu)) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: check 32 -- after a FUSED run of %u "
                                 "bounces, film[pixel %zu][channel %zu] = %.9g but the sum of "
                                 "the %u per-bounce contributions, reconstructed on the host "
                                 "from independently recorded throughput/estimator/MIS-weight "
                                 "primitives, is %.9g. Absolute error %.3g, allowed %.3g "
                                 "(gamma_%g = %.3g relative, plus a %.3g absolute floor)\n",
                                 k + 1u, idx / 3u, idx % 3u, gpu, k + 1u, host, absErr, allowed,
                                 n, relTol, kAbsFloor);
                    return 1;
                }
                worstAbsError = std::max(worstAbsError, absErr);
                if (std::abs(host) > 0.0) {
                    const double rel = absErr / std::abs(host);
                    if (rel > worstRelError) {
                        worstRelError = rel;
                        worstBounce = k + 1u;
                    }
                }
            }
        }

        // --- NON-VACUITY. All of the above is satisfied by a film of zeros
        // and a sink of zeros. These are the gates that say there was
        // something to detect.
        double finalTotal = 0.0;
        for (const double v : hostFilm) finalTotal += v;
        double minBounceTotal = bounceTotal[0];
        double maxBounceTotal = bounceTotal[0];
        for (uint32_t k = 0; k < kBounces; ++k) {
            minBounceTotal = std::min(minBounceTotal, bounceTotal[k]);
            maxBounceTotal = std::max(maxBounceTotal, bounceTotal[k]);
        }
        if (litLightSamples == 0 || litBsdfSamples == 0) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 32 -- %u light-sample and %u BSDF-sample "
                         "shadow rays were unoccluded across %u bounces. This run points the "
                         "shadow rays at an EMPTY acceleration structure precisely so they are "
                         "not occluded; with all of them blocked every contribution is zero and "
                         "the accumulation check would compare 0 against 0\n",
                         litLightSamples, litBsdfSamples, kBounces);
            return 1;
        }
        const double finalRelTol =
            ((kBounces + 4.0) * kUnitRoundoff) / (1.0 - (kBounces + 4.0) * kUnitRoundoff);
        if (!(minBounceTotal > kMinDiscriminationMargin * finalRelTol * finalTotal) ||
            !(finalTotal > 0.0)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 32 -- the smallest single bounce "
                         "contributes %.9g of a film total of %.9g. Dropping that bounce "
                         "entirely would have to move the film by more than %.0fx the "
                         "tolerance (%.3g relative) for this check to be able to reject it, and "
                         "it does not -- so a pass here would not be evidence\n",
                         minBounceTotal, finalTotal, kMinDiscriminationMargin, finalRelTol);
            return 1;
        }

        std::printf("[diff_gpu_probe] OK: check 32 -- after a FUSED %u-bounce run through "
                    "WavefrontLoop::record, the caller-owned film buffer holds exactly the sum "
                    "of the %u per-bounce contributions reconstructed on the host from "
                    "independently recorded primitives (arrival throughput, both "
                    "single-strategy estimators, both MIS weights, the pixel index): worst "
                    "relative error %.3g over %u pixels x 3 channels x %u prefix lengths (worst "
                    "at %u bounces; bound gamma_{k+4}, %.3g at k=%u), worst absolute error "
                    "%.3g. Non-vacuous: the film total is %.6g, the smallest single bounce "
                    "contributes %.6g of it (the largest is %.1fx that, and the smallest is "
                    "%.3g times the tolerance -- floor %.0fx), and %u light-sample / %u "
                    "BSDF-sample shadow rays reached the environment\n",
                    kBounces, kBounces, worstRelError, kPixels, kBounces, worstBounce,
                    finalRelTol, kBounces, worstAbsError, finalTotal, minBounceTotal,
                    (minBounceTotal > 0.0 ? maxBounceTotal / minBounceTotal : 0.0),
                    minBounceTotal / (finalRelTol * finalTotal), kMinDiscriminationMargin,
                    litLightSamples, litBsdfSamples);
    }

    // -----------------------------------------------------------------
    // 33-34. THE STAGE GATE: the wavefront integrator against an
    // independent reference integrator, on a probe-owned scene.
    // -----------------------------------------------------------------
    //
    // WHAT THE REFERENCE IS, AND WHY IT IS NOT ohao::PathTracer. Stated in
    // full at the head of this file's anonymous namespace (see "INDEPENDENT
    // CPU REFERENCE INTEGRATOR"): PathTracer is a
    // VK_KHR_ray_tracing_pipeline renderer and GpuProbeContext's device
    // deliberately does not enable that extension, which this task's
    // constraints forbid adding. The reference is therefore a CPU path
    // tracer written from the definition of the estimator, sharing no code,
    // no constant and no sampling machinery with the GPU integrator -- its
    // own intersector, its own basis, its own RNG, cosine sampling with NO
    // environment importance sampling and NO MIS. Two different estimators
    // of one integral agree only if both are unbiased.
    //
    // WHAT IS COMPARED, AND WHAT IS EXCLUDED. The film holds only
    // SUM_k T_k * Lhat_direct(x_k): MIS direct lighting at surface vertices,
    // truncated at kBounces, with no emissive term and no background term.
    // The reference computes exactly that, with the same truncation and the
    // same two omissions. The one term a classical path tracer would have
    // and this film does not is the CAMERA ray's own miss -- and the scene
    // below is built so that set is EMPTY, which the first assertion here
    // measures rather than assumes (a one-bounce run must leave all
    // kCapacity paths live). A ray that escapes at a LATER bounce is not a
    // gap at all: its environment contribution was already added, eagerly,
    // at the vertex it left, by the BSDF strategy's shadow ray along that
    // very direction. The anonymous-namespace header derives that identity.
    //
    // THE SCENE, and why each piece is where it is.
    //   * FLOOR, y = 0, |x|,|z| <= 8. Every primary ray lands on it: the
    //     camera sits at y = 3 looking straight down with tanHalfFov 0.2 at
    //     aspect 8, so the extreme ray lands at |x| = 3*0.984375*8*0.2 =
    //     4.725 and |z| = 0.525 -- 3.27 units inside the nearest edge. No
    //     primary ray is anywhere near a silhouette, which matters because
    //     a pixel whose primary ray grazed an edge could hit different
    //     triangles under a BVH and under Moller-Trumbore and would show up
    //     as a permanent per-pixel disagreement that no sample count fixes.
    //   * OVERHANG, y = 5, |x| <= 1.5. ABOVE the camera, and every primary
    //     ray travels strictly downward, so it is unreachable by a primary
    //     ray by construction -- but it occludes the zenith (the most
    //     cosine-weighted part of the hemisphere) for the middle of the
    //     image and catches second-bounce rays. This is where most of the
    //     visibility signal and most of the interreflection come from.
    //   * SIDE WALL, x = 5.5, 0 <= y <= 4. Out of the primary frustum with
    //     margin (a ray needs a horizontal slope of 5.5/3 = 1.833 to reach
    //     it and the widest is 1.575), and it makes the occlusion vary
    //     ASYMMETRICALLY across the image: a floor point at x = +4.725 is
    //     0.775 from it, one at x = -4.725 is 10.2 away.
    // Every quad is wound so that wf_intersect.comp's flip-to-oppose-the-ray
    // step fires for the rays that actually reach it.
    //
    // THE ENVIRONMENT is a smooth, strictly positive, doubly asymmetric
    // gradient (brightest at the +Y pole, where the floor can see it) with a
    // 5:1 contrast. That contrast is a DESIGN CALL: a strongly peaked
    // environment -- checks 29-31 use one with an 8x block -- inflates the
    // variance of BOTH estimators, and the variance is what sets how small a
    // disagreement this gate can resolve. Env importance sampling, pdfEnvMap
    // and the balance heuristic are all still exercised (the CDF is not
    // uniform in either axis); it is only their variance-reduction margin
    // that is smaller here, and no claim is made about that.
    //
    // THE BOUND, DERIVED. Two independent unbiased estimators of the same
    // per-pixel value mu(p):
    //   * GPU: kSeedsFull runs, each ONE sample per pixel, each with a
    //     distinct iterationSeed. wf_scatter.comp rebuilds its RNG from
    //     (pixelIndex, sampleIndex, iterationSeed) every bounce, so distinct
    //     seeds give independent paths. Sample mean mG(p), unbiased sample
    //     variance vG(p), standard error seG = sqrt(vG/R).
    //   * CPU: kCpuFull samples, its own generator. mC(p), seC = sqrt(vC/M).
    // D(p) = mG(p) - mC(p) has expectation 0 and standard deviation
    // sigma(p) = sqrt(seG^2 + seC^2), ESTIMATED FROM THE RUN ITSELF -- this
    // is the "variance estimate from the run" option Task 6 names, taken
    // deliberately: the per-pixel variance of an MIS path-tracing estimator
    // on this geometry has no closed form to derive it from. What IS derived
    // is the multiplier:
    //   * PER PIXEL, kPixels comparisons, two-sided. A family-wise false
    //     rejection rate of 1e-3 needs 2*kPixels*Phi(-z) <= 1e-3, i.e.
    //     Phi(-z) <= 9.8e-7, i.e. z >= 4.76. kPerPixelZ = 5.0 (family-wise
    //     2*512*Phi(-5) = 2*512*2.8665e-7 ~= 2.94e-4 under normality; still
    //     comfortably under the 1e-3 budget). Normality is approximate at these
    //     sample counts, which is why the pooled test below -- where the
    //     central limit theorem is on much firmer ground -- carries the
    //     sharp discrimination and this one carries the spatial coverage.
    //   * POOLED, ONE comparison, on the IMAGE TOTAL. For each GPU seed i
    //     the whole-image total S_i = sum_p film_i(p) is one draw; for each
    //     CPU sample index j, T_j = sum_p c(p,j) is one draw. Their sample
    //     variances are computed ACROSS RUNS, so this statistic needs no
    //     assumption at all about whether different pixels are independent
    //     -- any correlation is already inside Var(S_i). One comparison at
    //     1e-4 two-sided needs z >= 3.9; kPooledZ = 4.0.
    // No tolerance here was chosen by running the check and widening until
    // it passed; both multipliers come from the comparison count, and both
    // scales come from measured variances.
    //
    // WHY BOTH. The per-pixel test sees a disagreement that lives in a few
    // pixels and cancels in the mean; the pooled test sees a coherent bias
    // far too small for any single pixel to resolve (a wrong material
    // constant, a double-counted throughput, an MIS partition that does not
    // sum to one). Its resolution is reported as a number below and gated:
    // if 4*sigma_pooled/mean ever exceeded kMaxPooledResolution the check
    // refuses to claim a verdict, because "it passed" would then be
    // compatible with "there was nothing it could have detected".
    //
    // CONVERGENCE. Everything above is also computed on a PREFIX of both
    // sample sets -- a quarter of the seeds and a quarter of the CPU samples
    // -- and the check asserts that the root-mean-square of D over the image
    // SHRINKS BY THE PREDICTED FACTOR. rms(D)^2 estimates E[D^2] =
    // sigma^2, and quadrupling both sample counts quarters sigma^2, so the
    // predicted ratio is exactly 0.5. A FIXED BIAS b would floor rms(D) at
    // |b| and drive that ratio to 1: this is the assertion a pair of
    // wrong-but-close images cannot satisfy, and the reason a fixed
    // tolerance alone would not be a gate. The window is [0.30, 0.70]: the
    // sampling standard deviation of an RMS over kPixels values is about
    // sqrt(1/(2*kPixels)) ~ 3% relative, and the two sets are NESTED (the
    // prefix is literally the first quarter of the same runs), so the ratio
    // is far more stable than that; the window is widened well past it to
    // cover the non-Gaussian tail of a Monte Carlo difference. The lower
    // edge is not decoration -- a ratio near zero would mean D collapsed,
    // which is what comparing a quantity against itself looks like.
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 8;  // wf_generate's 1-D dispatch requires this exactly.
        constexpr uint32_t kCapacity = kW * kH;
        constexpr uint32_t kPixels = kW * kH;
        constexpr uint32_t kBounces = 3;
        constexpr float kAlbedo = 0.6f;

        constexpr uint32_t kEnvW = 64;
        constexpr uint32_t kEnvH = 32;
        static_assert(kEnvW != kEnvH, "a square environment hides a W<->H swap");
        // (the texel count is no longer needed here: buildParityEnvironment
        // sizes both of its outputs from kEnvW/kEnvH itself)

        constexpr std::size_t kSeedsFull = 512;
        constexpr std::size_t kSeedsPrefix = kSeedsFull / 4;
        constexpr std::size_t kCpuFull = 4096;
        constexpr std::size_t kCpuPrefix = kCpuFull / 4;
        static_assert(kSeedsFull == 4 * kSeedsPrefix && kCpuFull == 4 * kCpuPrefix,
                      "the convergence assertion predicts a factor of exactly 2 in rms(D), "
                      "which requires BOTH sample counts to scale by exactly 4");

        constexpr double kPerPixelZ = 5.0;
        constexpr double kPooledZ = 4.0;
        constexpr double kConvergenceMin = 0.30;
        constexpr double kConvergenceMax = 0.70;
        // Pre-registered non-vacuity gates. These are statements about how
        // sharp this check has to be before its verdict means anything, not
        // tolerances on the thing under test.
        constexpr double kMaxPooledResolution = 0.02;   // 4*sigma/mean on the image total
        constexpr double kMaxPerPixelResolution = 0.30;  // worst 5*sigma(p)/mu(p)
        // A floor under sigma(p), for the arithmetic corner where a pixel's
        // estimated variance underflows to something meaningless. Expressed
        // as a fraction of the image's own mean pixel value so it carries no
        // absolute scale of its own. The check reports the smallest observed
        // sigma(p) as a multiple of it, so it is visible whether it ever
        // came close to binding.
        constexpr double kSigmaFloorFraction = 1e-6;

        // --- The environment, the scene and the camera. All three are built
        // by the shared builders in this file's anonymous namespace, NOT
        // transcribed here: the Stage 1 Task 2 gradient checks run against
        // the identical configuration, and two copies of a test scene are two
        // chances for one of them to drift into measuring something else. The
        // reasons each quad is where it is, and the reason the environment's
        // contrast is 5:1 rather than peaked, are on those builders.
        std::vector<float> envRgba;
        std::vector<double> envLum;
        buildParityEnvironment(kEnvW, kEnvH, envRgba, envLum);

        std::vector<float> positions;
        std::vector<uint32_t> indices;
        buildParityScene(positions, indices);
        const std::vector<ParityTriangle> refTris = parityTrianglesFromSoup(positions, indices);

        const ohao::diff::WavefrontGenerateCamera camera = parityCamera();

        // Distinct, spread-out seeds. Any injective map would do; this one
        // is an odd stride so no two seeds collide and consecutive runs are
        // not neighbours in the RNG's input space.
        std::vector<uint32_t> seeds(kSeedsFull);
        for (std::size_t i = 0; i < kSeedsFull; ++i) {
            seeds[i] = 20260901u + static_cast<uint32_t>(i) * 2654435761u;
        }

        struct ParityConfig {
            const char* name;
            uint32_t checkNumber;
            ohao::diff::WavefrontScatterMaterial material;
        };
        // Two materials, so the gate covers both of diffBsdfSample's lobes.
        // The Lambert case is the one whose per-bounce estimator weight is
        // exactly `albedo` (checks 14/17); the conductor case has q = 1
        // exactly, so every sample comes from the GGX visible-normal
        // sampler and the diffuse lobe is entirely out of the picture.
        const ParityConfig configs[2] = {
            {"pure Lambert (roughness 1, metallic 0, specularWeight 0)", 33u, {1.0f, 0.0f, 0.0f}},
            {"rough conductor (roughness 0.7, metallic 1, specularWeight 1)", 34u,
             {0.7f, 1.0f, 1.0f}},
        };

        for (const ParityConfig& cfg : configs) {
            ohao::EnvCDF envCdf;
            envCdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
            if (!envCdf.valid()) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: check %u: EnvCDF::build produced no "
                                      "CDF for a %ux%u strictly-positive environment\n",
                             cfg.checkNumber, kEnvW, kEnvH);
                return 1;
            }

            ohao::diff::WavefrontBuffers wf;
            if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH) ||
                !wf.uploadEnvironment(envCdf.marginalSpan(), envCdf.conditionalSpan(),
                                      envCdf.integral())) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check %u buffers build / env CDF upload\n",
                             cfg.checkNumber);
                wf.destroy(ctx.allocator());
                return 1;
            }

            // --- NON-VACUITY GATE 1: no primary ray escapes. One bounce,
            // one seed: wf_intersect.comp compacts only survivors and
            // wf_scatter.comp re-queues everything it is given, so the live
            // count after a ONE-bounce run is exactly the number of primary
            // rays that hit something. Anything below kCapacity means the
            // film is missing a directly-visible-environment term this
            // comparison does not model, and the verdict below would be
            // about a different quantity than the one it claims.
            std::vector<std::vector<float>> probeFilm;
            std::vector<uint32_t> probeLive;
            const std::span<const uint32_t> oneSeed(seeds.data(), 1);
            if (!ctx.runWavefrontParityProbe(wf, kW, kH, /*bounces=*/1u, camera,
                                             std::span<const float>(positions),
                                             std::span<const uint32_t>(indices), kAlbedo,
                                             cfg.material, oneSeed, probeFilm, probeLive)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: check %u one-bounce probe dispatch\n",
                             cfg.checkNumber);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (probeLive.size() != 1 || probeLive[0] != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check %u -- %u of %u primary rays hit the "
                             "scene. This check compares a film that contains NO "
                             "directly-visible-environment term against a reference that "
                             "contains none either, which is only the same quantity when every "
                             "primary ray hits. It does not, so the scene is wrong, not the "
                             "integrator\n",
                             cfg.checkNumber, probeLive.empty() ? 0u : probeLive[0], kCapacity);
                wf.destroy(ctx.allocator());
                return 1;
            }

            // --- The GPU integrator: kSeedsFull independent 1-spp runs of
            // the full kBounces loop, through WavefrontLoop::record.
            std::vector<std::vector<float>> films;
            std::vector<uint32_t> liveCounts;
            if (!ctx.runWavefrontParityProbe(wf, kW, kH, kBounces, camera,
                                             std::span<const float>(positions),
                                             std::span<const uint32_t>(indices), kAlbedo,
                                             cfg.material,
                                             std::span<const uint32_t>(seeds), films, liveCounts)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: check %u parity probe dispatch\n",
                             cfg.checkNumber);
                wf.destroy(ctx.allocator());
                return 1;
            }
            wf.destroy(ctx.allocator());

            if (films.size() != kSeedsFull) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: check %u got %zu films, expected %zu\n",
                             cfg.checkNumber, films.size(), kSeedsFull);
                return 1;
            }

            // --- NON-VACUITY GATE 2: the three film channels. Both the base
            // colour and the environment are grey and every film factor is
            // applied per channel identically, so R, G and B must be the
            // SAME BITS. Asserting it costs nothing and is the only thing
            // here that would notice a per-channel indexing error; it also
            // licenses everything below comparing channel 0 alone.
            for (std::size_t i = 0; i < films.size(); ++i) {
                if (films[i].size() != static_cast<std::size_t>(kPixels) * 3u) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: check %u film %zu has %zu floats, "
                                 "expected %u\n",
                                 cfg.checkNumber, i, films[i].size(), kPixels * 3u);
                    return 1;
                }
                for (uint32_t p = 0; p < kPixels; ++p) {
                    const float r = films[i][static_cast<std::size_t>(p) * 3u + 0u];
                    const float g = films[i][static_cast<std::size_t>(p) * 3u + 1u];
                    const float b = films[i][static_cast<std::size_t>(p) * 3u + 2u];
                    if (!std::isfinite(r) || r < 0.0f || g != r || b != r) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: check %u run %zu pixel %u film is "
                                     "(%.9g, %.9g, %.9g). It must be finite, non-negative and "
                                     "bit-identical across the three channels: this scene's base "
                                     "colour and environment are both grey\n",
                                     cfg.checkNumber, i, p, static_cast<double>(r),
                                     static_cast<double>(g), static_cast<double>(b));
                        return 1;
                    }
                }
            }

            // --- The reference integrator. Pixel-parallel; each pixel's RNG
            // stream depends only on its own index (see the `p`-only seed
            // below), so EVERY PER-PIXEL RESULT -- cpuSumPrefix/Full,
            // cpuSumSqPrefix/Full, and everything checks 33-34 derive from
            // them (worstZ, sharpness, per-pixel resolution) -- is exactly
            // thread-count-independent: which thread computes pixel p never
            // changes p's own running sum. The ONE quantity that is not is
            // cpuImageTotals[j] (the pooled statistic's CPU side, below):
            // it is a sum of per-thread partial sums, so its low-order bits
            // depend on how many threads split the reduction, i.e. on
            // hardware_concurrency(). At this scale (an image total of
            // order 361, threadCount <= 16) that is a ~1e-13 relative
            // floating-point reordering effect -- roughly 5e-14 absolute --
            // many orders below anything kPooledZ resolves, so it is
            // immaterial to the verdict, but it is not exactly zero.
            ParityRefScene refScene;
            refScene.tris = refTris;
            refScene.envLum = &envLum;
            refScene.envW = kEnvW;
            refScene.envH = kEnvH;
            refScene.material.baseColor = {kAlbedo, kAlbedo, kAlbedo};
            refScene.material.roughness = cfg.material.roughness;
            refScene.material.metallic = cfg.material.metallic;
            refScene.material.specularWeight = cfg.material.specularWeight;
            refScene.bounces = kBounces;

            const OracleVec3 camOrigin{camera.origin[0], camera.origin[1], camera.origin[2]};

            std::vector<double> cpuSumPrefix(kPixels, 0.0);
            std::vector<double> cpuSumSqPrefix(kPixels, 0.0);
            std::vector<double> cpuSumFull(kPixels, 0.0);
            std::vector<double> cpuSumSqFull(kPixels, 0.0);
            // Per CPU sample index, the whole-image total. Accumulated
            // thread-locally and reduced in thread order, so it is
            // deterministic.
            std::vector<double> cpuImageTotals(kCpuFull, 0.0);

            const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
            const unsigned threadCount = std::min<unsigned>(hw, 16u);
            std::vector<std::vector<double>> perThreadTotals(
                threadCount, std::vector<double>(kCpuFull, 0.0));
            {
                std::vector<std::thread> workers;
                workers.reserve(threadCount);
                for (unsigned tIdx = 0; tIdx < threadCount; ++tIdx) {
                    workers.emplace_back([&, tIdx]() {
                        const uint32_t begin =
                            static_cast<uint32_t>((static_cast<std::size_t>(kPixels) * tIdx) /
                                                  threadCount);
                        const uint32_t end = static_cast<uint32_t>(
                            (static_cast<std::size_t>(kPixels) * (tIdx + 1u)) / threadCount);
                        std::vector<double>& totals = perThreadTotals[tIdx];
                        for (uint32_t p = begin; p < end; ++p) {
                            const uint32_t px = p % kW;
                            const uint32_t py = p / kW;
                            const OracleVec3 camDir = parityCameraRay(px, py, kW, kH, camera);
                            // Per-pixel stream, so the partition into
                            // threads cannot change any number.
                            std::mt19937_64 rng(0x9E3779B97F4A7C15ull *
                                                    (static_cast<std::uint64_t>(p) + 1ull) +
                                                static_cast<std::uint64_t>(cfg.checkNumber) *
                                                    1000003ull);
                            for (std::size_t j = 0; j < kCpuFull; ++j) {
                                const double v =
                                    parityReferenceSample(refScene, camOrigin, camDir, rng);
                                cpuSumFull[p] += v;
                                cpuSumSqFull[p] += v * v;
                                if (j < kCpuPrefix) {
                                    cpuSumPrefix[p] += v;
                                    cpuSumSqPrefix[p] += v * v;
                                }
                                totals[j] += v;
                            }
                        }
                    });
                }
                for (std::thread& w : workers) w.join();
            }
            for (unsigned tIdx = 0; tIdx < threadCount; ++tIdx) {
                for (std::size_t j = 0; j < kCpuFull; ++j) {
                    cpuImageTotals[j] += perThreadTotals[tIdx][j];
                }
            }

            // --- Per-pixel statistics, at the prefix and at the full budget.
            std::vector<double> gpuPixel(kSeedsFull, 0.0);
            double meanPixelValue = 0.0;
            for (uint32_t p = 0; p < kPixels; ++p) {
                meanPixelValue += cpuSumFull[p] / static_cast<double>(kCpuFull);
            }
            meanPixelValue /= static_cast<double>(kPixels);
            const double sigmaFloor = kSigmaFloorFraction * std::abs(meanPixelValue);

            double worstZ[2] = {0.0, 0.0};
            uint32_t worstZPixel[2] = {0u, 0u};
            double sumDSq[2] = {0.0, 0.0};
            double worstPixelResolution = 0.0;
            double minSigma = std::numeric_limits<double>::infinity();
            double worstDetail[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
            bool sigmaFloorBound = false;

            for (uint32_t p = 0; p < kPixels; ++p) {
                for (std::size_t i = 0; i < kSeedsFull; ++i) {
                    gpuPixel[i] = films[i][static_cast<std::size_t>(p) * 3u + 0u];
                }
                for (int which = 0; which < 2; ++which) {
                    const std::size_t r = (which == 0) ? kSeedsPrefix : kSeedsFull;
                    const std::size_t m = (which == 0) ? kCpuPrefix : kCpuFull;
                    double meanG = 0.0, varG = 0.0;
                    parityMoments(gpuPixel.data(), r, meanG, varG);
                    const double sumC = (which == 0) ? cpuSumPrefix[p] : cpuSumFull[p];
                    const double sumSqC = (which == 0) ? cpuSumSqPrefix[p] : cpuSumSqFull[p];
                    double meanC = 0.0, varC = 0.0;
                    parityMomentsFromSums(sumC, sumSqC, m, meanC, varC);
                    const double sigma = std::sqrt(varG / static_cast<double>(r) +
                                                   varC / static_cast<double>(m));
                    const double d = meanG - meanC;
                    const double denom = std::max(sigma, sigmaFloor);
                    if (sigma < sigmaFloor) sigmaFloorBound = true;
                    const double z = std::abs(d) / denom;
                    sumDSq[which] += d * d;
                    if (z > worstZ[which]) {
                        worstZ[which] = z;
                        worstZPixel[which] = p;
                        if (which == 1) {
                            worstDetail[0] = meanG;
                            worstDetail[1] = meanC;
                            worstDetail[2] = d;
                            worstDetail[3] = sigma;
                            worstDetail[4] = std::sqrt(varG / static_cast<double>(r));
                            worstDetail[5] = std::sqrt(varC / static_cast<double>(m));
                        }
                    }
                    if (which == 1) {
                        minSigma = std::min(minSigma, sigma);
                        const double mu = std::max(std::abs(meanG), std::abs(meanC));
                        if (mu > 0.0) {
                            worstPixelResolution =
                                std::max(worstPixelResolution, kPerPixelZ * sigma / mu);
                        }
                    }
                }
            }
            const double rmsPrefix = std::sqrt(sumDSq[0] / static_cast<double>(kPixels));
            const double rmsFull = std::sqrt(sumDSq[1] / static_cast<double>(kPixels));

            // --- The pooled statistic, on the IMAGE TOTAL. Its variance is
            // taken across whole runs, so it makes no independence
            // assumption about pixels.
            std::vector<double> gpuImageTotals(kSeedsFull, 0.0);
            for (std::size_t i = 0; i < kSeedsFull; ++i) {
                double s = 0.0;
                for (uint32_t p = 0; p < kPixels; ++p) {
                    s += films[i][static_cast<std::size_t>(p) * 3u + 0u];
                }
                gpuImageTotals[i] = s;
            }
            double pooledZ[2] = {0.0, 0.0};
            double pooledDetail[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
            for (int which = 0; which < 2; ++which) {
                const std::size_t r = (which == 0) ? kSeedsPrefix : kSeedsFull;
                const std::size_t m = (which == 0) ? kCpuPrefix : kCpuFull;
                double meanS = 0.0, varS = 0.0, meanT = 0.0, varT = 0.0;
                parityMoments(gpuImageTotals.data(), r, meanS, varS);
                parityMoments(cpuImageTotals.data(), m, meanT, varT);
                const double sigma =
                    std::sqrt(varS / static_cast<double>(r) + varT / static_cast<double>(m));
                pooledZ[which] = (sigma > 0.0) ? std::abs(meanS - meanT) / sigma : 0.0;
                if (which == 1) {
                    pooledDetail[0] = meanS;
                    pooledDetail[1] = meanT;
                    pooledDetail[2] = meanS - meanT;
                    pooledDetail[3] = sigma;
                    pooledDetail[4] = std::sqrt(varS / static_cast<double>(r));
                    pooledDetail[5] = std::sqrt(varT / static_cast<double>(m));
                }
            }
            const double pooledResolution =
                (pooledDetail[0] != 0.0) ? kPooledZ * pooledDetail[3] / std::abs(pooledDetail[0])
                                         : std::numeric_limits<double>::infinity();

            const double convergenceRatio =
                (rmsPrefix > 0.0) ? rmsFull / rmsPrefix : std::numeric_limits<double>::infinity();

            const bool perPixelOk = worstZ[1] <= kPerPixelZ;
            const bool pooledOk = pooledZ[1] <= kPooledZ;
            const bool convergenceOk =
                convergenceRatio >= kConvergenceMin && convergenceRatio <= kConvergenceMax;
            const bool sharpEnough = pooledResolution <= kMaxPooledResolution &&
                                     worstPixelResolution <= kMaxPerPixelResolution;

            if (!perPixelOk || !pooledOk || !convergenceOk || !sharpEnough || sigmaFloorBound) {
                std::fprintf(
                    stderr,
                    "[diff_gpu_probe] FAIL: check %u -- wavefront integrator vs the independent "
                    "CPU reference, %s, %u bounces, %zu GPU runs x 1 spp vs %zu reference "
                    "samples per pixel:\n"
                    "  PER PIXEL: worst |mean_gpu - mean_ref| / sigma = %.3f at pixel %u "
                    "(allowed %.2f, family-wise 1e-3 over %u comparisons). There mean_gpu = "
                    "%.9g, mean_ref = %.9g, difference %.4g, sigma %.4g (gpu %.4g, ref %.4g).\n"
                    "  POOLED (image total, variance taken across whole runs so no "
                    "pixel-independence assumption): |%.9g - %.9g| = %.4g, sigma %.4g (gpu %.4g, "
                    "ref %.4g), z = %.3f (allowed %.2f).\n"
                    "  CONVERGENCE: rms(D) went %.4g -> %.4g when both sample counts were "
                    "quadrupled, ratio %.4f (predicted 0.5, window [%.2f, %.2f]). A ratio near 1 "
                    "is a FIXED BIAS surviving more samples; a ratio near 0 is a difference that "
                    "collapsed, which is what comparing a quantity against itself looks like.\n"
                    "  SHARPNESS: the pooled test resolves a coherent bias of %.3g relative "
                    "(gate %.3g); the worst pixel resolves %.3g relative (gate %.3g). A verdict "
                    "is refused above those, because passing would then be compatible with there "
                    "being nothing to detect.%s\n",
                    cfg.checkNumber, cfg.name, kBounces, kSeedsFull, kCpuFull, worstZ[1],
                    worstZPixel[1], kPerPixelZ, kPixels, worstDetail[0], worstDetail[1],
                    worstDetail[2], worstDetail[3], worstDetail[4], worstDetail[5],
                    pooledDetail[0], pooledDetail[1], pooledDetail[2], pooledDetail[3],
                    pooledDetail[4], pooledDetail[5], pooledZ[1], kPooledZ, rmsPrefix, rmsFull,
                    convergenceRatio, kConvergenceMin, kConvergenceMax, pooledResolution,
                    kMaxPooledResolution, worstPixelResolution, kMaxPerPixelResolution,
                    sigmaFloorBound ? "\n  The sigma floor BOUND on at least one pixel, so at "
                                      "least one comparison was made against an arithmetic "
                                      "guard rather than a measured variance."
                                    : "");
                return 1;
            }

            std::printf(
                "[diff_gpu_probe] OK: check %u -- the wavefront integrator (%zu independent "
                "1-spp runs through WavefrontLoop::record, %u bounces) and an INDEPENDENT CPU "
                "reference path tracer (%zu samples/pixel, its own intersector, basis, RNG and "
                "cosine-sampled MIS-free estimator -- no CDF, no pdfEnvMap, no balance "
                "heuristic) agree on a probe-owned scene, %s. All %u primary rays hit, so the "
                "film's missing directly-visible-environment term is identically zero here and "
                "the two sides hold the same quantity. Per pixel: worst |D|/sigma = %.3f at "
                "pixel %u over %u comparisons (bound %.2f, family-wise 1e-3); image total: "
                "%.6g vs %.6g, z = %.3f (bound %.2f). Agreement IMPROVES with samples: rms(D) "
                "%.4g -> %.4g on quadrupling both budgets, ratio %.4f against a predicted 0.5 "
                "(window [%.2f, %.2f]) -- a fixed bias could not do that. Non-vacuous: the "
                "pooled test resolves a coherent bias of %.3g relative (gate %.3g), the worst "
                "pixel %.3g (gate %.3g), and the smallest per-pixel sigma is %.3gx the "
                "arithmetic floor\n",
                cfg.checkNumber, kSeedsFull, kBounces, kCpuFull, cfg.name, kCapacity, worstZ[1],
                worstZPixel[1], kPixels, kPerPixelZ, pooledDetail[0], pooledDetail[1], pooledZ[1],
                kPooledZ, rmsPrefix, rmsFull, convergenceRatio, kConvergenceMin, kConvergenceMax,
                pooledResolution, kMaxPooledResolution, worstPixelResolution,
                kMaxPerPixelResolution,
                (sigmaFloor > 0.0) ? minSigma / sigmaFloor
                                   : std::numeric_limits<double>::infinity());
        }
    }

    // =======================================================================
    // 35-36. REPLAY EQUIVALENCE (Stage 1 Task 1). NO GRADIENT IS INVOLVED.
    // =======================================================================
    //
    // Stage 1 makes this renderer differentiable by path replay
    // backpropagation, and the single property everything else in it rests on
    // is this: the BACKWARD kernel must walk the identical path the FORWARD
    // kernel walked, consuming the identical RNG values in the identical
    // order. Spec section 6.2: "Divergence by a single RNG call means the
    // replayed path is a different path and every gradient is silently wrong
    // -- no crash, no NaN." Established here, BEFORE any adjoint exists,
    // because found later it would look like a bad df/dtheta derivation
    // rather than like a stream that went out of step.
    //
    // THE TWO RUNS. shaders/diff/wf_scatter.comp and
    // shaders/diff/wf_scatter_replay.comp are two instantiations of ONE
    // source (shaders/includes/diff/traverse.glsl), differing only in the
    // body of `diffVertexHook` -- which checkTraverseInstantiationTie()
    // above has already established structurally. Each is dispatched in the
    // same position of the same WavefrontLoop over the same closed-box scene,
    // in two INDEPENDENT runs from re-zeroed buffers with the same seed, and
    // each writes its own binding-3 vertex trace into its own buffer.
    //
    // THE ORACLE IS NOT THE OTHER RUN. Check 35 first validates the FORWARD
    // trace on its own, against ohao::diff::PathRng replayed on the CPU and
    // against analytic properties of the scene -- so that check 36's
    // bit-exact comparison cannot pass by both runs having written nothing.
    // The replay is handed no value the forward produced: it re-zeroes path
    // state, re-dispatches generate, and re-derives every vertex from
    // (pixelIndex, sampleIndex, iterationSeed) and the `bounce` it reads back
    // out of path state. That is the seed invariant (spec 4.5) and it is the
    // whole reason "replay" is possible without storing the path.
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 8;
        constexpr uint32_t kCapacity = kW * kH;  // 512
        constexpr float kAlbedo = 0.5f;
        constexpr uint32_t kIterationSeed = 20260828u;
        constexpr uint32_t kBounces = 4;
        // NOT a literal: getting this wrong makes the CPU oracle below walk a
        // different stream than the shader, which is the very failure this
        // pair of checks exists to detect -- so it would be an expected value
        // and a measured value positioned by the same mistake. It is the host
        // constant TIED to the traversal's own declaration by
        // checkDrawsPerBounceTie() above, which the probe refuses to run
        // without. (Checks 15 and 18 predate the tie and still state it as a
        // local literal; they are left untouched, and this constant being
        // tied is what makes their number checkable too.)
        constexpr uint32_t kDrawsPerBounce = ohao::diff::kDrawsPerBounce;
        constexpr uint32_t kStride = ohao::diff::kDebugDrawFloats;

        // Slot names, for diagnostics only. Kept beside TraceSlot rather than
        // derived from it because a printed name is not a check; the TIE that
        // makes these offsets mean anything is checkWfScatterSinkLayoutTie's
        // per-slot expectation table, which is parsed out of the shader.
        struct TraceSlotName {
            uint32_t offset;
            const char* name;
        };
        const TraceSlotName kSlotNames[] = {
            {ohao::diff::kTraceSlotU1, "u1"},
            {ohao::diff::kTraceSlotU2, "u2"},
            {ohao::diff::kTraceSlotDrawCount, "drawCount"},
            {ohao::diff::kTraceSlotULobe, "uLobe"},
            {ohao::diff::kTraceSlotUEnv1, "uEnv1"},
            {ohao::diff::kTraceSlotUEnv2, "uEnv2"},
            {ohao::diff::kTraceSlotOrigin + 0u, "origin.x"},
            {ohao::diff::kTraceSlotOrigin + 1u, "origin.y"},
            {ohao::diff::kTraceSlotOrigin + 2u, "origin.z"},
            {ohao::diff::kTraceSlotDir + 0u, "dir.x"},
            {ohao::diff::kTraceSlotDir + 1u, "dir.y"},
            {ohao::diff::kTraceSlotDir + 2u, "dir.z"},
            {ohao::diff::kTraceSlotThroughput + 0u, "throughput.r"},
            {ohao::diff::kTraceSlotThroughput + 1u, "throughput.g"},
            {ohao::diff::kTraceSlotThroughput + 2u, "throughput.b"},
            {ohao::diff::kTraceSlotHitT, "hitT"},
            {ohao::diff::kTraceSlotBounce, "bounce"},
            {ohao::diff::kTraceSlotPixelIndex, "pixelIndex"},
        };
        static_assert(sizeof(kSlotNames) / sizeof(kSlotNames[0]) == kStride,
                      "every trace slot must have a name for diagnostics");

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: replay probe buffers build\n");
            return 1;
        }

        std::vector<std::vector<float>> fwdTrace;
        std::vector<std::vector<float>> repTrace;
        if (!ctx.runWavefrontReplayProbe(wf, kW, kH, kBounces, kAlbedo, kIterationSeed, fwdTrace,
                                         repTrace)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: replay probe dispatch. Until "
                         "shaders/diff/wf_scatter_replay.comp exists there is no second "
                         "instantiation of the traversal, and this is the expected failure\n");
            wf.destroy(ctx.allocator());
            return 1;
        }
        if (fwdTrace.size() != kBounces || repTrace.size() != kBounces) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: replay probe returned %zu forward and %zu replay "
                         "traces, expected %u of each\n",
                         fwdTrace.size(), repTrace.size(), kBounces);
            wf.destroy(ctx.allocator());
            return 1;
        }

        // -------------------------------------------------------------------
        // 35. The FORWARD trace, validated against an oracle that is not the
        //     replay run: ohao::diff::PathRng on the CPU, plus the analytic
        //     properties of the closed-box scene. This is what stops check 36
        //     from being able to pass on two buffers of zeros -- or on two
        //     buffers of anything else that happens to match.
        // -------------------------------------------------------------------
        std::vector<uint32_t> pixelHits(kCapacity, 0u);
        // Every record's reported pixel index, kept so that the pixel-index
        // IDENTITY assertion can run AFTER the coverage histogram rather than
        // before it. See the note at the histogram itself: asserting the
        // identity first is what made the histogram unable to fail.
        std::vector<uint32_t> recordedPixel(static_cast<std::size_t>(kCapacity) * kBounces, 0u);
        double minU1 = 2.0;
        double maxU1 = -1.0;
        double maxDirLenErr = 0.0;
        float minHitT = std::numeric_limits<float>::infinity();
        for (uint32_t b = 0; b < kBounces; ++b) {
            if (fwdTrace[b].size() != static_cast<std::size_t>(kCapacity) * kStride) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: forward trace for bounce %u holds %zu floats, "
                             "expected %u\n",
                             b, fwdTrace[b].size(), kCapacity * kStride);
                wf.destroy(ctx.allocator());
                return 1;
            }
            // The arrival throughput at bounce b is exactly albedo^b: the
            // material is Config's pure-Lambertian default, whose per-bounce
            // estimator weight f*cos/pdf is exactly `albedo`, and the closed
            // box keeps every path alive so nothing skips a decay. Formed by
            // repeated float multiplication, not powf, for the same reason
            // check 17's 0.0625 is: it must be the SAME arithmetic the GPU
            // did, and at albedo 0.5 both are exact anyway.
            float expectedThroughput = 1.0f;
            for (uint32_t i = 0; i < b; ++i) expectedThroughput *= kAlbedo;

            for (uint32_t path = 0; path < kCapacity; ++path) {
                const float* rec = &fwdTrace[b][static_cast<std::size_t>(path) * kStride];

                // Every slot must be a finite number. A record full of NaN
                // would compare unequal to itself and so could not sneak
                // through check 36 -- but it would sneak through as
                // "different", and this says so with the right diagnosis.
                for (uint32_t i = 0; i < kStride; ++i) {
                    if (!std::isfinite(rec[i])) {
                        // Slots 2, 16 and 17 are bit-cast uints, whose float
                        // reinterpretation is meaningless; exclude them.
                        if (i == ohao::diff::kTraceSlotDrawCount ||
                            i == ohao::diff::kTraceSlotBounce ||
                            i == ohao::diff::kTraceSlotPixelIndex) {
                            continue;
                        }
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: forward trace bounce %u path %u slot "
                                     "%u (%s) is not finite (%g)\n",
                                     b, path, i, kSlotNames[i].name, static_cast<double>(rec[i]));
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                }

                // --- The five draws and the draw count, against PathRng.
                //
                // pathIndex == pixelIndex here: wf_generate.comp writes one
                // path per pixel at sampleIndex 0 and indexes path state by
                // the pixel index, which is also the one-sample-per-pixel
                // condition asserted by the histogram below.
                ohao::diff::PathRng cpu = ohao::diff::PathRng::forPath(path, /*sampleIndex=*/0u,
                                                                       kIterationSeed);
                for (uint32_t i = 0; i < b * kDrawsPerBounce; ++i) (void)cpu.next1D();
                const float expected[5] = {cpu.next1D(), cpu.next1D(), cpu.next1D(), cpu.next1D(),
                                           cpu.next1D()};
                const uint32_t expectedDraws = cpu.drawCount();
                const uint32_t drawSlots[5] = {
                    ohao::diff::kTraceSlotU1, ohao::diff::kTraceSlotU2,
                    ohao::diff::kTraceSlotULobe, ohao::diff::kTraceSlotUEnv1,
                    ohao::diff::kTraceSlotUEnv2};
                for (uint32_t i = 0; i < 5; ++i) {
                    if (rec[drawSlots[i]] != expected[i]) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: forward trace bounce %u path %u draw "
                                     "%u (%s): GPU %.9g, ohao::diff::PathRng %.9g. The shader's "
                                     "reconstruction from (pixelIndex, sampleIndex, "
                                     "iterationSeed) fast-forwarded by %u*%u draws does not "
                                     "reproduce the CPU stream, so there is no forward stream for "
                                     "a replay to be equal TO\n",
                                     b, path, i, kSlotNames[drawSlots[i]].name,
                                     static_cast<double>(rec[drawSlots[i]]),
                                     static_cast<double>(expected[i]), b, kDrawsPerBounce);
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                }
                uint32_t gpuDraws = 0;
                std::memcpy(&gpuDraws, &rec[ohao::diff::kTraceSlotDrawCount], sizeof(gpuDraws));
                if (gpuDraws != expectedDraws || gpuDraws != (b + 1) * kDrawsPerBounce) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: forward trace bounce %u path %u drawCount "
                                 "= %u, PathRng says %u, arithmetic says %u\n",
                                 b, path, gpuDraws, expectedDraws, (b + 1) * kDrawsPerBounce);
                    wf.destroy(ctx.allocator());
                    return 1;
                }

                // --- The vertex the traversal reached.
                uint32_t gpuBounce = 0;
                std::memcpy(&gpuBounce, &rec[ohao::diff::kTraceSlotBounce], sizeof(gpuBounce));
                if (gpuBounce != b) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: forward trace slot `bounce` = %u in the "
                                 "run of %u bounces, expected %u. The host is not comparing the "
                                 "bounce it thinks it is\n",
                                 gpuBounce, b + 1, b);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
                uint32_t gpuPixel = 0;
                std::memcpy(&gpuPixel, &rec[ohao::diff::kTraceSlotPixelIndex], sizeof(gpuPixel));
                // RANGE first (the histogram below indexes by this value, so
                // an out-of-range one would be a write past the end rather
                // than a diagnosis), then RECORD. The `gpuPixel == path`
                // identity is asserted after the loop, NOT here -- see the
                // histogram for why the order is the whole point.
                if (gpuPixel >= kCapacity) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: forward trace bounce %u path %u carries "
                                 "pixelIndex %u, which is not a pixel of the %u-pixel film this "
                                 "run allocates\n",
                                 b, path, gpuPixel, kCapacity);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
                recordedPixel[static_cast<std::size_t>(b) * kCapacity + path] = gpuPixel;
                if (b == 0) ++pixelHits[gpuPixel];

                const float hitT = rec[ohao::diff::kTraceSlotHitT];
                if (!(hitT > 0.0f)) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: forward trace bounce %u path %u has hitT "
                                 "= %g. Every ray from strictly inside a CLOSED box leaves it "
                                 "through a face, so a non-positive hit distance means the record "
                                 "is not describing a real vertex\n",
                                 b, path, static_cast<double>(hitT));
                    wf.destroy(ctx.allocator());
                    return 1;
                }
                minHitT = std::min(minHitT, hitT);

                for (uint32_t c = 0; c < 3; ++c) {
                    const float t = rec[ohao::diff::kTraceSlotThroughput + c];
                    if (t != expectedThroughput) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: forward trace bounce %u path %u "
                                     "arrival throughput component %u = %.9g, expected EXACTLY "
                                     "%.9g (albedo^%u). The traced throughput is not the path's, "
                                     "so the comparison below would be ranging over something "
                                     "other than the vertex it names\n",
                                     b, path, c, static_cast<double>(t),
                                     static_cast<double>(expectedThroughput), b);
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                }

                const double dx = rec[ohao::diff::kTraceSlotDir + 0u];
                const double dy = rec[ohao::diff::kTraceSlotDir + 1u];
                const double dz = rec[ohao::diff::kTraceSlotDir + 2u];
                const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
                maxDirLenErr = std::max(maxDirLenErr, std::abs(len - 1.0));
                minU1 = std::min(minU1, static_cast<double>(rec[ohao::diff::kTraceSlotU1]));
                maxU1 = std::max(maxU1, static_cast<double>(rec[ohao::diff::kTraceSlotU1]));
            }
        }
        if (maxDirLenErr > 1e-5) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: a forward-trace direction is off unit length by "
                         "%.3g. The recorded `dir` is not a ray direction\n",
                         maxDirLenErr);
            wf.destroy(ctx.allocator());
            return 1;
        }
        // NON-VACUITY of the whole comparison, stated as a measurement rather
        // than assumed: if every path drew the same u1 the trace would carry
        // no information and two runs would agree for a reason that has
        // nothing to do with replay. 512 independent streams spread over
        // [0,1) is what makes agreement meaningful.
        if (!(maxU1 - minU1 > 0.9)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: the forward trace's u1 spans only [%.6f, %.6f] "
                         "across %u paths x %u bounces. A trace with no variety cannot "
                         "distinguish a real replay from a stage that wrote a constant\n",
                         minU1, maxU1, kCapacity, kBounces);
            wf.destroy(ctx.allocator());
            return 1;
        }
        // THE FILM HAZARD, MEASURED (spec 4.5; the choice and its reasoning
        // are stated in wf_scatter.comp's hook). This subsystem resolved the
        // hazard by option (a): ONE SAMPLE PER PIXEL PER DISPATCH, so that
        // several paths of one pixel never atomicAdd the same three film
        // floats within a dispatch and the accumulation order stops depending
        // on the scheduler. runWavefrontReplayProbe refuses to run without
        // width*height == capacity; this is the other half -- the histogram
        // of what the paths ACTUALLY carried.
        //
        // WHY THIS RUNS BEFORE THE IDENTITY ASSERTION (review Finding 3).
        // This histogram used to be built AFTER a per-record
        // `gpuPixel == path` hard failure, which made it a tautology: given
        // that assertion, every record's pixel index was already known to be
        // its own path index, so incrementing a bin per path in [0, capacity)
        // could not produce anything but all-ones. It was structurally
        // incapable of failing, while this check's own OK line credited it
        // with measuring the coverage. The property was still enforced -- by
        // the identity assertion, which is a real comparison against a GPU
        // record -- but by the other assertion, not this one.
        //
        // The fix is ORDER, not deletion: the identity assertion is still
        // made, in full, immediately below, and nothing was weakened. What
        // changed is that the whole trace is now read into `recordedPixel`
        // and `pixelHits` FIRST, so this loop is a measurement of what 512
        // GPU records actually said before anything has asserted what they
        // must say. A stage that wrote `pixelIndex = pathIndex / 2` now fails
        // HERE, with "pixel 0 is covered by 2 paths", rather than being
        // intercepted one record earlier by the identity check. Both
        // assertions remain; only the vacuous one became capable of firing.
        for (uint32_t pix = 0; pix < kCapacity; ++pix) {
            if (pixelHits[pix] != 1u) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: pixel %u is covered by %u paths, expected "
                             "exactly 1. More than one sample per pixel inside one dispatch makes "
                             "the film's atomicAdd order scheduler-dependent, and float addition "
                             "is not associative -- which is the accumulation-order artefact spec "
                             "4.5 warns would present as an almost-right, irreproducible "
                             "gradient. This subsystem's resolution is that this never happens\n",
                             pix, pixelHits[pix]);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        // THE PIXEL-INDEX IDENTITY, deferred out of the record loop above so
        // that the coverage histogram measures something (see the note on
        // it). Unchanged in what it asserts: every record, every bounce.
        for (uint32_t b = 0; b < kBounces; ++b) {
            for (uint32_t path = 0; path < kCapacity; ++path) {
                const uint32_t gpuPixel =
                    recordedPixel[static_cast<std::size_t>(b) * kCapacity + path];
                if (gpuPixel != path) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: forward trace bounce %u path %u carries "
                                 "pixelIndex %u. At one sample per pixel wf_generate.comp indexes "
                                 "path state BY the pixel index, so these must be equal\n",
                                 b, path, gpuPixel);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }
        }
        std::printf(
            "[diff_gpu_probe] OK: check 35 -- the FORWARD traversal's vertex trace is real and "
            "independently correct: for all %u paths x %u bounces, all %u RNG draws per bounce "
            "and the draw count match ohao::diff::PathRng replayed on the CPU (a stream the GPU "
            "never sees), the recorded bounce and pixel indices are self-consistent, every "
            "arrival throughput is EXACTLY albedo^bounce, every hit distance is positive (min "
            "%.4f) and every direction is unit length to %.1e. Non-vacuous: u1 spans [%.4f, "
            "%.4f] across the %u streams, and every one of the %u pixels is covered by exactly "
            "one path -- the one-sample-per-pixel resolution of the film hazard (spec 4.5), "
            "histogrammed over the recorded indices BEFORE the pixelIndex == pathIndex identity "
            "is asserted, so the coverage is measured and not implied by that assertion\n",
            kCapacity, kBounces, kDrawsPerBounce, static_cast<double>(minHitT), maxDirLenErr,
            minU1, maxU1, kCapacity, kCapacity);

        // -------------------------------------------------------------------
        // 36. REPLAY EQUIVALENCE, bit for bit.
        //
        // Every slot of every record, for every path and every bounce, must
        // be BIT-identical between the two instantiations -- compared as raw
        // 32-bit patterns, not as floats, so that a difference of one ulp is
        // a failure and not a rounding excuse. The draws and the draw count
        // are the RNG stream; the origin, direction, throughput and hit
        // distance are the vertex that stream produced. "Same draws" without
        // "same vertex" would be a replay that consumed the right numbers and
        // went somewhere else; "same vertex" without "same draws" would be a
        // path that happened to land in the same place with a stream that has
        // already gone out of step for the NEXT bounce.
        // -------------------------------------------------------------------
        std::size_t comparedSlots = 0;
        for (uint32_t b = 0; b < kBounces; ++b) {
            if (repTrace[b].size() != fwdTrace[b].size()) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: replay trace for bounce %u holds %zu floats, "
                             "forward holds %zu\n",
                             b, repTrace[b].size(), fwdTrace[b].size());
                wf.destroy(ctx.allocator());
                return 1;
            }
            for (uint32_t path = 0; path < kCapacity; ++path) {
                const std::size_t base = static_cast<std::size_t>(path) * kStride;
                for (uint32_t i = 0; i < kStride; ++i) {
                    uint32_t fwdBits = 0;
                    uint32_t repBits = 0;
                    std::memcpy(&fwdBits, &fwdTrace[b][base + i], sizeof(fwdBits));
                    std::memcpy(&repBits, &repTrace[b][base + i], sizeof(repBits));
                    ++comparedSlots;
                    if (fwdBits == repBits) continue;
                    std::fprintf(
                        stderr,
                        "[diff_gpu_probe] FAIL: REPLAY DIVERGED. Bounce %u, path %u, trace slot "
                        "%u (%s): forward 0x%08x (%.9g), replay 0x%08x (%.9g).\n"
                        "  The replay kernel did NOT walk the path the forward kernel walked. "
                        "Under path replay backpropagation this is not a tolerance question: a "
                        "replayed path that differs anywhere is a DIFFERENT path, every "
                        "df/dtheta accumulated along it is evaluated at the wrong vertex, and "
                        "the result is a gradient that is silently wrong with no crash, no NaN "
                        "and no diagnostic (spec 6.2). If the diverging slot is one of the five "
                        "draws or the draw count, one instantiation consumed a different number "
                        "of RNG values than the other -- check that nothing outside "
                        "shaders/includes/diff/traverse.glsl draws, and that neither hook does.\n",
                        b, path, i, kSlotNames[i].name, fwdBits,
                        static_cast<double>(fwdTrace[b][base + i]), repBits,
                        static_cast<double>(repTrace[b][base + i]));
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }
        }
        std::printf(
            "[diff_gpu_probe] OK: check 36 -- the REPLAY instantiation walks the IDENTICAL path: "
            "%zu trace slots (%u paths x %u bounces x %u slots) are bit-identical between two "
            "independent runs of ohao::diff::WavefrontLoop, one through "
            "shaders/diff/wf_scatter.comp and one through "
            "shaders/diff/wf_scatter_replay.comp. Same five RNG draws per bounce, same draw "
            "count, same origin, same direction, same throughput, same hit distance -- compared "
            "as raw bit patterns, so one ulp is a failure. The replay run was handed nothing the "
            "forward run produced: it re-zeroed path state, re-dispatched generate and "
            "re-derived every vertex from (pixelIndex, sampleIndex, iterationSeed) and the "
            "bounce it read back out of path state\n",
            comparedSlots, kCapacity, kBounces, kStride);

        wf.destroy(ctx.allocator());
    }


    // -----------------------------------------------------------------
    // 37-38. THE FIRST GRADIENT: dJ/d(albedo) against a common-random-number
    // finite difference, and the null test.
    // -----------------------------------------------------------------
    //
    // WHAT IS MEASURED. J(a) = the sum of every float in the film -- every
    // pixel, every channel -- produced by a fused `bounces`-bounce run of
    // ohao::diff::WavefrontLoop at ONE seed, one sample per pixel. The
    // analytic side is what shaders/diff/wf_scatter_replay.comp's hook
    // scattered into the gradient arena on a SEPARATE run of the same loop at
    // the same seed. The finite-difference side is
    // (J(a+h) - J(a-h)) / (2h) from two more runs at the same seed.
    //
    // WHY THIS IS AN EXACT COMPARISON AND NOT A STATISTICAL ONE. Common random
    // numbers: all three renders walk the SAME paths, because the albedo does
    // not enter any sampling decision at metallic == 0 (the lobe probability q
    // is built from F0 = 0.04 there, not from the base colour) and every stage
    // rebuilds its RNG from (pixelIndex, sampleIndex, iterationSeed). So this
    // is the derivative of ONE realisation of the estimator compared against
    // the analytic derivative of that same realisation -- there is no sampling
    // variance in the error budget at all, and the tolerance is pure
    // arithmetic. `runWavefrontGradientProbe` REFUSES to run at metallic != 0
    // or specularWeight != 0 rather than leaving that a comment.
    //
    // THE STEP SIZE, DERIVED. kStep = 2^-7 = 0.0078125, and here is where it
    // comes from. The two error terms of a central difference are
    //
    //     roundoff    ~ eps * |J| / h                (cancellation; grows as h shrinks)
    //     truncation  ~ |J| * max_n E_n(h)/a^n       (odd derivatives; grows as h grows)
    //
    // with E_n(h) = ((a+h)^n - (a-h)^n)/(2h) - n*a^(n-1). In THIS configuration
    // J is an exact polynomial in a: a pure Lambertian surface makes the
    // arrival throughput at bounce b exactly a^b and the direct estimate at
    // that vertex exactly linear in a, so J(a) = SUM_{n=1..B} K_n a^n with
    // every K_n >= 0. Relative to |J'| >= J/a, and keeping only the leading
    // term of E_n (which is (n choose 3) h^2 a^(n-3), so E_n/a^n ~ c h^2/a^3):
    //
    //     E(h)/|J'|  ~  eps*a/h  +  c*h^2/a^2,      minimised at
    //     h*         =  (eps * a^3 / (2c))^(1/3)
    //
    // -- a CUBE ROOT of the precision, which is why the answer is near 1e-2
    // and not near 1e-7. With eps = 2e-6 (see below), a = 0.6 and c = 1 (the
    // B = 3 case, where E_3(h) = h^2 exactly, so E_3/a^3 = h^2/a^3):
    //
    //     h*    = (2e-6 * 0.216 / 2)^(1/3) = (2.16e-7)^(1/3) = 6.0e-3
    //     E(h*) = 2e-6*0.6/6.0e-3 + (6.0e-3)^2/0.36
    //           = 2.00e-4 + 1.00e-4 = 3.0e-4      (relative to J')
    //
    // kStep = 2^-7 = 7.8125e-3 is the nearest power of two, giving
    // E = 1.54e-4 + 1.70e-4 = 3.2e-4 -- within 7% of the minimum, so the
    // choice is not sensitive to the estimate of eps. A power of two is used
    // so that a +/- h is exact in float32 (0.6f has ulp 2^-24, and 2^-7 is
    // representable in its low bits), and the harness divides by the ACTUAL
    // float difference regardless, so the representation of a itself cancels.
    //
    // eps = kFilmRelativeEps = 2e-6, or about 32 float32 ulp. Derivation: a
    // film value is a sum over `bounces` vertices of a product of about six
    // float32 factors (throughput, MIS weight, f*cos, radiance, visibility,
    // 1/pdf), each rounding at 2^-24 = 6.0e-8; a few tens of roundings is
    // 2e-6. It is a BOUND on the film's relative accuracy, not a measured
    // reproducibility -- two runs at one albedo are bit-identical, which says
    // nothing about the distance from exact arithmetic.
    //
    // THE VERDICT IS |FD - analytic| <= (the bound the harness computed from
    // this run's own J and h). No safety factor is applied and no tolerance
    // was widened until it passed: both terms are computed from the numbers
    // the run produced, and the truncation half is exact-arithmetic-tight for
    // this polynomial family (see the harness header).
    //
    // NON-VACUITY, PRE-REGISTERED. A verdict means nothing unless the gate
    // could have failed. Two gates:
    //   * kMaxGradientResolution -- the error bound, as a fraction of the
    //     analytic gradient, must be below 1e-2. That is the resolution
    //     claim: this check can resolve a 1% error in the gradient, which is
    //     exactly what Step 5's demonstration perturbs by.
    //   * The film must be strictly positive and the gradient strictly
    //     positive: a scene that produced no light would let 0 == 0 pass.
    // The check also reports analytic*a/J, which is 1 exactly for a
    // single-bounce film (J is then linear in a) and strictly greater than 1
    // whenever the higher bounces contribute -- so the OK line SHOWS that the
    // multi-bounce terms, including the throughput-derivative term
    // bsdf_adjoint.glsl adds, are carrying real weight rather than rounding.
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 8;  // wf_generate's 1-D dispatch requires this exactly.
        constexpr uint32_t kCapacity = kW * kH;
        constexpr uint32_t kEnvW = 64;
        constexpr uint32_t kEnvH = 32;
        static_assert(kEnvW != kEnvH, "a square environment hides a W<->H swap");
        constexpr float kAlbedo = 0.6f;
        constexpr float kStep = 0.0078125f;  // 2^-7 -- derived above
        constexpr double kFilmRelativeEps = 2e-6;
        constexpr uint32_t kGradientSeed = 20260828u;
        constexpr double kMaxGradientResolution = 1e-2;
        // One, two and three bounces. One is the case the recursion in
        // DiffVertex is complete for on its own (J is linear in a, the central
        // difference is EXACT, and the truncation bound is identically zero);
        // two and three are where the throughput-derivative term
        // bsdf_adjoint.glsl adds is load-bearing. Running all three is what
        // distinguishes "the direct scatter line is right" from "the whole
        // derivative is right", and the OK line reports both.
        constexpr uint32_t kBounceCounts[3] = {1u, 2u, 3u};

        // --- The parameters. ONE ScalarBlock for the albedo, and a SECOND
        // one the scene does not depend on and nothing scatters into, which
        // is the null test's subject. ParamRegistry allocates two blocks per
        // parameter -- the gradient block, then the Adam m/v state block --
        // so registering two parameters lays out four blocks, of which
        // exactly ONE is ever written.
        ohao::diff::ParamRegistry gradReg;
        const auto regAlbedo = gradReg.registerScalarBlock("albedo", 1);
        const auto regUnused = gradReg.registerScalarBlock("unused_scalar", 1);
        if (!regAlbedo.ok || !regUnused.ok) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 37 registry setup: %s %s\n",
                         regAlbedo.error.c_str(), regUnused.error.c_str());
            return 1;
        }
        const ohao::diff::DiffParam* albedoParam = gradReg.find("albedo");
        const ohao::diff::DiffParam* unusedParam = gradReg.find("unused_scalar");
        if (albedoParam == nullptr || unusedParam == nullptr) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 37 registered params not found\n");
            return 1;
        }

        ohao::diff::GradientArena gradArena;
        if (!gradArena.build(ctx.allocator(), gradReg.layout())) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 37 gradient arena build\n");
            return 1;
        }
        // What the shader is told. The offset is a FLOAT index, derived from
        // the layout's byte offset -- the one place the host's picture of the
        // arena and the shader's addressing meet. The arena's blocks are
        // 256-byte aligned, so this is always a whole number of floats.
        const ohao::diff::ArenaBlock albedoGradBlock =
            gradReg.layout().block(albedoParam->gradBlock);
        const uint32_t kGradArenaFloats =
            static_cast<uint32_t>(gradReg.layout().totalBytes() / sizeof(float));
        const uint32_t kGradAlbedoOffset =
            static_cast<uint32_t>(albedoGradBlock.offsetBytes / sizeof(float));

        std::vector<float> envRgba;
        std::vector<double> envLum;
        buildParityEnvironment(kEnvW, kEnvH, envRgba, envLum);
        std::vector<float> positions;
        std::vector<uint32_t> indices;
        buildParityScene(positions, indices);
        const ohao::diff::WavefrontGenerateCamera camera = parityCamera();

        ohao::EnvCDF gradEnvCdf;
        gradEnvCdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
        if (!gradEnvCdf.valid()) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 37 EnvCDF::build produced no CDF\n");
            gradArena.destroy(ctx.allocator());
            return 1;
        }

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH) ||
            !wf.uploadEnvironment(gradEnvCdf.marginalSpan(), gradEnvCdf.conditionalSpan(),
                                  gradEnvCdf.integral())) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 37 buffers build / env CDF upload\n");
            wf.destroy(ctx.allocator());
            gradArena.destroy(ctx.allocator());
            return 1;
        }

        // Pure Lambert. Not a default: runWavefrontGradientProbe refuses
        // anything else, and bsdf_adjoint.glsl's header says why in both
        // directions.
        const ohao::diff::WavefrontScatterMaterial kGradMaterial{1.0f, 0.0f, 0.0f};

        CrnFdMeasurement measurements[3]{};
        double worstRatio = 0.0;
        double worstResolution = 0.0;

        for (std::size_t i = 0; i < 3; ++i) {
            const uint32_t bounces = kBounceCounts[i];
            CrnFdMeasurement& m = measurements[i];
            if (!measureCrnAlbedoGradient(ctx, wf, kW, kH, bounces, camera, positions, indices,
                                          kAlbedo, kStep, kGradMaterial, kGradientSeed, gradArena,
                                          albedoParam->gradBlock, kGradArenaFloats,
                                          kGradAlbedoOffset, kFilmRelativeEps, m)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 37 measurement failed at %u bounce(s)\n",
                             bounces);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }

            // --- NON-VACUITY 1: there is light, and there is a gradient.
            if (!(m.jCenter > 0.0) || !std::isfinite(m.jCenter) || !(m.analytic > 0.0) ||
                !std::isfinite(m.analytic)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 37 at %u bounce(s): J = %.9g and the "
                             "scattered gradient = %.9g. Both must be finite and strictly "
                             "positive -- every factor of both is non-negative and the scene is "
                             "lit, so a zero on either side means nothing was accumulated and "
                             "the comparison below would be 0 against 0\n",
                             bounces, m.jCenter, m.analytic);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }

            // --- NON-VACUITY 2: the gate's resolution, pre-registered.
            const double resolution = m.errorBound / m.analytic;
            if (resolution > worstResolution) worstResolution = resolution;
            if (!(resolution <= kMaxGradientResolution)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 37 at %u bounce(s) REFUSES TO CLAIM A "
                             "VERDICT: the derived error bound is %.6g, which is %.3g of the "
                             "gradient %.9g -- above the pre-registered %.3g. A pass at this "
                             "resolution would be compatible with there being nothing it could "
                             "have detected. roundoff %.6g + truncation %.6g at h = %.9g\n",
                             bounces, m.errorBound, resolution, m.analytic,
                             kMaxGradientResolution, m.roundoffBound, m.truncationBound,
                             m.hActual);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }

            // --- THE GATE.
            const double ratio = m.absError / m.errorBound;
            if (ratio > worstRatio) worstRatio = ratio;
            if (!(m.absError <= m.errorBound)) {
                std::fprintf(
                    stderr,
                    "[diff_gpu_probe] FAIL: check 37 at %u bounce(s) -- THE ANALYTIC GRADIENT IS "
                    "NOT THE DERIVATIVE OF THE FILM.\n"
                    "  finite difference (J(a+h) - J(a-h)) / 2h = %.12g\n"
                    "  gradient scattered into the arena         = %.12g\n"
                    "  |difference| = %.6g, which is %.6g of the gradient\n"
                    "  derived error bound = %.6g (roundoff %.6g + truncation %.6g)\n"
                    "  J(a-h) = %.12g, J(a) = %.12g, J(a+h) = %.12g, h = %.12g\n"
                    "  Both sides describe ONE realisation of the estimator at seed %u under "
                    "common random numbers, so there is no sampling error to absorb this: the "
                    "two numbers are the derivative of the same function computed two ways, and "
                    "they disagree. WHICH bounce counts fail localises it: if only the ONE-bounce "
                    "measurement fails, the DIRECT scatter line (DiffVertex's recursion) is "
                    "wrong; if one bounce passes and two/three fail, the throughput-derivative "
                    "term is. If all three fail by a common factor, "
                    "suspect one of the eight per-strategy fields of DiffVertex before "
                    "suspecting the derivative -- nothing else in this repository reads them.\n",
                    bounces, m.finiteDiff, m.analytic, m.absError, m.relError, m.errorBound,
                    m.roundoffBound, m.truncationBound, m.jMinus, m.jCenter, m.jPlus, m.hActual,
                    kGradientSeed);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }
        }

        // --- NON-VACUITY 3: the higher bounces actually contribute. For a
        // one-bounce film J is exactly linear in a, so a*J'/J is exactly 1;
        // every extra bounce raises it. If the 3-bounce run's value were also
        // 1, the second and third bounces would be contributing nothing and
        // the two of the three measurements that exercise the
        // throughput-derivative term would be measuring a term that is zero.
        const double shape1 = kAlbedo * measurements[0].analytic / measurements[0].jCenter;
        const double shape3 = kAlbedo * measurements[2].analytic / measurements[2].jCenter;
        if (!(shape3 > shape1 + 0.05)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 37 -- a*J'/J is %.6f at one bounce and "
                         "%.6f at three. It is exactly 1 for a film that is linear in the albedo "
                         "and rises with every bounce that contributes, so these being equal "
                         "means the multi-bounce terms carry no weight and the 2- and 3-bounce "
                         "measurements are not exercising the throughput-derivative term at "
                         "all\n",
                         shape1, shape3);
            wf.destroy(ctx.allocator());
            gradArena.destroy(ctx.allocator());
            return 1;
        }

        std::printf(
            "[diff_gpu_probe] OK: check 37 -- the gradient shaders/diff/wf_scatter_replay.comp "
            "scatters into the arena IS the derivative of the film shaders/diff/wf_scatter.comp "
            "accumulates. Common random numbers, seed %u, %u paths at one sample per pixel, "
            "h = 2^-7 = %.9g (derived: the minimiser of eps*a/h + h^2/a^2 at eps = %.0e is "
            "6.0e-3; this is the nearest power of two).\n"
            "    1 bounce : FD %.9g vs analytic %.9g -- |err| %.4g <= bound %.4g "
            "(roundoff %.4g + truncation %.4g)\n"
            "    2 bounces: FD %.9g vs analytic %.9g -- |err| %.4g <= bound %.4g "
            "(roundoff %.4g + truncation %.4g)\n"
            "    3 bounces: FD %.9g vs analytic %.9g -- |err| %.4g <= bound %.4g "
            "(roundoff %.4g + truncation %.4g)\n"
            "  Worst |err|/bound %.4g; worst bound/gradient %.3g (pre-registered limit %.3g, so "
            "the gate resolves better than 1%% and would reject Step 5's 1.01 scaling). "
            "a*J'/J rises from %.4f at one bounce to %.4f at three, so the throughput-derivative "
            "term is carrying real weight and not rounding\n",
            kGradientSeed, kCapacity, static_cast<double>(kStep), kFilmRelativeEps,
            measurements[0].finiteDiff, measurements[0].analytic, measurements[0].absError,
            measurements[0].errorBound, measurements[0].roundoffBound,
            measurements[0].truncationBound, measurements[1].finiteDiff,
            measurements[1].analytic, measurements[1].absError, measurements[1].errorBound,
            measurements[1].roundoffBound, measurements[1].truncationBound,
            measurements[2].finiteDiff, measurements[2].analytic, measurements[2].absError,
            measurements[2].errorBound, measurements[2].roundoffBound,
            measurements[2].truncationBound, worstRatio, worstResolution,
            kMaxGradientResolution, shape1, shape3);

        // -----------------------------------------------------------------
        // 38. THE NULL TEST. Exactly zero, not "small".
        // -----------------------------------------------------------------
        //
        // The arena holds FOUR blocks: albedo's gradient, albedo's Adam m/v
        // state, the unused parameter's gradient, and the unused parameter's
        // state. The scatter addresses exactly one of them, through
        // ScatterPush::gradAlbedoOffset. So the other three must come back
        // BIT-EXACTLY zero after a run that wrote a large gradient into the
        // first -- and it is bit-exact rather than a tolerance because
        // nothing added anything to them at all: `GradientArena::zero` filled
        // them and no atomicAdd named them.
        //
        // WHAT THIS CATCHES THAT THE GATE ABOVE DOES NOT. A scatter into the
        // wrong element is not necessarily visible to check 37: if the offset
        // were wrong by a whole block the gate would read zero from the albedo
        // block and fail -- but an offset wrong by an amount that lands
        // somewhere else in the arena, or a stray second write, leaves the
        // gate's number intact and corrupts something the gate never reads.
        // That is exactly the failure Task 5's per-texel scatter can make at
        // scale, and this is the cheapest statement of "the arena is
        // addressed, not sprayed".
        //
        // IT READS THE WHOLE ARENA, NOT THE OTHER BLOCKS. For one task this
        // check read back the three unwritten BLOCKS -- five floats of a
        // 256-float arena -- while its own rationale named the 256-byte
        // alignment PADDING as the place a wrong offset would land. The
        // padding was not examined at all, so the rationale claimed a failure
        // the check could not catch. The fix is to make the check deliver:
        // every float of the arena is read and every one of them except the
        // single element the scatter is addressed at must be exactly zero.
        // That covers the three null blocks as before AND the 251 padding
        // floats between and after them, which is where a mis-computed
        // per-texel k is likeliest to land.
        //
        // It also states the block layout Task 5 must fit: ONE float per
        // ScalarBlock element at gradAlbedoOffset + k, blocks in registration
        // order, gradient block before state block, 256-byte aligned.
        //
        // The two named-block spans are kept as a SEPARATE statement so the
        // failure text can say WHICH parameter was corrupted when the stray
        // write lands in a block rather than in padding -- a bare arena index
        // would not.
        struct NullBlock {
            const char* name;
            std::size_t index;
            std::size_t expectedFloats;
        };
        const NullBlock nullBlocks[3] = {
            {"albedo's Adam m/v state", albedoParam->stateBlock, albedoParam->floatCount * 2u},
            {"unused_scalar's gradient", unusedParam->gradBlock, unusedParam->floatCount},
            {"unused_scalar's Adam m/v state", unusedParam->stateBlock,
             unusedParam->floatCount * 2u},
        };

        const std::vector<float> wholeArena = gradArena.readbackAll(ctx.allocator());
        if (wholeArena.size() != static_cast<std::size_t>(kGradArenaFloats)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 38 -- the whole-arena readback returned "
                         "%zu floats, expected %u. A null test over the wrong number of floats "
                         "is not a null test\n",
                         wholeArena.size(), kGradArenaFloats);
            wf.destroy(ctx.allocator());
            gradArena.destroy(ctx.allocator());
            return 1;
        }

        // Which arena float belongs to which named block, for the failure
        // text only. Anything not covered here is alignment padding.
        auto describeArenaFloat = [&](std::size_t f) -> std::string {
            for (const NullBlock& nb : nullBlocks) {
                const ohao::diff::ArenaBlock b = gradReg.layout().block(nb.index);
                const std::size_t first = b.offsetBytes / sizeof(float);
                const std::size_t count = b.sizeBytes / sizeof(float);
                if (f >= first && f < first + count) {
                    return std::string(nb.name) + ", element " + std::to_string(f - first) +
                           " of arena block " + std::to_string(nb.index);
                }
            }
            return "256-byte alignment padding owned by no block";
        };

        std::size_t nullFloatsChecked = 0;
        for (std::size_t f = 0; f < wholeArena.size(); ++f) {
            if (f == static_cast<std::size_t>(kGradAlbedoOffset)) continue;  // the one written
            ++nullFloatsChecked;
            if (wholeArena[f] != 0.0f) {
                std::fprintf(
                    stderr,
                    "[diff_gpu_probe] FAIL: check 38 -- arena float %zu (%s) is %.9g and must be "
                    "EXACTLY 0. The traversal's only arena write is one atomicAdd at "
                    "ScatterPush::gradAlbedoOffset + k with k = 0, i.e. arena float %u, which is "
                    "element 0 of block %zu. A non-zero anywhere else is a scatter that landed "
                    "outside the element it was told to write -- which check 37 is blind to, "
                    "since it reads only that one float\n",
                    f, describeArenaFloat(f).c_str(), static_cast<double>(wholeArena[f]),
                    kGradAlbedoOffset, albedoParam->gradBlock);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }
        }

        // Cross-check: the three null blocks really are inside what was just
        // scanned, so "the whole arena" is not quietly a smaller set than the
        // block-wise test this replaced.
        std::size_t namedNullFloats = 0;
        for (const NullBlock& nb : nullBlocks) {
            const ohao::diff::ArenaBlock b = gradReg.layout().block(nb.index);
            const std::size_t first = b.offsetBytes / sizeof(float);
            const std::size_t count = b.sizeBytes / sizeof(float);
            if (count != nb.expectedFloats || first + count > wholeArena.size()) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 38 -- block %zu (%s) spans floats "
                             "[%zu, %zu) of a %zu-float arena and holds %zu floats, expected %zu. "
                             "The whole-arena scan cannot claim to cover a block it does not "
                             "contain\n",
                             nb.index, nb.name, first, first + count, wholeArena.size(), count,
                             nb.expectedFloats);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }
            namedNullFloats += count;
        }

        std::printf(
            "[diff_gpu_probe] OK: check 38 -- the null test: after a run that accumulated %.9g "
            "into arena float %u (element 0 of the albedo's gradient block, block %zu), ALL %zu "
            "other floats of the %u-float arena are EXACTLY 0.0f, compared as floats and not "
            "through a tolerance. That is the whole arena, not just the %zu floats of the three "
            "unwritten blocks -- albedo's Adam m/v state and both blocks of a second registered "
            "parameter the scene does not depend on -- so the %zu floats of 256-byte alignment "
            "padding, which is where a mis-computed element index is likeliest to land, are "
            "covered too. This is the statement Stage 1 Task 5's per-texel scatter has to keep: "
            "one atomicAdd per element at gradBlockOffset + k, blocks in registration order "
            "(gradient then Adam state), 256-byte aligned, and nothing anywhere else\n",
            measurements[2].analytic, kGradAlbedoOffset, albedoParam->gradBlock, nullFloatsChecked,
            kGradArenaFloats, namedNullFloats, nullFloatsChecked - namedNullFloats);

        wf.destroy(ctx.allocator());
        gradArena.destroy(ctx.allocator());
    }

    // -----------------------------------------------------------------
    // 39-41. THE GGX ADJOINTS: dJ/d(roughness), dJ/d(metallic), and the
    //        detached-sampling bias.
    // -----------------------------------------------------------------
    //
    // WHAT IS MEASURED, AND AGAINST WHAT. Same J as check 37 -- the sum of
    // every float of the film a fused `bounces`-bounce run of
    // ohao::diff::WavefrontLoop produced at ONE seed, one sample per pixel --
    // but differentiated with respect to the pushed roughness or the pushed
    // metallic, and against a DETACHED finite difference. The instrument and
    // the reason it had to change are in `measureDetachedGgxGradient`'s
    // header; the short of it is that perturbing either parameter moves the
    // sampled direction, spec section 6.3 does not differentiate sampled
    // directions, so the reference must hold the directions still or it
    // measures a term the adjoint deliberately omits.
    //
    // THE STEP SIZES, DERIVED, AND WHY ONE WOULD NOT SERVE BOTH.
    //
    // The central difference D(h) = (J(t+h) - J(t-h))/(2h) carries the two
    // errors Task 2's harness derives:
    //
    //     E(h) ~ eps |J| / h   +   |J'''| h^2 / 6
    //
    // Write L for the length scale on which J varies in the parameter, so that
    // |J| ~ |J'| L and |J'''| ~ |J'| / L^2. Then, relative to |J'|,
    //
    //     E(h)/|J'|  ~  eps L / h  +  h^2 / (6 L^2)
    //
    // whose minimiser is h* = (3 eps)^(1/3) L = 0.0182 L at eps = 2e-6, with
    // E(h*)/|J'| = 1.10e-4 + 5.5e-5 = 1.65e-4. The whole question is therefore
    // "what is L", and the two parameters answer it differently by up to a
    // factor of 30.
    //
    //   METALLIC. Every appearance of metallic is low-order polynomial over
    //   the WHOLE of [0,1]: F0 = 0.04 + m(baseColor - 0.04) is linear,
    //   specScale = mix(specularWeight,1,m) is linear, the diffuse lobe's
    //   (1-m) is linear, and q is a cubic in m. There is no small parameter,
    //   so L_m = 1 and h*_m = 1.8e-2. kMetallicStep = 2^-6 = 1.5625e-2 is the
    //   nearest power of two, giving E/|J'| = 1.28e-4 + 4.1e-5 = 1.7e-4.
    //
    //   ROUGHNESS. Everything that matters -- D, both Smith Lambdas, and the
    //   specular half of the mixture density -- is a function of
    //   alpha = roughness^2, and the GGX lobe's angular width IS alpha. A step
    //   dr changes alpha by 2 r dr, i.e. by a RELATIVE 2 dr / r, so J varies
    //   in r on the scale L_r = r/2 and
    //
    //       h*_r = 0.0182 * r / 2 = 0.0091 r
    //
    //   -- proportional to the roughness itself. At r = 0.60 that is 5.4e-3
    //   (2^-8); the A-PRIORI estimate at r = 0.04 is 3.6e-4 (2^-11).
    //
    //   THE NEAR-SPECULAR CASE'S STEP IS CORRECTED FROM THAT A-PRIORI VALUE.
    //   At h = 2^-11 kMaxGgxResolution's non-vacuity guard (below) refused to
    //   claim a verdict: error bound 4.248 against a gradient of 287.7, i.e.
    //   1.48% -- above the pre-registered 1%. The two halves were
    //   roundoff 4.179 + truncation 0.0688, a ratio of 61:1, where the E(h)
    //   model above is minimised at roundoff = 2*truncation. That imbalance
    //   is the model's OWN diagnostic that L = r/2 underestimated the true
    //   scale for this case: the film integrates over many directions and
    //   the sharp lobe is a modest part of J, so J varies more slowly in r
    //   than alpha's local geometry alone predicts. The two-term model gives
    //   the correction from measured quantities alone --
    //
    //       h_opt = h * (roundoff / (2*truncation))^(1/3)
    //             = 2^-11 * (4.179 / (2*0.0688))^(1/3)
    //             = 2^-11 * 3.12 = 1.52e-3
    //
    //   -- and 2^-9 = 1.953e-3 is the nearest power of two. At 2^-9 the
    //   halves are roundoff 1.045 + truncation 1.148, balanced as the model
    //   predicts, and the resolution is 0.76%. That is the correction
    //   kParamRoughness's fourth case carries below.
    //
    //   WHY THIS IS A MEASUREMENT, NOT A FIT TOWARD A PASSING COMPARISON.
    //   roundoffBound and truncationBound (GgxFdMeasurement, computed in
    //   measureDetachedGgxGradient above) are functions of jPlus, jMinus,
    //   jPlus2, jMinus2 and hActual alone -- the five FORWARD renders and
    //   the step -- and read `out.analytic`, the scattered gradient, NOWHERE.
    //   So h was re-derived from a run that computes no gradient at all: the
    //   E(h) model itself (derived above) was fixed BEFORE any case ran, and
    //   only its input L was re-estimated here, from adjoint-independent
    //   measurements. A step chosen to make the comparison pass would have
    //   had to read `analytic` to know what to aim at, and this derivation
    //   never does.
    //
    //   Using metallic's 2^-6 at r = 0.04 would perturb alpha by +/-78% and
    //   measure a chord across the whole lobe rather than a derivative;
    //   using the near-specular case's own (corrected) 2^-9 for metallic
    //   would put the difference quotient's cancellation error 8x higher for
    //   no truncation benefit. Each case below therefore carries its own
    //   step, derived from its own r.
    //
    // A POWER OF TWO in every case, so that t +/- h and t +/- 2h are exact in
    // float32 and the doubling Richardson needs is exact too; the harness
    // divides by the ACTUAL float difference regardless.
    //
    // THE ERROR BOUND is roundoffBound + truncationBound with the roundoff
    // half computed exactly as check 37's (eps = 2e-6, same derivation, same
    // kind of film) and the truncation half RICHARDSON-MEASURED from the run's
    // own D(h) and D(2h) -- because J is not a polynomial in these parameters
    // and Task 2's exact polynomial bound has no counterpart. See the
    // harness's header for the direction it errs in.
    //
    // NON-VACUITY, PRE-REGISTERED, four ways:
    //   * kMaxGgxResolution -- the bound, as a fraction of |analytic|, must be
    //     below 1e-2, so the gate resolves better than 1% and could reject
    //     Step 5's per-term perturbations.
    //   * The film must be strictly positive and the gradient strictly
    //     non-zero and finite: 0 == 0 is not a pass.
    //   * traceMismatches == 0 -- the instrument's own claim, MEASURED. Every
    //     one of the five renders must have walked the bit-identical path.
    //     If it did not, the difference quotient is not the detached
    //     derivative and the comparison below means nothing.
    //   * Check 41 measures the naive difference on the same cases and
    //     REQUIRES its traceMismatches to be non-zero -- otherwise freezing
    //     the sampling material changed nothing and the whole instrument is a
    //     no-op that the three checks above would not notice.
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 8;  // wf_generate's 1-D dispatch requires this exactly.
        constexpr uint32_t kCapacity = kW * kH;
        constexpr uint32_t kEnvW = 64;
        constexpr uint32_t kEnvH = 32;
        static_assert(kEnvW != kEnvH, "a square environment hides a W<->H swap");
        constexpr float kAlbedo = 0.6f;
        constexpr double kFilmRelativeEps = 2e-6;
        constexpr uint32_t kGgxSeed = 20260829u;
        constexpr double kMaxGgxResolution = 1e-2;
        constexpr uint32_t kParamRoughness = 1u;
        constexpr uint32_t kParamMetallic = 2u;

        ohao::diff::ParamRegistry ggxReg;
        const auto regGgx = ggxReg.registerScalarBlock("ggx_scalar", 1);
        if (!regGgx.ok) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 39 registry setup: %s\n",
                         regGgx.error.c_str());
            return 1;
        }
        const ohao::diff::DiffParam* ggxParam = ggxReg.find("ggx_scalar");
        if (ggxParam == nullptr) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 39 registered param not found\n");
            return 1;
        }

        ohao::diff::GradientArena ggxArena;
        if (!ggxArena.build(ctx.allocator(), ggxReg.layout())) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 39 gradient arena build\n");
            return 1;
        }
        const ohao::diff::ArenaBlock ggxBlock = ggxReg.layout().block(ggxParam->gradBlock);
        const uint32_t kGgxArenaFloats =
            static_cast<uint32_t>(ggxReg.layout().totalBytes() / sizeof(float));
        const uint32_t kGgxOffset = static_cast<uint32_t>(ggxBlock.offsetBytes / sizeof(float));

        std::vector<float> envRgba;
        std::vector<double> envLum;
        buildParityEnvironment(kEnvW, kEnvH, envRgba, envLum);
        std::vector<float> positions;
        std::vector<uint32_t> indices;
        buildParityScene(positions, indices);
        const ohao::diff::WavefrontGenerateCamera camera = parityCamera();

        ohao::EnvCDF ggxEnvCdf;
        ggxEnvCdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
        if (!ggxEnvCdf.valid()) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 39 EnvCDF::build produced no CDF\n");
            ggxArena.destroy(ctx.allocator());
            return 1;
        }

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH) ||
            !wf.uploadEnvironment(ggxEnvCdf.marginalSpan(), ggxEnvCdf.conditionalSpan(),
                                  ggxEnvCdf.integral())) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 39 buffers build / env CDF upload\n");
            wf.destroy(ctx.allocator());
            ggxArena.destroy(ctx.allocator());
            return 1;
        }

        struct GgxCase {
            const char* name;
            ohao::diff::WavefrontScatterMaterial material;
            uint32_t param;
            float step;  // derived above: 0.0091*r for roughness, 2^-6 for metallic
            uint32_t bounces;
        };
        // FOUR ROUGHNESS CASES SPANNING THE CONDITIONING. The last is
        // near-specular, where alpha = 1.6e-3, the lobe is at its sharpest,
        // D at its peak is ~1/(pi alpha^2) ~ 1.2e5, and the derivative is the
        // worst conditioned of the set -- which is exactly why it is here and
        // why it carries the smallest step.
        const GgxCase roughnessCases[4] = {
            {"dielectric, broad lobe (r=0.60, m=0.00, sw=1.0)",
             {0.60f, 0.00f, 1.0f},
             kParamRoughness,
             0.00390625f,  // 2^-8; h* = 0.0091*0.60 = 5.4e-3
             2u},
            {"mixture (r=0.35, m=0.50, sw=1.0)",
             {0.35f, 0.50f, 1.0f},
             kParamRoughness,
             0.00390625f,  // 2^-8; h* = 0.0091*0.35 = 3.2e-3
             3u},
            {"conductor, glossy (r=0.10, m=1.00, sw=1.0)",
             {0.10f, 1.00f, 1.0f},
             kParamRoughness,
             0.0009765625f,  // 2^-10; h* = 0.0091*0.10 = 9.1e-4
             2u},
            {"conductor, NEAR-SPECULAR (r=0.04, m=1.00, sw=1.0)",
             {0.04f, 1.00f, 1.0f},
             kParamRoughness,
             0.001953125f,  // 2^-9 -- corrected from the a-priori 2^-11; the
                             // derivation is in this check's header comment,
                             // "THE NEAR-SPECULAR CASE'S STEP IS CORRECTED..."
             2u},
        };
        const GgxCase metallicCases[2] = {
            {"broad lobe, mid metallic (r=0.60, m=0.35, sw=1.0)",
             {0.60f, 0.35f, 1.0f},
             kParamMetallic,
             0.015625f,  // 2^-6; h* = 0.0182 (L = 1, no small parameter)
             2u},
            {"glossy, high metallic (r=0.20, m=0.70, sw=1.0)",
             {0.20f, 0.70f, 1.0f},
             kParamMetallic,
             0.015625f,
             3u},
        };

        // ONE gate body for both parameters: the two differ only in which
        // field the step moves and in how that step was derived, and writing
        // the verdict twice is how two gates drift apart.
        GgxFdMeasurement roughnessM[4]{};
        GgxFdMeasurement metallicM[2]{};
        // TWO "worst" INDICES, because the two questions are different and
        // their answers need not be the same case. `worstResolution` is the
        // largest bound/|gradient| -- how little the gate could have resolved
        // -- and `worstRatio` is the largest |err|/bound, i.e. how close the
        // adjoint came to being rejected. On this run they land on different
        // cases by a hair, and reporting only one of them under the single
        // word "worst" would be a claim narrower than it sounds.
        // COLLECTS EVERY FAILING CASE, IT DOES NOT STOP AT THE FIRST. An
        // earlier version `return`ed the instant any of the four per-case
        // checks below failed, which meant a perturbed adjoint was always
        // diagnosed from `cases[0]` alone (the broad dielectric) and no
        // perturbed build ever exercised, let alone printed, a rejection at
        // the near-specular case's corrected h = 2^-9 -- the one case this
        // whole section's step correction is about. Only a dispatch failure
        // (no measurement to report at all) still aborts the loop early;
        // every other failure is printed and the loop continues, so a run
        // with more than one broken case reports all of them and the
        // corrected-h case gets its own verdict instead of being shadowed.
        auto runGate = [&](const char* checkName, const GgxCase* cases, std::size_t count,
                           GgxFdMeasurement* out, double& worstRatio, double& worstResolution,
                           std::size_t& worstRatioIndex, std::size_t& worstIndex) -> bool {
            worstRatio = 0.0;
            worstResolution = 0.0;
            worstIndex = 0;
            worstRatioIndex = 0;
            bool anyFailed = false;
            for (std::size_t i = 0; i < count; ++i) {
                const GgxCase& c = cases[i];
                if (!measureDetachedGgxGradient(ctx, wf, kW, kH, c.bounces, camera, positions,
                                                indices, kAlbedo, c.material, c.param, c.step,
                                                /*freezeSampling=*/true, kGgxSeed, ggxArena,
                                                ggxParam->gradBlock, kGgxArenaFloats, kGgxOffset,
                                                kFilmRelativeEps, out[i])) {
                    std::fprintf(stderr, "[diff_gpu_probe] FAIL: %s measurement failed on %s\n",
                                 checkName, c.name);
                    return false;
                }
                const GgxFdMeasurement& m = out[i];

                // --- NON-VACUITY 1: the instrument's own claim. Every one of
                // the five renders walked the bit-identical path.
                if (m.traceMismatches != 0) {
                    std::fprintf(
                        stderr,
                        "[diff_gpu_probe] FAIL: %s on %s -- THE FINITE DIFFERENCE IS NOT "
                        "DETACHED. %zu float(s) of the vertex trace's (origin, dir, hitT) slots "
                        "differ between a perturbed render and the centre one, across %u paths. "
                        "The sampling material was frozen at the unperturbed value on all five "
                        "renders, so every direction of every bounce should be bit-identical; "
                        "that it is not means the difference quotient below measures the "
                        "movement of the sampled directions as well as the derivative of the "
                        "estimator -- and spec 6.3 says the adjoint does NOT contain that term. "
                        "Suspect the sampling-material override (WavefrontLoop::Config's "
                        "sampling* fields, the traversal's pc.sample* tail, or "
                        "diffBsdfSampleDetached's use of qs vs q) before suspecting the "
                        "derivative\n",
                        checkName, c.name, m.traceMismatches, kCapacity);
                    anyFailed = true;
                    continue;
                }

                // --- NON-VACUITY 2: there is light and there is a gradient.
                if (!(m.jCenter > 0.0) || !std::isfinite(m.jCenter) ||
                    !std::isfinite(m.analytic) || !(std::fabs(m.analytic) > 0.0)) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: %s on %s: J = %.9g and the scattered "
                                 "gradient = %.9g. J must be finite and strictly positive (the "
                                 "scene is lit and every factor is non-negative) and the gradient "
                                 "finite and non-zero -- a zero on either side makes the "
                                 "comparison 0 against 0\n",
                                 checkName, c.name, m.jCenter, m.analytic);
                    anyFailed = true;
                    continue;
                }

                // --- NON-VACUITY 3: the gate's resolution, pre-registered.
                const double resolution = m.errorBound / std::fabs(m.analytic);
                if (resolution > worstResolution) {
                    worstResolution = resolution;
                    worstIndex = i;
                }
                if (!(resolution <= kMaxGgxResolution)) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: %s on %s REFUSES TO CLAIM A VERDICT: the "
                                 "error bound is %.6g, which is %.3g of the gradient %.9g -- above "
                                 "the pre-registered %.3g. A pass at this resolution would be "
                                 "compatible with there being nothing it could have detected. "
                                 "roundoff %.6g + truncation %.6g at h = %.9g\n",
                                 checkName, c.name, m.errorBound, resolution, m.analytic,
                                 kMaxGgxResolution, m.roundoffBound, m.truncationBound, m.hActual);
                    anyFailed = true;
                    continue;
                }

                // --- THE GATE.
                const double ratio = m.absError / m.errorBound;
                if (ratio > worstRatio) {
                    worstRatio = ratio;
                    worstRatioIndex = i;
                }
                if (!(m.absError <= m.errorBound)) {
                    std::fprintf(
                        stderr,
                        "[diff_gpu_probe] FAIL: %s on %s (%u bounce(s)) -- THE ANALYTIC GRADIENT "
                        "IS NOT THE DETACHED DERIVATIVE OF THE FILM.\n"
                        "  detached finite difference D(h)  = %.12g\n"
                        "  gradient scattered into the arena = %.12g\n"
                        "  |difference| = %.6g, which is %.6g of the gradient\n"
                        "  error bound = %.6g (roundoff %.6g + Richardson truncation %.6g)\n"
                        "  D(2h) = %.12g, h = %.12g\n"
                        "  J(-2h) = %.12g J(-h) = %.12g J(0) = %.12g J(+h) = %.12g "
                        "J(+2h) = %.12g\n"
                        "  0 trace mismatches were measured, covering bounces 0..N-2 (the trace "
                        "record is written at the TOP of each bounce, so a divergence at any "
                        "earlier bounce propagates forward into it). The LAST bounce's own drawn "
                        "direction never reaches path state before the run ends and so is NOT "
                        "directly measured here; it is covered instead by a structural argument: "
                        "the direction is a function of the frozen sampling material alone, in "
                        "every branch diffBsdfSampleDetached takes. So there is no sampling "
                        "difference to absorb this: the two numbers are the derivative of one "
                        "arithmetic function of theta computed two ways. WHERE TO LOOK: a failure "
                        "on the METALLIC cases only points at "
                        "dF0/dm, dspecScale/dm or dq/dm; on the ROUGHNESS cases only, at dD/dA, "
                        "dLambda/dA or dq/dr; on BOTH and growing with the bounce count, at the "
                        "throughput tangent the traversal carries (PathStateField::TangentR); on "
                        "BOTH at every bounce count, at the MIS-weight term or at the "
                        "d(1/pdf) half of d(bsdfWeight)\n",
                        checkName, c.name, c.bounces, m.finiteDiff, m.analytic, m.absError,
                        m.relError, m.errorBound, m.roundoffBound, m.truncationBound,
                        m.finiteDiff2h, m.hActual, m.jMinus2, m.jMinus, m.jCenter, m.jPlus,
                        m.jPlus2);
                    anyFailed = true;
                    continue;
                }
            }
            return !anyFailed;
        };

        double rWorstRatio = 0.0, rWorstRes = 0.0, mWorstRatio = 0.0, mWorstRes = 0.0;
        std::size_t rWorstIdx = 0, mWorstIdx = 0, rWorstRatioIdx = 0, mWorstRatioIdx = 0;
        if (!runGate("check 39 (d/d roughness)", roughnessCases, 4, roughnessM, rWorstRatio,
                     rWorstRes, rWorstRatioIdx, rWorstIdx)) {
            wf.destroy(ctx.allocator());
            ggxArena.destroy(ctx.allocator());
            return 1;
        }
        std::printf(
            "[diff_gpu_probe] OK: check 39 -- dJ/d(roughness) through the GGX microfacet model IS "
            "the DETACHED derivative of the film, at %zu material configurations. Seed %u, %u "
            "paths at one sample per pixel, sampling material frozen at the unperturbed value on "
            "all five renders of each case (0 trace mismatches measured over "
            "%zu perturbed renders x %u paths x 7 geometry slots, covering bounces 0..N-2; the "
            "last bounce's own drawn direction is never written to path state and so is not "
            "directly measured by this count -- it is covered instead by a structural argument, "
            "that the direction is a function of the frozen sampling material alone in every "
            "branch diffBsdfSampleDetached takes). Step per case h = 0.0091*r, "
            "the minimiser of eps*L/h + h^2/(6L^2) at L = r/2 (the scale alpha = r^2 varies on), "
            "rounded to a power of two.\n",
            static_cast<std::size_t>(4), kGgxSeed, kCapacity, static_cast<std::size_t>(16),
            kCapacity);
        for (std::size_t i = 0; i < 4; ++i) {
            std::printf("    %-52s h=%.9g bounces=%u: FD %.9g vs analytic %.9g -- |err| %.4g <= "
                        "bound %.4g (roundoff %.4g + truncation %.4g)\n",
                        roughnessCases[i].name, static_cast<double>(roughnessCases[i].step),
                        roughnessCases[i].bounces, roughnessM[i].finiteDiff, roughnessM[i].analytic,
                        roughnessM[i].absError, roughnessM[i].errorBound,
                        roughnessM[i].roundoffBound, roughnessM[i].truncationBound);
        }
        std::printf("  Closest to rejection: %s at |err|/bound %.4g. Least resolving: %s at "
                    "bound/|gradient| %.3g (pre-registered limit %.3g). The near-specular case is "
                    "the only one of the four where the Richardson truncation term exceeds the "
                    "roundoff term -- the lobe is sharp enough there that h is finally limited by "
                    "curvature rather than by cancellation, which is what 'worst conditioned' "
                    "means for this parameter\n",
                    roughnessCases[rWorstRatioIdx].name, rWorstRatio,
                    roughnessCases[rWorstIdx].name, rWorstRes, kMaxGgxResolution);

        if (!runGate("check 40 (d/d metallic)", metallicCases, 2, metallicM, mWorstRatio, mWorstRes,
                     mWorstRatioIdx, mWorstIdx)) {
            wf.destroy(ctx.allocator());
            ggxArena.destroy(ctx.allocator());
            return 1;
        }
        std::printf(
            "[diff_gpu_probe] OK: check 40 -- dJ/d(metallic) through F0, specScale and the "
            "lobe-selection probability IS the DETACHED derivative of the film, at %zu material "
            "configurations. Seed %u, %u paths, same frozen-sampling instrument (0 trace "
            "mismatches over bounces 0..N-2; see check 39's note on the last bounce's own drawn "
            "direction). Step h = 2^-6 = %.9g for both: metallic has NO small parameter -- F0, "
            "specScale and the diffuse (1-m) are all linear over the whole of [0,1] -- so L = 1 "
            "and h* = (3*eps)^(1/3) = 1.8e-2. The largest true ratio anywhere in checks 39-40 is "
            "SIXTEEN, against the glossy conductor's 2^-10; the near-specular case's own step, "
            "corrected in check 39's header, is 2^-9 -- EIGHT times smaller than this one, not "
            "thirty-two.\n",
            static_cast<std::size_t>(2), kGgxSeed, kCapacity, 0.015625);
        for (std::size_t i = 0; i < 2; ++i) {
            std::printf("    %-52s h=%.9g bounces=%u: FD %.9g vs analytic %.9g -- |err| %.4g <= "
                        "bound %.4g (roundoff %.4g + truncation %.4g)\n",
                        metallicCases[i].name, static_cast<double>(metallicCases[i].step),
                        metallicCases[i].bounces, metallicM[i].finiteDiff, metallicM[i].analytic,
                        metallicM[i].absError, metallicM[i].errorBound, metallicM[i].roundoffBound,
                        metallicM[i].truncationBound);
        }
        std::printf("  Closest to rejection: %s at |err|/bound %.4g. Least resolving: %s at "
                    "bound/|gradient| %.3g (pre-registered limit %.3g)\n",
                    metallicCases[mWorstRatioIdx].name, mWorstRatio,
                    metallicCases[mWorstIdx].name, mWorstRes, kMaxGgxResolution);

        // -----------------------------------------------------------------
        // 41. THE DETACHED-SAMPLING BIAS. A FINDING, NOT A GATE.
        // -----------------------------------------------------------------
        //
        // Spec section 6.3 makes the finite-difference harness the thing that
        // decides whether detached sampling is acceptable PER PARAMETER. This
        // is where that decision gets its number: the same five-render
        // measurement, on the same cases, at the same steps, with the sampling
        // material NOT frozen -- so the +/-h renders re-run the sampler and
        // the paths move. The difference between that quotient and the
        // detached one is the term the adjoint omits by design.
        //
        // NOTHING HERE GATES ON THE SIZE OF THE GAP. A large gap would be a
        // statement about the estimator (detached sampling is a poor
        // approximation for this parameter at this configuration), not about
        // the adjoint, and turning it into a pass/fail would be inventing a
        // threshold nobody derived.
        //
        // WHAT IS GATED, because without it the number would be meaningless:
        // the naive measurement's paths MUST actually differ from the centre
        // render's. If they did not, the sampling-material override would be a
        // no-op, the "detached" measurement above would be the naive one, and
        // checks 39-40 would be measuring something other than what they say.
        // That is the one way this section can fail, and it is a statement
        // about the INSTRUMENT rather than about the bias.
        struct BiasCase {
            const char* name;
            ohao::diff::WavefrontScatterMaterial material;
            uint32_t param;
            float step;
            uint32_t bounces;
        };
        // THREE BOUNCES for all three, whatever bounce count the gate above
        // ran them at, and the reason is the sensitivity of the one thing
        // this section gates on. The last bounce's trace record is the only
        // one that survives -- every bounce overwrites it -- so a moved
        // direction is visible in it only if some EARLIER bounce moved, which
        // for the first case (a dielectric at specularWeight 1, whose lobe
        // probability q is about 1.8%) happens for only ~9 of 512 paths per
        // opportunity. At two bounces there is ONE such opportunity per path
        // and a zero count is an ordinary outcome: 134, 16 and 0 measured
        // over three seeds. At three bounces there are two per path, and the
        // gate is on the per-case TOTAL over three seeds -- of order 55
        // expected affected paths, so an all-zero case is e^-55 rather than a
        // coin flip. Worth spelling out, because a gate that fails one run in
        // ten teaches people to re-run it instead of reading it.
        const uint32_t kBiasBounces = 3u;
        const BiasCase biasCases[3] = {
            {roughnessCases[0].name, roughnessCases[0].material, roughnessCases[0].param,
             roughnessCases[0].step, kBiasBounces},
            {roughnessCases[3].name, roughnessCases[3].material, roughnessCases[3].param,
             roughnessCases[3].step, kBiasBounces},
            {metallicCases[0].name, metallicCases[0].material, metallicCases[0].param,
             metallicCases[0].step, kBiasBounces},
        };
        // THREE SEEDS PER CASE, and the reason is that a single realisation
        // cannot tell a systematic bias from a noisy one. The naive quotient
        // is a genuine derivative -- sampleGGXVNDF is smooth in alpha, so
        // J_naive(theta) is a smooth function of theta and its central
        // difference converges -- but it is the derivative of ONE 512-path,
        // one-sample-per-pixel realisation, and the term it adds is the
        // movement of 512 sample points across an integrand they each see
        // only once. Its VARIANCE is therefore large and its sign is not
        // fixed. Reporting one number would invite reading a large |gap| as
        // "detached sampling is badly biased here" when it may be "this
        // estimator of the attached term has a large spread". Three seeds is
        // the fewest from which a spread can be quoted at all, and it is
        // quoted rather than tested: nothing below gates on the size of the
        // gap or on its spread.
        constexpr uint32_t kBiasSeeds[3] = {20260829u, 20260830u, 20260831u};
        double gapRatio[3][3]{};
        double detachedFd[3][3]{};
        double naiveFd[3][3]{};
        std::size_t naiveMismatchTotal = 0;
        std::size_t naiveMismatchByCase[3] = {0, 0, 0};
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t k = 0; k < 3; ++k) {
                GgxFdMeasurement det{};
                GgxFdMeasurement nai{};
                if (!measureDetachedGgxGradient(ctx, wf, kW, kH, biasCases[i].bounces, camera,
                                                positions, indices, kAlbedo, biasCases[i].material,
                                                biasCases[i].param, biasCases[i].step,
                                                /*freezeSampling=*/true, kBiasSeeds[k], ggxArena,
                                                ggxParam->gradBlock, kGgxArenaFloats, kGgxOffset,
                                                kFilmRelativeEps, det) ||
                    !measureDetachedGgxGradient(ctx, wf, kW, kH, biasCases[i].bounces, camera,
                                                positions, indices, kAlbedo, biasCases[i].material,
                                                biasCases[i].param, biasCases[i].step,
                                                /*freezeSampling=*/false, kBiasSeeds[k], ggxArena,
                                                ggxParam->gradBlock, kGgxArenaFloats, kGgxOffset,
                                                kFilmRelativeEps, nai)) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: check 41 measurement failed on %s at "
                                 "seed %u\n",
                                 biasCases[i].name, kBiasSeeds[k]);
                    wf.destroy(ctx.allocator());
                    ggxArena.destroy(ctx.allocator());
                    return 1;
                }
                // --- THE ONE GATE HERE, and it is about the INSTRUMENT, not
                // about the bias: freezing the sampling material must
                // actually change which path is walked, or checks 39-40's
                // "0 trace mismatches" is a fact about nothing.
                if (det.traceMismatches != 0) {
                    std::fprintf(
                        stderr,
                        "[diff_gpu_probe] FAIL: check 41 on %s at seed %u -- the DETACHED "
                        "measurement MOVED: %zu float(s) of the vertex trace's geometry slots "
                        "differ from the centre render, with the sampling material frozen on all "
                        "five renders. Same failure and same causes as checks 39-40's own trace "
                        "gate\n",
                        biasCases[i].name, kBiasSeeds[k], det.traceMismatches);
                    wf.destroy(ctx.allocator());
                    ggxArena.destroy(ctx.allocator());
                    return 1;
                }
                naiveMismatchByCase[i] += nai.traceMismatches;
                naiveMismatchTotal += nai.traceMismatches;
                detachedFd[i][k] = det.finiteDiff;
                naiveFd[i][k] = nai.finiteDiff;
                gapRatio[i][k] =
                    (nai.finiteDiff - det.finiteDiff) / std::fabs(det.analytic);
            }
        }
        // --- THE ONE GATE, and it is about the INSTRUMENT, not about the
        // bias. Freezing the sampling material must ACTUALLY change which
        // path is walked, or checks 39-40's "0 trace mismatches" is a fact
        // about nothing: their measurement would be detached by accident
        // rather than by the override, and their central non-vacuity claim
        // would be empty. Stated per CASE over the three seeds rather than
        // per measurement, for the sensitivity reason on kBiasBounces above.
        for (std::size_t i = 0; i < 3; ++i) {
            if (naiveMismatchByCase[i] == 0) {
                std::fprintf(
                    stderr,
                    "[diff_gpu_probe] FAIL: check 41 on %s -- the NAIVE measurement walked the "
                    "IDENTICAL path at all three seeds (0 differing geometry slots over %u paths "
                    "x %zu measurements). Perturbing roughness or metallic without freezing the "
                    "sampling material must move the GGX VNDF's alpha or the lobe-selection "
                    "probability and therefore the sampled direction, so identical paths mean "
                    "the sampling-material override is not what makes checks 39-40 detached, and "
                    "their non-vacuity claim is empty\n",
                    biasCases[i].name, kCapacity, static_cast<std::size_t>(3));
                wf.destroy(ctx.allocator());
                ggxArena.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf(
            "[diff_gpu_probe] OK: check 41 -- THE DETACHED-SAMPLING BIAS, MEASURED (a finding, "
            "not a gate). The same five-render difference with the sampling material NOT frozen "
            "re-runs the sampler at theta +/- h, so the paths move -- confirmed and not assumed: "
            "%zu vertex-trace geometry slots differ from the centre render across the 9 naive "
            "measurements below, against exactly 0 across all 9 detached ones. NOTHING GATES ON "
            "THE SIZE OF THE GAP: it is the derivative of the sampled directions' own "
            "contribution, which spec 6.3 says the adjoint does not contain, and it is estimated "
            "here from ONE 512-path realisation per seed -- a high-variance quantity whose spread "
            "over three seeds is quoted beside its mean precisely so a large value is not read as "
            "a large systematic bias.\n",
            naiveMismatchTotal);
        for (std::size_t i = 0; i < 3; ++i) {
            double mean = 0.0;
            for (std::size_t k = 0; k < 3; ++k) mean += gapRatio[i][k];
            mean /= 3.0;
            double var = 0.0;
            for (std::size_t k = 0; k < 3; ++k) var += (gapRatio[i][k] - mean) * (gapRatio[i][k] - mean);
            const double sd = std::sqrt(var / 2.0);  // sample sd, n-1 = 2
            std::printf("    %-52s gap/|gradient| over 3 seeds: %+.4g %+.4g %+.4g  -> mean %+.4g, "
                        "sample sd %.4g\n",
                        biasCases[i].name, gapRatio[i][0], gapRatio[i][1], gapRatio[i][2], mean,
                        sd);
            std::printf("        %-48s detached FD %+.6g %+.6g %+.6g | naive FD %+.6g %+.6g "
                        "%+.6g\n",
                        "", detachedFd[i][0], detachedFd[i][1], detachedFd[i][2], naiveFd[i][0],
                        naiveFd[i][1], naiveFd[i][2]);
        }

        wf.destroy(ctx.allocator());
        ggxArena.destroy(ctx.allocator());
    }

    // -----------------------------------------------------------------
    // 42-43. STAGE 1 TASK 4: dJ/d(EMISSION), THE PLUMBING CHECK.
    // -----------------------------------------------------------------
    //
    // THE CLOSED FORM, DERIVED (Step 1). `wf_scatter.comp`'s forward hook
    // now writes `throughput * (Lr + vec3(pc.emission))`, so
    //
    //     J(emission) = SUM_p SUM_c SUM_b T_b(p) * (Lr_b(p)[c] + emission)
    //                 = A + emission * B
    //
    //     A = SUM_p SUM_c SUM_b T_b(p) * Lr_b(p)[c]     (check 37's J at
    //                                                     emission == 0)
    //     B = SUM_p SUM_b (T_b.r(p) + T_b.g(p) + T_b.b(p))
    //
    // and NEITHER A nor B depends on emission: `Lr` is built from
    // `neeTerm`/`bsdfTerm`, which read the material and the environment and
    // never `pc.emission` (grep nee.glsl and bsdf.glsl -- neither declares
    // the parameter), and the throughput recursion `T_{b+1} = T_b *
    // bsdfWeight_b` reads the same BSDF and is equally blind to it. So
    // `dJ/d(emission) = B`, a CONSTANT (independent of emission itself) equal
    // to the arrival throughput summed over every hit vertex -- and J is
    // EXACTLY LINEAR, not merely locally so, which is the mathematical
    // content that makes this the easiest derivative in the stage and the
    // sharpest test of the PLUMBING: a central difference has zero
    // truncation error at every step size and every bounce count, so nothing
    // about the comparison below can be blamed on curvature.
    //
    // `h`, DERIVED THE WAY TASKS 2 AND 3 WERE (Step 1). Both harnesses
    // minimise `E(h)/|J'| ~ eps*L/h + h^2/(6L^2)`, giving
    // `h* = (3*eps)^(1/3) * L`, with `eps = 2e-6` UNCHANGED from Tasks 2/3
    // (still: a film value is a sum over bounces of a product of about six
    // float32 factors, each rounding at 2^-24) and `L` the scale the film
    // varies over in the parameter -- here `L = kEmission = 0.6`, the same
    // convention Task 2 used for the albedo (a representative value of an
    // unbounded-above physical quantity, not a domain like metallic's [0,1]).
    //
    //     h* = (3 * 2e-6)^(1/3) * 0.6 = (6e-6)^(1/3) * 0.6 ~= 0.01817 * 0.6
    //        ~= 1.090e-2
    //
    // nearest power of two: 2^-7 = 7.8125e-3 (log2(1.090e-2) = -6.52, closer
    // to -7 than to -6).
    //
    // THIS h IS NOT ACTUALLY OPTIMAL, AND THAT IS STATED RATHER THAN LEFT
    // IMPLICIT. Unlike albedo/roughness/metallic, the h^2/(6L^2) term of the
    // model above is not merely small at this h -- it is IDENTICALLY ZERO AT
    // EVERY h, because J is exactly linear (see the derivation above). The
    // two-term optimum this formula computes therefore does not exist for
    // this parameter: `E(h)/|J'|` is monotonically DECREASING in h (pure
    // roundoff, no offsetting curvature), so a LARGER h would give a
    // strictly smaller error bound with no downside. `h* = 2^-7` is used
    // anyway, for two reasons stated rather than hidden: (a) it keeps the
    // derivation procedure identical across all four parameters this stage
    // differentiates, which is what makes it auditable by inspection rather
    // than by re-deriving a special case each time, and (b) the resulting
    // bound, measured below, already resolves far inside the pre-registered
    // limit, so there is no practical need to push h larger. A future task
    // that wants the tightest possible emission gate should simply use a
    // bigger h; this one does not need to.
    //
    // WHAT THIS CONVENTION COSTS, QUANTIFIED (2026 review). With the
    // truncation term identically zero, the roundoff-only bound falls as
    // 1/h, so any h short of the largest one that keeps `emission - h > 0`
    // (theta = kEmission = 0.6 admits h up to just under 0.6 -- ~0.5 stays
    // comfortably clear) leaves resolution on the table. At h ~= 0.5 rather
    // than 2^-7 (~7.81e-3), the bound would be roughly (0.6/0.5) * 2^-7 /
    // 0.5, i.e. about 64x SHARPER than what this file measures --
    // bound/gradient ~= 4e-6 rather than the ~2.59e-4 this h achieves. At
    // 2^-7, this gate cannot discriminate a uniform-scaling bug below
    // roughly 1.00026x (a 0.026% miscalibration passes undetected); at
    // h ~= 0.5 that floor would drop to roughly 4e-6, a bug about 64x
    // smaller. This is a genuine, paid-for cost of keeping this task's
    // derivation procedure identical to Tasks 2/3's, not a free choice --
    // recorded here so the next reader knows what the convention bought
    // (auditability by inspection) and what it cost (resolution this
    // parameter's exact linearity would have given away for free).
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 8;
        constexpr uint32_t kEnvW = 64;
        constexpr uint32_t kEnvH = 32;
        static_assert(kEnvW != kEnvH, "a square environment hides a W<->H swap");
        // The FIXED, unperturbed scene -- "a scene whose only parameter is
        // emission" means these do not move across the three renders.
        constexpr float kMatAlbedo = 0.4f;
        constexpr float kEmission = 0.6f;
        constexpr float kStep = 0.0078125f;  // 2^-7 -- derived above
        constexpr double kFilmRelativeEps = 2e-6;
        constexpr uint32_t kGradientSeed = 20260829u;
        constexpr double kMaxGradientResolution = 1e-2;
        constexpr uint32_t kDiffParamEmission = 3u;  // DIFF_PARAM_EMISSION
        constexpr uint32_t kBounceCounts[3] = {1u, 2u, 3u};

        // ONE ScalarBlock for the emission, and a SECOND the scene does not
        // depend on -- check 43's null-test subject, the same shape check
        // 38 uses for the albedo.
        ohao::diff::ParamRegistry gradReg;
        const auto regEmission = gradReg.registerScalarBlock("emission", 1);
        const auto regUnused = gradReg.registerScalarBlock("unused_scalar_emission", 1);
        if (!regEmission.ok || !regUnused.ok) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 42 registry setup: %s %s\n",
                         regEmission.error.c_str(), regUnused.error.c_str());
            return 1;
        }
        const ohao::diff::DiffParam* emissionParam = gradReg.find("emission");
        const ohao::diff::DiffParam* unusedParam = gradReg.find("unused_scalar_emission");
        if (emissionParam == nullptr || unusedParam == nullptr) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 42 registered params not found\n");
            return 1;
        }

        ohao::diff::GradientArena gradArena;
        if (!gradArena.build(ctx.allocator(), gradReg.layout())) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 42 gradient arena build\n");
            return 1;
        }
        const ohao::diff::ArenaBlock emissionGradBlock =
            gradReg.layout().block(emissionParam->gradBlock);
        const uint32_t kGradArenaFloats =
            static_cast<uint32_t>(gradReg.layout().totalBytes() / sizeof(float));
        const uint32_t kGradEmissionOffset =
            static_cast<uint32_t>(emissionGradBlock.offsetBytes / sizeof(float));

        std::vector<float> envRgba;
        std::vector<double> envLum;
        buildParityEnvironment(kEnvW, kEnvH, envRgba, envLum);
        std::vector<float> positions;
        std::vector<uint32_t> indices;
        buildParityScene(positions, indices);
        const ohao::diff::WavefrontGenerateCamera camera = parityCamera();

        ohao::EnvCDF gradEnvCdf;
        gradEnvCdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
        if (!gradEnvCdf.valid()) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 42 EnvCDF::build produced no CDF\n");
            gradArena.destroy(ctx.allocator());
            return 1;
        }

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kW * kH, kEnvW, kEnvH) ||
            !wf.uploadEnvironment(gradEnvCdf.marginalSpan(), gradEnvCdf.conditionalSpan(),
                                  gradEnvCdf.integral())) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 42 buffers build / env CDF upload\n");
            wf.destroy(ctx.allocator());
            gradArena.destroy(ctx.allocator());
            return 1;
        }

        // Pure Lambert, like check 37's material -- irrelevant to the
        // derivative under test (emission reaches neither the BSDF nor the
        // sampler at all) but a fixed, lit, non-degenerate scene the direct
        // term A above is genuinely nonzero on, so the film is a sum of two
        // nonzero pieces rather than emission alone.
        const ohao::diff::WavefrontScatterMaterial kEmissionMaterial{1.0f, 0.0f, 0.0f};

        CrnFdMeasurement measurements[3]{};
        double worstRatio = 0.0;
        double worstResolution = 0.0;
        std::size_t totalTraceMismatches = 0;

        for (std::size_t i = 0; i < 3; ++i) {
            const uint32_t bounces = kBounceCounts[i];
            CrnFdMeasurement& m = measurements[i];
            if (!measureCrnEmissionGradient(ctx, wf, kW, kH, bounces, camera, positions, indices,
                                            kMatAlbedo, kEmissionMaterial, kEmission, kStep,
                                            kGradientSeed, gradArena, emissionParam->gradBlock,
                                            kGradArenaFloats, kGradEmissionOffset,
                                            kFilmRelativeEps, m)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 42 measurement failed at %u bounce(s)\n",
                             bounces);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }
            totalTraceMismatches += m.traceMismatches;

            // --- THE CRN-VALIDITY MEASUREMENT (Step 1's self-check). Plain
            // CRN is valid for emission ONLY IF perturbing it moves no
            // sampled direction; this is what confirms that rather than
            // inheriting it from Task 2's albedo case by analogy.
            if (m.traceMismatches != 0u) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 42 at %u bounce(s) -- %zu vertex-trace "
                             "geometry slots differ between an emission +/-h render and the "
                             "centre one. Plain common-random-number comparison is NOT valid if "
                             "this is nonzero: something now reads pc.emission from inside "
                             "diffBsdfSample/diffBsdfSampleDetached or sampleEnvMap, which would "
                             "move a sampled direction and require Task 3's detached instrument "
                             "instead\n",
                             bounces, m.traceMismatches);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }

            // --- NON-VACUITY 1: there is light, and there is a gradient.
            if (!(m.jCenter > 0.0) || !std::isfinite(m.jCenter) || !(m.analytic > 0.0) ||
                !std::isfinite(m.analytic)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 42 at %u bounce(s): J = %.9g and the "
                             "scattered gradient = %.9g. Both must be finite and strictly "
                             "positive -- every hit vertex's arrival throughput is non-negative "
                             "and the scene is lit, so a zero on either side means nothing was "
                             "accumulated and the comparison below would be 0 against 0\n",
                             bounces, m.jCenter, m.analytic);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }

            // --- NON-VACUITY 2: the gate's resolution, pre-registered.
            const double resolution = m.errorBound / m.analytic;
            if (resolution > worstResolution) worstResolution = resolution;
            if (!(resolution <= kMaxGradientResolution)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 42 at %u bounce(s) REFUSES TO CLAIM A "
                             "VERDICT: the derived error bound is %.6g, which is %.3g of the "
                             "gradient %.9g -- above the pre-registered %.3g. A pass at this "
                             "resolution would be compatible with there being nothing it could "
                             "have detected. roundoff %.6g (no truncation term -- J is exactly "
                             "linear) at h = %.9g\n",
                             bounces, m.errorBound, resolution, m.analytic,
                             kMaxGradientResolution, m.roundoffBound, m.hActual);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }

            // --- THE GATE. THIS IS THE MAGNITUDE ASSERTION (Step 5's
            // explicit requirement), not merely a direction check, and is
            // stated as such rather than left implicit: it bounds
            // |finiteDiff - analytic| in ABSOLUTE terms against a
            // roundoff-only error bound derived independently of the two
            // numbers being compared, and NON-VACUITY 2 above has already
            // pre-registered that bound to resolve to <= 1e-2 of the
            // gradient's own scale (`resolution = errorBound / analytic`).
            // So passing this gate implies
            // `|analytic/finiteDiff - 1| <= kMaxGradientResolution *
            // (1 + O(kMaxGradientResolution))` in the worst CASE THE GATE
            // ITSELF ADMITS, and at the resolution actually measured here
            // (~2.6e-4, far inside that 1e-2 ceiling) it implies a ratio-to-1
            // deviation about 38x tighter than a literal ratio-to-1 test
            // pinned to the shared `kMaxGradientResolution` limit could ever
            // discriminate. A same-sign-but-wrong-scale bug -- exactly what
            // a direction-only (same-sign) check would miss -- still fails
            // HERE, hard: see this check's own Step 5(a) transcript, where a
            // synthetic uniform-scale bug at 38.6x this bound tripped this
            // exact message.
            //
            // A SEPARATE ratio-to-1 assertion against `kMaxGradientResolution`
            // used to sit after this gate, under a "MAGNITUDE, NOT ONLY
            // DIRECTION" banner, for exactly the claim this comment now
            // makes explicitly. It was deleted (2026 review, Task 4 Finding
            // 1) because no perturbation exists that clears THIS gate while
            // failing that one at the shared 1e-2 limit: both compare the
            // same |finiteDiff - analytic| deviation, on two scales
            // (`errorBound` here, `|finiteDiff|` there) that agree to within
            // roundoff, so the loosely-thresholded one could never fire
            // first -- a check that cannot fire is worse than no check. A
            // magnitude check that used a materially TIGHTER threshold
            // instead of the shared 1e-2 would not be independent either: at
            // any threshold at or below this gate's own ~2.6e-4 resolution
            // it degenerates into a restatement of THIS inequality with a
            // hardcoded constant in place of the roundoff formula that
            // derives `errorBound` -- strictly worse, since it loses the
            // per-h, per-parameter derivation this file's checks otherwise
            // share.
            const double ratio = m.absError / m.errorBound;
            if (ratio > worstRatio) worstRatio = ratio;
            if (!(m.absError <= m.errorBound)) {
                std::fprintf(
                    stderr,
                    "[diff_gpu_probe] FAIL: check 42 at %u bounce(s) -- THE ANALYTIC GRADIENT IS "
                    "NOT THE DERIVATIVE OF THE FILM.\n"
                    "  finite difference (J(e+h) - J(e-h)) / 2h = %.12g\n"
                    "  gradient scattered into the arena         = %.12g\n"
                    "  |difference| = %.6g, which is %.6g of the gradient\n"
                    "  derived error bound = %.6g (roundoff only, no truncation term)\n"
                    "  J(e-h) = %.12g, J(e) = %.12g, J(e+h) = %.12g, h = %.12g\n"
                    "  Both sides describe ONE realisation of the estimator at seed %u under "
                    "common random numbers (measured 0 trace mismatches), so there is no "
                    "sampling error and no truncation error to absorb this: the two numbers are "
                    "the derivative of the SAME LINEAR function computed two ways, and they "
                    "disagree. diffVertexEmissionScatter returns v.adjoint UNMODIFIED -- if this "
                    "fails, suspect the SCATTER SITE (wrong offset, wrong sign, missing branch) "
                    "before suspecting any calculus, because there is none here to get wrong\n",
                    bounces, m.finiteDiff, m.analytic, m.absError, m.relError, m.errorBound,
                    m.jMinus, m.jCenter, m.jPlus, m.hActual, kGradientSeed);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }
        }

        // --- NON-VACUITY 3: every hit vertex actually contributes, at every
        // bounce count -- B = SUM_b T_b strictly increases with the number
        // of bounces in a scene where every path stays alive (the fused-loop
        // survival induction, same as check 37 relies on), so the gradient
        // itself must strictly increase from one bounce count to three. If
        // it did not, emission would only be reaching the film at bounce 0
        // (a plausible bug: applying it once at the camera ray rather than
        // at every hit vertex) and the 2- and 3-bounce measurements would be
        // exercising nothing the one-bounce case does not already cover.
        if (!(measurements[2].analytic > measurements[0].analytic + 1.0)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 42 -- dJ/d(emission) is %.6f at one bounce "
                         "and %.6f at three. It must strictly increase with the bounce count in "
                         "this closed-box scene (every path is alive at every bounce), because it "
                         "equals the arrival throughput summed over every hit vertex -- more "
                         "bounces means more vertices. Equal values would mean emission is only "
                         "reaching the film at bounce 0, not at every hit vertex\n",
                         measurements[0].analytic, measurements[2].analytic);
            wf.destroy(ctx.allocator());
            gradArena.destroy(ctx.allocator());
            return 1;
        }

        std::printf(
            "[diff_gpu_probe] OK: check 42 -- the gradient shaders/diff/wf_scatter_replay.comp "
            "scatters into the arena for DIFF_PARAM_EMISSION IS the derivative of the film "
            "shaders/diff/wf_scatter.comp accumulates. J(emission) is EXACTLY LINEAR (dT_b/d"
            "(emission) = 0 and dLr_b/d(emission) = 0 for every b -- neither the throughput "
            "recursion nor the MIS-combined direct term ever reads pc.emission), so the only "
            "error term is roundoff. Common random numbers, seed %u, %u paths at one sample per "
            "pixel, h = 2^-7 = %.9g (derived: h* = (3*eps)^(1/3)*L at eps = %.0e, L = %.2g is "
            "~1.090e-2; this is the nearest power of two -- NOT the true optimum, since the "
            "truncation half of the model is identically zero here and a larger h would resolve "
            "even better; kept for derivation-procedure consistency with Tasks 2/3). PLAIN CRN "
            "MEASURED VALID: %zu vertex-trace geometry mismatches across all 6 perturbed renders "
            "(3 bounce counts x 2 perturbations), against a nonzero count for roughness/metallic "
            "at check 41 -- this parameter genuinely does not need Task 3's detached instrument.\n"
            "    1 bounce : FD %.9g vs analytic %.9g -- |err| %.4g <= bound %.4g (roundoff only)\n"
            "    2 bounces: FD %.9g vs analytic %.9g -- |err| %.4g <= bound %.4g (roundoff only)\n"
            "    3 bounces: FD %.9g vs analytic %.9g -- |err| %.4g <= bound %.4g (roundoff only)\n"
            "  Worst |err|/bound %.4g; worst bound/gradient %.3g (pre-registered limit %.3g) -- "
            "THE GATE above IS the magnitude assertion (see its comment); no separate "
            "ratio-to-1 check follows it. dJ/d(emission) rises from %.4f at one bounce to %.4f "
            "at three, so every hit vertex is carrying real weight, not just bounce 0\n",
            kGradientSeed, kW * kH, static_cast<double>(kStep), kFilmRelativeEps,
            static_cast<double>(kEmission), totalTraceMismatches, measurements[0].finiteDiff,
            measurements[0].analytic, measurements[0].absError, measurements[0].errorBound,
            measurements[1].finiteDiff, measurements[1].analytic, measurements[1].absError,
            measurements[1].errorBound, measurements[2].finiteDiff, measurements[2].analytic,
            measurements[2].absError, measurements[2].errorBound, worstRatio, worstResolution,
            kMaxGradientResolution, measurements[0].analytic, measurements[2].analytic);

        // -----------------------------------------------------------------
        // 43. THE NULL TEST. Exactly zero, not "small" -- check 38's shape,
        // applied to the emission block. See check 38 for the full
        // rationale (a scatter into the wrong element is invisible to
        // check 42, which reads only the addressed float); this restates it
        // for a SECOND registered parameter to confirm the plumbing
        // generalises rather than happening to work for one.
        // -----------------------------------------------------------------
        struct NullBlock {
            const char* name;
            std::size_t index;
            std::size_t expectedFloats;
        };
        const NullBlock nullBlocks[3] = {
            {"emission's Adam m/v state", emissionParam->stateBlock,
             emissionParam->floatCount * 2u},
            {"unused_scalar_emission's gradient", unusedParam->gradBlock, unusedParam->floatCount},
            {"unused_scalar_emission's Adam m/v state", unusedParam->stateBlock,
             unusedParam->floatCount * 2u},
        };

        const std::vector<float> wholeArena = gradArena.readbackAll(ctx.allocator());
        if (wholeArena.size() != static_cast<std::size_t>(kGradArenaFloats)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 43 -- the whole-arena readback returned "
                         "%zu floats, expected %u. A null test over the wrong number of floats "
                         "is not a null test\n",
                         wholeArena.size(), kGradArenaFloats);
            wf.destroy(ctx.allocator());
            gradArena.destroy(ctx.allocator());
            return 1;
        }

        auto describeArenaFloat = [&](std::size_t f) -> std::string {
            for (const NullBlock& nb : nullBlocks) {
                const ohao::diff::ArenaBlock b = gradReg.layout().block(nb.index);
                const std::size_t first = b.offsetBytes / sizeof(float);
                const std::size_t count = b.sizeBytes / sizeof(float);
                if (f >= first && f < first + count) {
                    return std::string(nb.name) + ", element " + std::to_string(f - first) +
                           " of arena block " + std::to_string(nb.index);
                }
            }
            return "256-byte alignment padding owned by no block";
        };

        std::size_t nullFloatsChecked = 0;
        for (std::size_t f = 0; f < wholeArena.size(); ++f) {
            if (f == static_cast<std::size_t>(kGradEmissionOffset)) continue;  // the one written
            ++nullFloatsChecked;
            if (wholeArena[f] != 0.0f) {
                std::fprintf(
                    stderr,
                    "[diff_gpu_probe] FAIL: check 43 -- arena float %zu (%s) is %.9g and must be "
                    "EXACTLY 0. The traversal's only arena write for DIFF_PARAM_EMISSION is one "
                    "atomicAdd at ScatterPush::gradAlbedoOffset + k with k = 0, i.e. arena float "
                    "%u, which is element 0 of block %zu. A non-zero anywhere else is a scatter "
                    "that landed outside the element it was told to write -- which check 42 is "
                    "blind to, since it reads only that one float\n",
                    f, describeArenaFloat(f).c_str(), static_cast<double>(wholeArena[f]),
                    kGradEmissionOffset, emissionParam->gradBlock);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }
        }

        std::size_t namedNullFloats = 0;
        for (const NullBlock& nb : nullBlocks) {
            const ohao::diff::ArenaBlock b = gradReg.layout().block(nb.index);
            const std::size_t first = b.offsetBytes / sizeof(float);
            const std::size_t count = b.sizeBytes / sizeof(float);
            if (count != nb.expectedFloats || first + count > wholeArena.size()) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 43 -- block %zu (%s) spans floats "
                             "[%zu, %zu) of a %zu-float arena and holds %zu floats, expected "
                             "%zu. The whole-arena scan cannot claim to cover a block it does "
                             "not contain\n",
                             nb.index, nb.name, first, first + count, wholeArena.size(), count,
                             nb.expectedFloats);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }
            namedNullFloats += count;
        }

        std::printf(
            "[diff_gpu_probe] OK: check 43 -- the null test for DIFF_PARAM_EMISSION: after a run "
            "that accumulated %.9g into arena float %u (element 0 of the emission's gradient "
            "block, block %zu), ALL %zu other floats of the %u-float arena are EXACTLY 0.0f, "
            "compared as floats and not through a tolerance -- emission's Adam m/v state and both "
            "blocks of a second registered parameter the scene does not depend on (%zu floats), "
            "plus %zu floats of 256-byte alignment padding\n",
            measurements[2].analytic, kGradEmissionOffset, emissionParam->gradBlock,
            nullFloatsChecked, kGradArenaFloats, namedNullFloats,
            nullFloatsChecked - namedNullFloats);

        wf.destroy(ctx.allocator());
        gradArena.destroy(ctx.allocator());
    }

    // -----------------------------------------------------------------
    // 44-45. STAGE 1 TASK 5: THE TEXTURE SCATTER.
    // -----------------------------------------------------------------
    //
    // THE FIRST PARAMETER IN THIS SUBSYSTEM THAT IS NOT A SCALAR. A texture
    // read is `E(uv) = SUM_i w_i(uv) * texel_i` over four texels and its
    // adjoint scatters `dL * w_i` into each; the weights are the ones the
    // FORWARD read used, and they must be, because a different bilinear
    // reconstruction is a different function whose derivative answers a
    // different question. shaders/includes/diff/bsdf_adjoint.glsl's "STAGE 1
    // TASK 5" banner argues that at length -- it is the ONE place in this
    // stage where forward/adjoint sharing is required rather than forbidden.
    //
    // WHICH PARAMETER THE TEXTURE DRIVES, AND THEREFORE WHICH INSTRUMENT.
    // It drives the EMISSION -- the same additive, never-sampled-from
    // self-emitted radiance Task 4's scalar drove, read out of a texture
    // instead of out of a push constant. So the instrument is PLAIN COMMON
    // RANDOM NUMBERS, not Task 3's detached finite difference, and the reason
    // is STRUCTURAL rather than empirical: the texture is read by
    // `diffEmissionAt` and by nothing else in the traversal's translation
    // unit, while `diffBsdfSample`/`diffBsdfSampleDetached` and `sampleEnvMap`
    // take no emission argument of any kind and never touch binding 11 -- so
    // no perturbation of a texel can move a draw or a direction at any bounce.
    // Check 45 MEASURES that too (`traceGeometryMismatches` between each
    // perturbed render and the centre, required to be exactly 0), the way
    // Task 4 did, but the measurement is corroboration: the trace record is
    // overwritten each bounce and so covers bounces 0..N-2, and it is the
    // structural argument that closes the remaining one.
    //
    // A TEXTURE DRIVING THE BASE COLOUR WOULD ALSO HAVE ADMITTED PLAIN CRN
    // (the albedo does not move the sampled direction at metallic 0 either).
    // Emission was chosen instead so that the ADJOINT under test is Task 4's
    // one-line `return v.adjoint` and the whole of what these two checks can
    // fail on is the SCATTER: which element, with which weight. With a
    // base-colour texture a failure would have been ambiguous between the
    // bilinear machinery and the albedo derivative Task 2 already gates.
    //
    // THE TWO CHECKS, AND WHY BOTH:
    //
    //   * CHECK 44 is Step 1's pair, in a CONSTANT-UV configuration
    //     (`uvScale == 0`, so every vertex reads one footprint) that makes
    //     the touched element set knowable in closed form on the host: the
    //     four weights' scattered totals sum to the incoming adjoint, and
    //     every arena float outside the predicted footprint is EXACTLY 0.
    //     The first half is independent of what the individual weights are;
    //     the second is what pins the ELEMENT ORDERING absolutely, because
    //     the prediction comes from `ParamShape::elementIndex` and not from
    //     the shader.
    //   * CHECK 45 is the MAGNITUDE, per element, in a VARYING-uv
    //     configuration -- the texture actually used as a texture. It is what
    //     the conservation identity cannot see: a scatter that put every
    //     vertex's whole adjoint into ONE of the four texels conserves the
    //     total exactly. (It would fail check 44's footprint half only if
    //     that one texel were outside the footprint, so this is not
    //     redundant.)
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 8;
        constexpr uint32_t kEnvW = 64;
        constexpr uint32_t kEnvH = 32;
        static_assert(kEnvW != kEnvH, "a square environment hides a W<->H swap");
        constexpr float kMatAlbedo = 0.4f;
        constexpr uint32_t kGradientSeed = 20260901u;
        constexpr uint32_t kBounces = 3u;
        constexpr double kFilmRelativeEps = 2e-6;
        // 2^-24: the relative spacing of float32 near 1. Used to derive the
        // conservation bound below from the number of atomic accumulations,
        // not chosen.
        constexpr double kFloat32Eps = 5.9604644775390625e-08;

        // THE SHAPE IS 4 x 3 x 3, and none of the three numbers is free.
        //   * WIDTH != HEIGHT, because a square texture makes a row/column
        //     transposition invisible: (y*w + x) and (x*h + y) agree.
        //   * CHANNELS == 3, because a single-channel texture makes the
        //     ordering's `* channels + c` factor vacuous -- interleaved and
        //     planar layouts coincide when there is one channel -- and
        //     because the per-channel scatter (channel c to channel c's
        //     element, no summing) is a shape the scalar parameters could not
        //     exercise at all.
        //   * SMALL, because check 44 asserts EXACT zeros over the whole
        //     arena and names every nonzero it expects.
        const ohao::diff::ParamShape kTexShape{4u, 3u, 3u};
        const uint32_t kTexFloats = kTexShape.floatCount();

        // The primal. NON-UNIFORM in x, y AND c, so that the FORWARD read is
        // exercised as a real bilinear interpolation rather than as a
        // constant: a wrong texel index in the read would still return the
        // same value from a flat texture. The values do not affect any
        // gradient (dJ/d(texel_k) is `SUM_b T_b * w_k`, which contains no
        // texel value at all) -- they affect J, which is what check 45's
        // finite difference measures.
        std::vector<float> baseTexels(kTexFloats, 0.0f);
        for (uint32_t y = 0; y < kTexShape.height; ++y) {
            for (uint32_t x = 0; x < kTexShape.width; ++x) {
                for (uint32_t c = 0; c < kTexShape.channels; ++c) {
                    baseTexels[kTexShape.elementIndex(x, y, c)] =
                        0.30f + 0.05f * static_cast<float>(y * kTexShape.width + x) +
                        0.02f * static_cast<float>(c);
                }
            }
        }

        // THREE registered parameters: the texture, a SCALAR emission (whose
        // gradient is check 44's independent reference for the conservation
        // identity), and one the scene does not depend on at all (whose two
        // blocks are part of what must be exactly zero).
        ohao::diff::ParamRegistry gradReg;
        const auto regTex =
            gradReg.registerTexture("emission_tex", kTexShape, VK_FORMAT_R32G32B32_SFLOAT);
        const auto regScalar = gradReg.registerScalarBlock("emission_scalar", 1);
        const auto regUnused = gradReg.registerScalarBlock("unused_scalar_tex", 1);
        if (!regTex.ok || !regScalar.ok || !regUnused.ok) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 44 registry setup: %s %s %s\n",
                         regTex.error.c_str(), regScalar.error.c_str(), regUnused.error.c_str());
            return 1;
        }
        const ohao::diff::DiffParam* texParam = gradReg.find("emission_tex");
        const ohao::diff::DiffParam* scalarParam = gradReg.find("emission_scalar");
        const ohao::diff::DiffParam* unusedParam = gradReg.find("unused_scalar_tex");
        if (texParam == nullptr || scalarParam == nullptr || unusedParam == nullptr) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 44 registered params not found\n");
            return 1;
        }
        if (texParam->floatCount != kTexFloats) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 44 -- the registry gave the texture "
                         "parameter %u floats, its shape says %u\n",
                         texParam->floatCount, kTexFloats);
            return 1;
        }

        ohao::diff::GradientArena gradArena;
        if (!gradArena.build(ctx.allocator(), gradReg.layout())) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 44 gradient arena build\n");
            return 1;
        }
        const uint32_t kGradArenaFloats =
            static_cast<uint32_t>(gradReg.layout().totalBytes() / sizeof(float));
        const uint32_t kGradTexOffset = static_cast<uint32_t>(
            gradReg.layout().block(texParam->gradBlock).offsetBytes / sizeof(float));
        const uint32_t kGradScalarOffset = static_cast<uint32_t>(
            gradReg.layout().block(scalarParam->gradBlock).offsetBytes / sizeof(float));

        std::vector<float> envRgba;
        std::vector<double> envLum;
        buildParityEnvironment(kEnvW, kEnvH, envRgba, envLum);
        std::vector<float> positions;
        std::vector<uint32_t> indices;
        buildParityScene(positions, indices);
        const ohao::diff::WavefrontGenerateCamera camera = parityCamera();

        ohao::EnvCDF gradEnvCdf;
        gradEnvCdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
        if (!gradEnvCdf.valid()) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 44 EnvCDF::build produced no CDF\n");
            gradArena.destroy(ctx.allocator());
            return 1;
        }

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kW * kH, kEnvW, kEnvH) ||
            !wf.uploadEnvironment(gradEnvCdf.marginalSpan(), gradEnvCdf.conditionalSpan(),
                                  gradEnvCdf.integral())) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 44 buffers build / env CDF "
                                  "upload\n");
            wf.destroy(ctx.allocator());
            gradArena.destroy(ctx.allocator());
            return 1;
        }

        // Pure Lambert, check 42's material exactly: irrelevant to this
        // derivative (an emission texture reaches neither the BSDF nor the
        // sampler) but a fixed, lit, non-degenerate scene.
        const ohao::diff::WavefrontScatterMaterial kTexMaterial{1.0f, 0.0f, 0.0f};

        // === CHECK 44: the constant-uv configuration. =====================
        //
        // `uvScale == 0` on both axes pins every vertex to ONE texture
        // coordinate. That is a deliberate instrument, not a degenerate
        // scene: it is what makes the set of arena floats the scatter may
        // touch a closed-form prediction rather than a re-derivation of what
        // the shader did.
        //
        // The uv is chosen so the footprint sits WELL INSIDE the texture and
        // all four weights are far from 0 and from each other:
        // u = 0.525 puts the continuous x coordinate at 0.525*4 - 0.5 = 1.6
        // (texels 1 and 2, tx = 0.6) and v = 19/30 puts the y coordinate at
        // 1.4 (texels 1 and 2, ty = 0.4), giving weights 0.24, 0.36, 0.16,
        // 0.24. A footprint against a border would collapse corners under
        // clamping and a weight near 0 would make "this element is nonzero"
        // a claim about rounding.
        constexpr float kConstUvU = 0.525f;
        constexpr float kConstUvV = 0.6333333f;
        const HostBilinearFootprint fp =
            hostBilinearFootprint(kConstUvU, kConstUvV, kTexShape.width, kTexShape.height);

        // THE PREDICTION, from ParamShape::elementIndex ALONE. These are the
        // only arena floats the scatter is permitted to touch.
        std::vector<uint32_t> footprintElements;
        const uint32_t fpX[4] = {fp.x0, fp.x1, fp.x0, fp.x1};
        const uint32_t fpY[4] = {fp.y0, fp.y0, fp.y1, fp.y1};
        for (int i = 0; i < 4; ++i) {
            for (uint32_t c = 0; c < kTexShape.channels; ++c) {
                footprintElements.push_back(kTexShape.elementIndex(fpX[i], fpY[i], c));
            }
        }
        std::sort(footprintElements.begin(), footprintElements.end());
        footprintElements.erase(std::unique(footprintElements.begin(), footprintElements.end()),
                                footprintElements.end());
        if (footprintElements.size() != 12u) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 44 -- the predicted footprint covers %zu "
                         "distinct elements, expected 12 (four distinct texels x three channels). "
                         "The chosen uv must sit strictly inside a cell and away from the border, "
                         "or corners collapse and the null test below stops discriminating\n",
                         footprintElements.size());
            wf.destroy(ctx.allocator());
            gradArena.destroy(ctx.allocator());
            return 1;
        }

        // --- Run A: the TEXTURE. -----------------------------------------
        std::vector<float> filmA;
        {
            const ohao::diff::WavefrontGradientOptions options = emissionTextureOptions(
                baseTexels, kTexShape, /*uvScaleU=*/0.0f, /*uvScaleV=*/0.0f, kConstUvU, kConstUvV);
            if (!ctx.runWavefrontGradientProbe(wf, kW, kH, kBounces, camera,
                                               std::span<const float>(positions),
                                               std::span<const uint32_t>(indices), kMatAlbedo,
                                               kTexMaterial, kGradientSeed, gradArena,
                                               kGradArenaFloats, kGradTexOffset, filmA, options)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 44 texture run dispatch\n");
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }
        }
        const std::vector<float> arenaAfterTexture = gradArena.readbackAll(ctx.allocator());
        if (arenaAfterTexture.size() != static_cast<std::size_t>(kGradArenaFloats)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 44 -- the whole-arena readback returned "
                         "%zu floats, expected %u. A null test over the wrong number of floats is "
                         "not a null test\n",
                         arenaAfterTexture.size(), kGradArenaFloats);
            wf.destroy(ctx.allocator());
            gradArena.destroy(ctx.allocator());
            return 1;
        }

        // --- Run B: the SCALAR emission, same scene, same seed, same bounce
        // count. Its gradient is `SUM over hit vertices of (dL.x+dL.y+dL.z)`
        // (check 42's derivation, gated there against a finite difference),
        // and its scatter -- ONE atomicAdd of a channel sum, with no weight
        // and no texel index anywhere in it -- shares no code with the
        // bilinear scatter under test. That is what makes it an INDEPENDENT
        // reference for the conservation identity rather than a restatement
        // of it. The adjoint `dL` is the arrival throughput, which reads no
        // emission of either kind, so it is the same sequence in both runs.
        double referenceAdjointTotal = 0.0;
        {
            ohao::diff::WavefrontGradientOptions options;
            options.diffParam = 3u;  // DIFF_PARAM_EMISSION
            options.emission = 0.6f;
            std::vector<float> filmB;
            if (!ctx.runWavefrontGradientProbe(wf, kW, kH, kBounces, camera,
                                               std::span<const float>(positions),
                                               std::span<const uint32_t>(indices), kMatAlbedo,
                                               kTexMaterial, kGradientSeed, gradArena,
                                               kGradArenaFloats, kGradScalarOffset, filmB,
                                               options)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 44 scalar reference dispatch\n");
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }
            const std::vector<float> arenaAfterScalar = gradArena.readbackAll(ctx.allocator());
            if (arenaAfterScalar.size() != static_cast<std::size_t>(kGradArenaFloats)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 44 scalar-run arena readback "
                                      "size\n");
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }
            referenceAdjointTotal = static_cast<double>(arenaAfterScalar[kGradScalarOffset]);
            // The texture branch must not have fired at all in a
            // DIFF_PARAM_EMISSION run -- if it had, this reference would be
            // measuring the thing it is meant to be independent of.
            for (uint32_t k = 0; k < kTexFloats; ++k) {
                const std::size_t f = static_cast<std::size_t>(kGradTexOffset) + k;
                if (arenaAfterScalar[f] != 0.0f) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: check 44 -- a DIFF_PARAM_EMISSION run "
                                 "wrote %.9g into the TEXTURE parameter's element %u. The scalar "
                                 "run is this check's independent reference; a texture scatter "
                                 "firing inside it would make it a restatement of the thing under "
                                 "test\n",
                                 static_cast<double>(arenaAfterScalar[f]), k);
                    wf.destroy(ctx.allocator());
                    gradArena.destroy(ctx.allocator());
                    return 1;
                }
            }
        }

        // --- NON-VACUITY: the reference is real. -------------------------
        if (!(referenceAdjointTotal > 0.0) || !std::isfinite(referenceAdjointTotal)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 44 -- the reference adjoint total is %.9g "
                         "and must be finite and strictly positive: every hit vertex's arrival "
                         "throughput is non-negative and this scene is lit, so a zero means "
                         "nothing was accumulated and the identity below would be 0 == 0\n",
                         referenceAdjointTotal);
            wf.destroy(ctx.allocator());
            gradArena.destroy(ctx.allocator());
            return 1;
        }

        // --- THE FOOTPRINT / NULL HALF. Every arena float outside the 12
        // predicted elements must be EXACTLY 0.0f -- compared as floats, not
        // through a tolerance -- and the 12 must each be strictly positive.
        //
        // This is what pins the ELEMENT ORDERING at runtime, and it is
        // ABSOLUTE rather than self-consistent: the 12 indices come from
        // ParamShape::elementIndex and the host's own bilinear footprint, not
        // from anything the shader computed. A shader that transposed x and y
        // would put its mass at (x*height + y)*channels + c -- elements 12-14
        // and 15-17 and 21-23 and 24-26 for this footprint -- and be rejected
        // both by a nonzero outside the prediction and by a zero inside it.
        // A channel-PLANAR shader ordering (c*w*h + y*w + x) would be
        // rejected the same way.
        //
        // WHAT IT STILL CANNOT SEE, stated rather than implied: a permutation
        // WITHIN one texel's three channels. This scene's throughput is grey
        // (grey albedo, grey environment), so dL is grey and the three
        // channel elements of a texel carry equal values -- swapping two of
        // them is unobservable HERE. That residue is covered by
        // `checkTexelOrderingTie()`, which pins the `+ c` term in the source.
        //
        // A SECOND THING THIS CHECK ALONE CANNOT SEE, for a different reason
        // than the one above: an argument-order swap at
        // `diffScatterEmissionTexture`'s call sites (`xs[i]`/`ys[i]` passed as
        // `y`/`x` to `diffTexelElementIndex`) rather than in the ordering
        // FORMULA itself. This constant-uv configuration's footprint is
        // SYMMETRIC -- texels (1,1) (2,1) (1,2) (2,2) -- so swapping x and y
        // at the call site maps to the SAME twelve elements, only exchanging
        // w10 and w01 between two of them; the set this check compares
        // against is unchanged and conservation still holds, so this check
        // would pass either way. What this check DOES catch is the ordering
        // FORMULA transposed (`(x*height+y)*channels+c`, landing at
        // {12-14,15-17,21-23,24-26} above), which is a different bug from an
        // argument-order swap at the call site even though both involve x and
        // y. Check 45's per-element finite difference, run at three distinct
        // texels under a VARYING uv where the footprint is not symmetric, is
        // what actually catches the call-site swap.
        std::size_t nullFloatsChecked = 0;
        for (std::size_t f = 0; f < arenaAfterTexture.size(); ++f) {
            const bool predicted =
                f >= kGradTexOffset && f < static_cast<std::size_t>(kGradTexOffset) + kTexFloats &&
                std::binary_search(footprintElements.begin(), footprintElements.end(),
                                   static_cast<uint32_t>(f - kGradTexOffset));
            if (predicted) continue;
            ++nullFloatsChecked;
            if (arenaAfterTexture[f] != 0.0f) {
                std::fprintf(
                    stderr,
                    "[diff_gpu_probe] FAIL: check 44 -- arena float %zu is %.9g and must be "
                    "EXACTLY 0. With uvScale 0 every vertex reads texture coordinate (%.7g, "
                    "%.7g), whose bilinear footprint is texels (%u,%u) (%u,%u) (%u,%u) (%u,%u); "
                    "by ParamShape::elementIndex those are elements {%u..%u} of the texture's "
                    "gradient block, which starts at arena float %u. A nonzero anywhere else is a "
                    "scatter that landed outside these twelve predicted elements -- the "
                    "conservation identity below, which sums only these twelve, is blind to a "
                    "SWAP AMONG them (the total is unaffected), but NOT to a scatter that leaves "
                    "them altogether: that drops a predicted element's contribution and the "
                    "totals stop matching too\n",
                    f, static_cast<double>(arenaAfterTexture[f]), static_cast<double>(kConstUvU),
                    static_cast<double>(kConstUvV), fp.x0, fp.y0, fp.x1, fp.y0, fp.x0, fp.y1,
                    fp.x1, fp.y1, footprintElements.front(), footprintElements.back(),
                    kGradTexOffset);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }
        }
        double scatteredTotal = 0.0;
        double perChannel[3] = {0.0, 0.0, 0.0};
        for (uint32_t k : footprintElements) {
            const float value = arenaAfterTexture[static_cast<std::size_t>(kGradTexOffset) + k];
            if (!(value > 0.0f) || !std::isfinite(value)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 44 -- element %u of the texture's "
                             "gradient block is %.9g. All four footprint texels carry weights "
                             "between 0.16 and 0.36 and every hit vertex's adjoint is strictly "
                             "positive, so every one of the 12 predicted elements must be "
                             "strictly positive. A zero here means the scatter reached fewer "
                             "texels than the read did\n",
                             k, static_cast<double>(value));
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }
            scatteredTotal += static_cast<double>(value);
            perChannel[k % kTexShape.channels] += static_cast<double>(value);
        }

        // --- THE CONSERVATION HALF. `SUM_i w_i == 1` for a bilinear
        // reconstruction, so the four scattered values at a vertex sum to
        // that vertex's incoming adjoint, and summing over vertices and
        // channels the whole texture block must equal the scalar run's
        // gradient. THE IDENTITY DOES NOT DEPEND ON WHAT THE INDIVIDUAL
        // WEIGHTS ARE, which is exactly why it is worth asserting: the
        // shared reconstruction cannot make it true by construction, and a
        // weight that is individually wrong breaks it (Step 5 demonstrates
        // that: scaling w00 by 0.9 moves this total by 0.1*0.24 = 2.4% of
        // itself, about 65x the bound below, and this comparison rejects).
        //
        // THE BOUND IS DERIVED, NOT CHOSEN. Both sides are float32 sums
        // accumulated by atomicAdd in a scheduler-dependent order: each arena
        // element receives one add per hit vertex, i.e. N = capacity *
        // bounces of them, and a float32 sum of N non-negative terms carries
        // a relative error of at most (N-1) * 2^-24. Two such sums, plus one
        // rounding per weighted product on the texture side, is bounded by
        // 4*N*2^-24 relative -- deliberately the worst case, since the
        // observed value is far below it and is printed on the OK line.
        const double kAccumulations = static_cast<double>(kW) * kH * kBounces;
        const double conservationBound = 4.0 * kAccumulations * kFloat32Eps * referenceAdjointTotal;
        const double conservationError = std::fabs(scatteredTotal - referenceAdjointTotal);
        if (!(conservationError <= conservationBound)) {
            std::fprintf(
                stderr,
                "[diff_gpu_probe] FAIL: check 44 -- THE FOUR SCATTERED WEIGHTS DO NOT SUM TO THE "
                "INCOMING ADJOINT.\n"
                "  total scattered into the texture's %u gradient floats = %.12g\n"
                "  SUM over hit vertices of (dL.x+dL.y+dL.z), from a separate\n"
                "  DIFF_PARAM_EMISSION run at the same seed                = %.12g\n"
                "  |difference| = %.6g, derived bound = %.6g (%.0f atomic accumulations per\n"
                "  element, 4*N*2^-24 relative)\n"
                "  A bilinear reconstruction's four weights sum to exactly 1, so the four values\n"
                "  scattered at a vertex must sum to that vertex's adjoint whatever the weights\n"
                "  individually are. A mismatch means the weights do not sum to 1 (a wrong\n"
                "  interpolant, a dropped corner, a scaled weight) or that some vertices scatter\n"
                "  and others do not. The two runs walk the identical path -- neither the\n"
                "  throughput recursion nor Lr reads any emission -- so there is no sampling\n"
                "  difference to absorb this\n",
                kTexFloats, scatteredTotal, referenceAdjointTotal, conservationError,
                conservationBound, kAccumulations);
            wf.destroy(ctx.allocator());
            gradArena.destroy(ctx.allocator());
            return 1;
        }

        // --- PER CHANNEL. Valid because THIS scene's throughput is grey (a
        // grey albedo and a grey environment make every channel of dL the
        // same float), so each channel must carry exactly a third of the
        // total. It catches a scatter that applied a channel-dependent
        // weight or wrote one channel twice -- neither of which moves the
        // total the identity above compares.
        for (uint32_t c = 0; c < kTexShape.channels; ++c) {
            const double expected = referenceAdjointTotal / 3.0;
            if (!(std::fabs(perChannel[c] - expected) <= conservationBound)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 44 -- channel %u of the texture's "
                             "gradient block totals %.12g; this scene's throughput is grey, so "
                             "every channel must carry a third of %.12g (= %.12g) to within "
                             "%.6g\n",
                             c, perChannel[c], referenceAdjointTotal, expected, conservationBound);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }
        }

        std::printf(
            "[diff_gpu_probe] OK: check 44 -- THE BILINEAR SCATTER CONSERVES THE ADJOINT AND "
            "LANDS ONLY WHERE THE HOST SAYS IT MAY. A %ux%ux%u emission texture (element ordering "
            "k = (y*width + x)*channels + c, tied to the shader at startup), read at ONE texture "
            "coordinate (%.7g, %.7g) by every vertex of a %u-path, %u-bounce run: footprint "
            "texels (%u,%u) (%u,%u) (%u,%u) (%u,%u), weights %.4f %.4f %.4f %.4f.\n"
            "    CONSERVATION: the %u texture gradient floats total %.9g; an independent "
            "DIFF_PARAM_EMISSION run at the same seed (one atomicAdd of a channel sum, no weights "
            "in it at all) gives SUM_b dL = %.9g. |difference| %.4g <= derived bound %.4g "
            "(%.0f atomic accumulations per element).\n"
            "    FOOTPRINT: all %zu OTHER floats of the %u-float arena -- the 24 texture elements "
            "outside the footprint, the texture's Adam m/v state, both blocks of a scalar the "
            "scene does not depend on, and the 256-byte alignment padding -- are EXACTLY 0.0f, "
            "compared as floats. The 12 predicted indices were computed from the four texel "
            "coordinates of hostBilinearFootprint (a second, host-written bilinear "
            "reconstruction that calls nothing shader-derived) mapped through "
            "ParamShape::elementIndex alone, so a transposed or channel-planar shader ordering "
            "would be rejected from both sides.\n"
            "    PER CHANNEL: %.9g / %.9g / %.9g, each a third of the total (this scene's "
            "throughput is grey).\n",
            kTexShape.width, kTexShape.height, kTexShape.channels,
            static_cast<double>(kConstUvU), static_cast<double>(kConstUvV), kW * kH, kBounces,
            fp.x0, fp.y0, fp.x1, fp.y0, fp.x0, fp.y1, fp.x1, fp.y1,
            static_cast<double>(fp.w00), static_cast<double>(fp.w10),
            static_cast<double>(fp.w01), static_cast<double>(fp.w11), kTexFloats, scatteredTotal,
            referenceAdjointTotal, conservationError, conservationBound, kAccumulations,
            nullFloatsChecked, kGradArenaFloats, perChannel[0], perChannel[1], perChannel[2]);

        // === CHECK 45: the per-element magnitude gate, VARYING uv. ========
        //
        // The texture used as a texture: uv = position.xz / 16 + 0.5, which
        // maps the parity scene's floor (|x|, |z| <= 8) onto [0,1]^2, so
        // different vertices read different footprints and every texel
        // carries a different share of the gradient.
        //
        // WHAT THIS ADDS OVER CHECK 44. Conservation is blind to WHICH of the
        // four texels got what -- a scatter that gave one corner the whole
        // adjoint conserves the total exactly. This perturbs ONE primal texel
        // element on the HOST and compares (J(+h) - J(-h)) / 2h against the
        // single arena float the scatter wrote for that element. It is only
        // equal if the forward read and the adjoint agree about which element
        // and about the weight on it -- which is the "sharing is required"
        // property, MEASURED rather than argued from the fact that both call
        // one function.
        //
        // `h`, DERIVED THE WAY TASKS 2, 3 AND 4 WERE. Minimising
        // `E(h)/|J'| ~ eps*L/h + h^2/(6L^2)` gives `h* = (3*eps)^(1/3) * L`
        // with `eps = 2e-6` (a film value is a sum over bounces of a product
        // of about six float32 factors, each rounding at 2^-24) and `L` the
        // scale the film varies over in the parameter -- here the texel
        // values themselves, which sit around 0.6, the same L Task 4 used for
        // the emission scalar. h* ~= 1.090e-2, nearest power of two 2^-7.
        // As in Task 4 this is not the true optimum -- the truncation half is
        // identically zero because J is exactly linear in every texel -- and
        // it is kept for procedure consistency across all five parameters
        // this stage differentiates.
        //
        // THE MARGIN THIS BUYS IS THIN -- record the number rather than a
        // qualitative "resolves fine", per review (Task 5 Finding 7): the
        // worst observed bound/gradient ratio across the three tested
        // elements is 0.00662 against the pre-registered limit of 1e-2, i.e.
        // 1.5x -- the tightest margin of any check in this task. It fails
        // LOUD, refusing a verdict rather than passing falsely, so this is
        // fragility in the instrument's headroom, not a defect in the
        // gradient. A future scene where the emission texture's values sit
        // further from 0.6 (changing `L`) or a step derived differently
        // would need this margin re-checked before trusting it stays green.
        //
        // WHICH ELEMENTS ARE TESTED, and why they are chosen rather than
        // fixed. The finite difference's roundoff bound is set by the WHOLE
        // film's scale (eps*(|J+|+|J-|)/2h), while the quantity compared is
        // ONE element's share of the gradient -- so an element that only a
        // handful of vertices touch cannot be resolved against it at any h,
        // and a check pinned to such an element would refuse to claim a
        // verdict for a reason that has nothing to do with the scatter. The
        // three elements carrying the MOST gradient are used, read off the
        // centre run below. That is a selection on the gradient's own scale,
        // not on the agreement being measured: the comparison for each is
        // whatever it is.
        constexpr float kUvScale = 0.0625f;  // 1/16: the floor's |x|,|z| <= 8 onto [0,1]
        constexpr float kUvBias = 0.5f;
        constexpr float kStep = 0.0078125f;  // 2^-7 -- derived above
        constexpr double kMaxGradientResolution = 1e-2;

        std::vector<float> centreArenaBlock;
        {
            std::vector<float> filmC;
            const ohao::diff::WavefrontGradientOptions options = emissionTextureOptions(
                baseTexels, kTexShape, kUvScale, kUvScale, kUvBias, kUvBias);
            if (!ctx.runWavefrontGradientProbe(wf, kW, kH, kBounces, camera,
                                               std::span<const float>(positions),
                                               std::span<const uint32_t>(indices), kMatAlbedo,
                                               kTexMaterial, kGradientSeed, gradArena,
                                               kGradArenaFloats, kGradTexOffset, filmC, options)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 45 centre run dispatch\n");
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }
            centreArenaBlock = gradArena.readback(ctx.allocator(), texParam->gradBlock);
        }
        if (centreArenaBlock.size() != static_cast<std::size_t>(kTexFloats)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 45 -- the texture's gradient block read "
                         "back %zu floats, expected %u\n",
                         centreArenaBlock.size(), kTexFloats);
            wf.destroy(ctx.allocator());
            gradArena.destroy(ctx.allocator());
            return 1;
        }
        // NON-VACUITY: a VARYING uv must spread the gradient over more than
        // the one footprint check 44 pinned, or this configuration is not
        // actually exercising the texture as a texture.
        std::size_t nonzeroElements = 0;
        for (float value : centreArenaBlock) {
            if (value > 0.0f) ++nonzeroElements;
        }
        if (nonzeroElements <= 12u) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 45 -- only %zu of the %u texture gradient "
                         "elements are nonzero under a VARYING uv. Check 44's constant-uv run "
                         "already covers 12; this configuration exists to exercise more than one "
                         "footprint, and with 12 or fewer it is measuring nothing check 44 does "
                         "not\n",
                         nonzeroElements, kTexFloats);
            wf.destroy(ctx.allocator());
            gradArena.destroy(ctx.allocator());
            return 1;
        }
        std::vector<uint32_t> order(kTexFloats);
        for (uint32_t k = 0; k < kTexFloats; ++k) order[k] = k;
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return centreArenaBlock[a] > centreArenaBlock[b];
        });
        // THREE DISTINCT TEXELS, AND ALL THREE CHANNELS. Taking the three
        // largest elements outright would take the three CHANNELS OF ONE
        // TEXEL (this scene's throughput is grey, so a texel's three
        // elements are equal and sort adjacently), which would test one
        // texel three times and no channel but whichever sorted first. So
        // the i-th test is channel i of the i-th most-loaded TEXEL: three
        // different footprint positions and, between them, every channel.
        std::vector<uint32_t> chosen;
        std::set<uint32_t> chosenTexels;
        for (uint32_t k : order) {
            const uint32_t texel = k / kTexShape.channels;
            if (chosenTexels.count(texel) != 0) continue;
            chosenTexels.insert(texel);
            chosen.push_back(texel * kTexShape.channels +
                             static_cast<uint32_t>(chosen.size()) % kTexShape.channels);
            if (chosen.size() == 3u) break;
        }
        if (chosen.size() != 3u) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 45 -- only %zu distinct texels carry "
                         "gradient, so three different footprint positions cannot be tested\n",
                         chosen.size());
            wf.destroy(ctx.allocator());
            gradArena.destroy(ctx.allocator());
            return 1;
        }

        CrnFdMeasurement texMeasurements[3]{};
        uint32_t testedElements[3] = {0u, 0u, 0u};
        double worstTexRatio = 0.0;
        double worstTexResolution = 0.0;
        std::size_t texTraceMismatches = 0;
        for (std::size_t i = 0; i < 3; ++i) {
            const uint32_t element = chosen[i];
            testedElements[i] = element;
            if (!measureCrnEmissionTexelGradient(
                    ctx, wf, kW, kH, kBounces, camera, positions, indices, kMatAlbedo,
                    kTexMaterial, baseTexels, kTexShape, kUvScale, kUvScale, kUvBias, kUvBias,
                    element, kStep, kGradientSeed, gradArena, texParam->gradBlock,
                    kGradArenaFloats, kGradTexOffset, kFilmRelativeEps, texMeasurements[i])) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 45 measurement failed at element %u\n",
                             element);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }
            const CrnFdMeasurement& m = texMeasurements[i];
            texTraceMismatches += m.traceMismatches;

            // --- CRN VALIDITY, MEASURED. See this block's banner: the
            // structural argument is what closes it, this is what would
            // notice the structure changing.
            if (m.traceMismatches != 0u) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 45 at element %u -- %zu vertex-trace "
                             "geometry slots differ between a texel +/-h render and the centre "
                             "one. Plain common-random-number comparison is NOT valid if this is "
                             "nonzero: something now reads the binding-11 emission texture from "
                             "inside diffBsdfSample/diffBsdfSampleDetached or sampleEnvMap, which "
                             "would move a sampled direction and require Task 3's detached "
                             "instrument instead\n",
                             element, m.traceMismatches);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }

            if (!(m.jCenter > 0.0) || !std::isfinite(m.jCenter) || !(m.analytic > 0.0) ||
                !std::isfinite(m.analytic)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 45 at element %u: J = %.9g and the "
                             "scattered gradient = %.9g. Both must be finite and strictly "
                             "positive -- this element was chosen because it carries gradient, so "
                             "a zero means the arena readback and the selection disagree\n",
                             element, m.jCenter, m.analytic);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }

            const double resolution = m.errorBound / m.analytic;
            if (resolution > worstTexResolution) worstTexResolution = resolution;
            if (!(resolution <= kMaxGradientResolution)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 45 at element %u REFUSES TO CLAIM A "
                             "VERDICT: the derived error bound is %.6g, which is %.3g of the "
                             "gradient %.9g -- above the pre-registered %.3g. A pass at this "
                             "resolution would be compatible with there being nothing it could "
                             "have detected. roundoff %.6g (no truncation term -- J is exactly "
                             "linear in every texel) at h = %.9g\n",
                             element, m.errorBound, resolution, m.analytic,
                             kMaxGradientResolution, m.roundoffBound, m.hActual);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }

            // --- THE GATE, AND IT IS THE MAGNITUDE ASSERTION. It bounds
            // |finiteDiff - analytic| in ABSOLUTE terms against a
            // roundoff-only bound derived from the two films alone, and the
            // resolution check above has pre-registered that bound to resolve
            // to <= 1e-2 of the gradient's own scale. No separate ratio-to-1
            // test follows it, for the reason check 42 records: at this
            // gate's own resolution such a test could never fire first.
            const double ratio = m.absError / m.errorBound;
            if (ratio > worstTexRatio) worstTexRatio = ratio;
            const uint32_t exC = element % kTexShape.channels;
            const uint32_t exX = (element / kTexShape.channels) % kTexShape.width;
            const uint32_t exY = (element / kTexShape.channels) / kTexShape.width;
            if (!(m.absError <= m.errorBound)) {
                std::fprintf(
                    stderr,
                    "[diff_gpu_probe] FAIL: check 45 at texture element %u (texel (%u,%u) channel "
                    "%u) -- THE SCATTERED GRADIENT IS NOT THE DERIVATIVE OF THE FILM WITH RESPECT "
                    "TO THAT ELEMENT.\n"
                    "  finite difference (J(e+h) - J(e-h)) / 2h = %.12g\n"
                    "  arena float for this element              = %.12g\n"
                    "  |difference| = %.6g, which is %.6g of the gradient\n"
                    "  derived error bound = %.6g (roundoff only; J is exactly linear in every\n"
                    "  texel, so there is no truncation term)\n"
                    "  J(e-h) = %.12g, J(e) = %.12g, J(e+h) = %.12g, h = %.12g\n"
                    "  The forward read and this scatter go through ONE `diffBilinearFootprint`\n"
                    "  and ONE `diffTexelElementIndex`, so a disagreement here means the two\n"
                    "  stopped sharing them, or that the weight applied on the scatter side is\n"
                    "  not the weight the read used. Both sides describe ONE realisation at seed\n"
                    "  %u under common random numbers (measured 0 trace mismatches), so there is\n"
                    "  no sampling error and no truncation error to absorb it\n",
                    element, exX, exY, exC, m.finiteDiff, m.analytic, m.absError, m.relError,
                    m.errorBound,
                    m.jMinus, m.jCenter, m.jPlus, m.hActual, kGradientSeed);
                wf.destroy(ctx.allocator());
                gradArena.destroy(ctx.allocator());
                return 1;
            }
        }

        std::printf(
            "[diff_gpu_probe] OK: check 45 -- the per-element MAGNITUDE gate: at THREE DISTINCT "
            "TEXELS of a %ux%ux%u emission texture (channel i of the i-th most-loaded texel, so "
            "three footprint positions and all three channels between them), read at a VARYING uv "
            "(position.xz/16 + 0.5, so %zu of %u elements carry gradient), the single arena float "
            "the bilinear scatter wrote IS d(film)/d(that texel element), measured by perturbing "
            "that ONE primal float on the host. Common random numbers, seed %u, %u paths at one "
            "sample per pixel, %u bounces, h = 2^-7 = %.9g (derived: h* = (3*eps)^(1/3)*L at eps "
            "= %.0e, L ~ 0.6; the truncation half is identically zero because J is exactly linear "
            "in every texel). PLAIN CRN MEASURED VALID: %zu vertex-trace geometry mismatches "
            "across all 6 perturbed renders -- an emission texture reaches no sampler, so this "
            "parameter needs no detached instrument, and that is a statement about which callees "
            "read binding 11, not an inference from this count.\n"
            "    element %2u: FD %.9g vs analytic %.9g -- |err| %.4g <= bound %.4g\n"
            "    element %2u: FD %.9g vs analytic %.9g -- |err| %.4g <= bound %.4g\n"
            "    element %2u: FD %.9g vs analytic %.9g -- |err| %.4g <= bound %.4g\n"
            "  Worst |err|/bound %.4g; worst bound/gradient %.3g (pre-registered limit %.3g). "
            "THE GATE above IS the magnitude assertion; no separate ratio-to-1 check follows it\n",
            kTexShape.width, kTexShape.height, kTexShape.channels, nonzeroElements, kTexFloats,
            kGradientSeed, kW * kH, kBounces, static_cast<double>(kStep), kFilmRelativeEps,
            texTraceMismatches, testedElements[0], texMeasurements[0].finiteDiff,
            texMeasurements[0].analytic, texMeasurements[0].absError,
            texMeasurements[0].errorBound, testedElements[1], texMeasurements[1].finiteDiff,
            texMeasurements[1].analytic, texMeasurements[1].absError,
            texMeasurements[1].errorBound, testedElements[2], texMeasurements[2].finiteDiff,
            texMeasurements[2].analytic, texMeasurements[2].absError,
            texMeasurements[2].errorBound, worstTexRatio, worstTexResolution,
            kMaxGradientResolution);

        wf.destroy(ctx.allocator());
        gradArena.destroy(ctx.allocator());
    }

    arena.destroy(ctx.allocator());
    ctx.shutdown();
    std::printf("[diff_gpu_probe] PASS\n");
    return 0;
}
