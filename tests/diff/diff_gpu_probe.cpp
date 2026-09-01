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
// EIGHT GLSL/C++ ties run BEFORE any Vulkan object exists, and refuse to run
// the probe at all if they do not hold. In the order main() calls them:
// checkNeeStrideTie, checkScatterPushSizeTie, checkWfScatterSinkLayoutTie,
// checkDrawsPerBounceTie, checkTraverseInstantiationTie,
// checkBsdfShaderConstantTies, checkParityRefConstantsTie and
// checkTexelOrderingTie, which now live in probe/ties.{hpp,cpp} rather than
// this file's anonymous namespace. (This list said SIX and omitted
// checkDrawsPerBounceTie and checkTraverseInstantiationTie, both of which
// main() has always called and both of which print their own NOTE line; the
// `using` block below already named all eight, which is where the count
// should have been read off.) They print NOTE lines rather than OK lines:
// they are preconditions of the checks above meaning anything, not checks in
// their own right.

#include "gpu_probe_context.hpp"

// Everything this file used to carry inline. Stage 1 moved out the oracles,
// the shared scene, the finite-difference harnesses and the startup ties;
// Stage 2 moved out the checks themselves, one header per subsystem, listed
// here in the order main() runs them. All of it moved verbatim -- nothing
// about what any check compares changed.
#include "probe/checks_foundation.hpp"
#include "probe/checks_wavefront.hpp"
#include "probe/checks_bsdf.hpp"
#include "probe/checks_env.hpp"
#include "probe/checks_nee_film.hpp"
#include "probe/checks_parity.hpp"
#include "probe/checks_replay.hpp"
#include "probe/checks_adjoint_seed.hpp"
#include "probe/checks_convergence.hpp"
#include "probe/checks_gradients.hpp"
#include "probe/checks_texture.hpp"
#include "probe/ties.hpp"

#include "diff/device_caps.hpp"
#include "diff/grad/arena_layout.hpp"
#include "diff/grad/gradient_arena.hpp"

#include <cstddef>
#include <cstdio>

namespace {

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

// The checks themselves live in probe/checks_*.{hpp,cpp}, one file per
// subsystem. Each was one of main()'s top-level braced scopes and is still
// called from the same place in the same order; the only change is that a
// failure comes back as false rather than as a `return 1` from inside main().
//   probe/checks_foundation.cpp
using ohao::diff::probe::checkArenaAtomicsAndStage;
using ohao::diff::probe::checkRayQueryVisibility;
using ohao::diff::probe::checkRegistryArenaSeam;
using ohao::diff::probe::checkRngParity;
//   probe/checks_wavefront.cpp
using ohao::diff::probe::checkWavefrontBuffersZero;
using ohao::diff::probe::checkWavefrontGenerate;
using ohao::diff::probe::checkPathStateLayoutMapping;
using ohao::diff::probe::checkWavefrontIntersect;
using ohao::diff::probe::checkEmptyIndirectDispatch;
using ohao::diff::probe::checkWavefrontScatter;
using ohao::diff::probe::checkFusedBounceLoop;
using ohao::diff::probe::checkGeometricNormals;
//   probe/checks_bsdf.cpp
using ohao::diff::probe::checkBsdfTerms;
using ohao::diff::probe::checkFurnaces;
//   probe/checks_env.cpp
using ohao::diff::probe::checkEnvImportanceSampling;
using ohao::diff::probe::checkEnvPushFillAndShadowRay;
//   probe/checks_nee_film.cpp
using ohao::diff::probe::checkNeeMisAndRouting;
using ohao::diff::probe::checkFilmAccumulation;
//   probe/checks_parity.cpp
using ohao::diff::probe::checkIntegratorParity;
//   probe/checks_replay.cpp
using ohao::diff::probe::checkReplayEquivalence;
//   probe/checks_gradients.cpp
using ohao::diff::probe::checkAlbedoGradient;
using ohao::diff::probe::checkGgxGradients;
using ohao::diff::probe::checkAdjointSeed;
using ohao::diff::probe::checkAlbedoConvergence;
using ohao::diff::probe::checkEmissionConvergence;
using ohao::diff::probe::checkGgxConvergence;
using ohao::diff::probe::checkTextureConvergence;
using ohao::diff::probe::checkEmissionGradient;
//   probe/checks_texture.cpp
using ohao::diff::probe::checkTextureScatter;
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

    // 1, 2, 2b. Arena zero/readback, float atomics under contention, and the
    // ComputePipeline / WavefrontStage lifecycles that drive them. blockA is
    // the atomics target, blockB the second zeroed block, blockC the
    // WavefrontStage canary's own block -- see the function's own commentary
    // for why 2b cannot reuse blockA. `layout` is needed as well as the
    // indices: 2b resolves blockC's absolute float offset through it.
    if (!checkArenaAtomicsAndStage(ctx, layout, arena, blockA, blockB, blockC)) return 1;
    // 3, 4. Ray-query visibility against an analytic plane, then the half-quad
    // that pins the camera Y orientation the closed form is blind to.
    if (!checkRayQueryVisibility(ctx)) return 1;
    // 5. A ParamRegistry block index resolves against an arena built from that
    // registry's layout.
    if (!checkRegistryArenaSeam(ctx)) return 1;
    // 6. CPU/GPU RNG parity, values and draw count.
    if (!checkRngParity(ctx)) return 1;
    // 7. WavefrontBuffers build + zero, every field of every path.
    if (!checkWavefrontBuffersZero(ctx)) return 1;
    // 8-10. wf_generate.comp: closed-form directions, field round-trip, and a
    // race-free queue/counter population.
    if (!checkWavefrontGenerate(ctx)) return 1;
    // 11. path_state_layout.hpp and path_state.glsl agree field by field.
    if (!checkPathStateLayoutMapping(ctx)) return 1;
    // 12. wf_intersect.comp compacts survivors exactly, across an indirectly
    // sized dispatch.
    if (!checkWavefrontIntersect(ctx)) return 1;
    // 13. An indirect dispatch sized from a live count of 0 launches nothing.
    if (!checkEmptyIndirectDispatch(ctx)) return 1;
    // 14-15. wf_scatter.comp over 4 real bounces: exact throughput decay and
    // per-bounce RNG parity across dispatch boundaries.
    if (!checkWavefrontScatter(ctx)) return 1;
    // 16-18. The same two properties re-proved with the bounce loop fused into
    // one command buffer through WavefrontLoop.
    if (!checkFusedBounceLoop(ctx)) return 1;
    // 19. wf_intersect.comp's stored geometric normals against a closed box.
    if (!checkGeometricNormals(ctx)) return 1;
    // 20. f, the sampling pdf and the sampler weight against an independent CPU
    // oracle.
    if (!checkBsdfTerms(ctx)) return 1;
    // 21-23. White, glossy and mixture furnaces, at q = 0, q = 1 and an
    // intermediate q.
    if (!checkFurnaces(ctx)) return 1;
    // 24-26. The chi-squared goodness-of-fit test, the pdf range and texel-centre
    // inversion, and the pdfs integrating to 1.
    if (!checkEnvImportanceSampling(ctx)) return 1;
    // 27-28. WavefrontLoop::record's own envWidth/envHeight fill against a
    // non-square environment, and THE SHADOW RAY EXISTS.
    if (!checkEnvPushFillAndShadowRay(ctx)) return 1;
    // 29-31. The three estimators of one direct-lighting integral, the pointwise
    // MIS partition, and what nothing tested before: envIntegral, pdfEnvMap and
    // the routing claim.
    if (!checkNeeMisAndRouting(ctx)) return 1;
    // 32. The film equals the sum of the per-bounce contributions, reconstructed
    // on the host from primitives the sink records separately.
    if (!checkFilmAccumulation(ctx)) return 1;
    // 33-34. THE STAGE GATE. Two unbiased estimators of one integral agree only
    // if both are unbiased -- per pixel at a family-wise-corrected z, and pooled
    // on the image total with a convergence assertion a fixed bias cannot meet.
    if (!checkIntegratorParity(ctx)) return 1;
    // 35-36. NO GRADIENT IS INVOLVED: the forward trace against a CPU PathRng the
    // GPU never sees, then the replay instantiation's trace against it.
    if (!checkReplayEquivalence(ctx)) return 1;
    // 37-38. THE FIRST GRADIENT: a common-random-number finite difference against
    // what the replay hook scattered into the arena, and the null test.
    if (!checkAlbedoGradient(ctx)) return 1;
    // 39-41. df/droughness and df/dmetallic against a DETACHED finite difference,
    // plus the detached-sampling bias measured rather than assumed away.
    if (!checkGgxGradients(ctx)) return 1;
    // 42-43. dJ/d(emission), the plumbing test for the gradient path, and its own
    // null test.
    if (!checkEmissionGradient(ctx)) return 1;
    // 44-45. The first parameter that is not a scalar: the conservation identity
    // plus an exact-zero footprint gate, then the per-element magnitude gate.
    if (!checkTextureScatter(ctx)) return 1;
    // 46-49. STAGE 1 TASK 6 -- THE FOUR GATES, each at TWO STEP SIZES. Not
    // four more agreements: the pair (D(h), D(2h)) measures the truncation
    // term and SUBTRACTS it, so what is compared against the arena is the
    // gradient's own error rather than a bound that contains it. The four
    // parameters obey three different convergence laws and each is asserted
    // in the direction its own analytic form dictates -- see
    // checks_convergence.hpp.
    if (!checkAlbedoConvergence(ctx)) return 1;
    if (!checkGgxConvergence(ctx)) return 1;
    if (!checkEmissionConvergence(ctx)) return 1;
    if (!checkTextureConvergence(ctx)) return 1;

    // 50. STAGE 2 TASK 1: the adjoint seed IS dL/d(film), and the
    // sum-of-film objective every check above measures is its w = 1 case.
    if (!checkAdjointSeed(ctx)) return 1;

    arena.destroy(ctx.allocator());
    ctx.shutdown();
    std::printf("[diff_gpu_probe] PASS\n");
    return 0;
}
