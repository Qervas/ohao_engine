// The texture scatter, checks 44-45: the bilinear weights conserve the
// incoming adjoint and land only in the host-predicted footprint, and the
// per-element magnitude gate.
//
// Lifted verbatim out of diff_gpu_probe.cpp, commentary and all.
#include "probe/checks_texture.hpp"

#include "probe/fd_harness.hpp"
#include "probe/scene.hpp"

#include "diff/grad/arena_layout.hpp"
#include "diff/grad/gradient_arena.hpp"
#include "diff/param/param_registry.hpp"
#include "diff/wavefront/wavefront_buffers.hpp"
#include "diff/wavefront/wavefront_loop.hpp"
#include "render/rt/env_cdf.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace ohao::diff::probe {

// The oracles, scene and finite-difference harnesses these checks call are
// in this same namespace, so the `using ohao::diff::probe::...` block
// diff_gpu_probe.cpp needed to reach them is not repeated here.

bool checkTextureScatter(ohao::diff::GpuProbeContext& ctx) {
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
        return false;
    }
    const ohao::diff::DiffParam* texParam = gradReg.find("emission_tex");
    const ohao::diff::DiffParam* scalarParam = gradReg.find("emission_scalar");
    const ohao::diff::DiffParam* unusedParam = gradReg.find("unused_scalar_tex");
    if (texParam == nullptr || scalarParam == nullptr || unusedParam == nullptr) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 44 registered params not found\n");
        return false;
    }
    if (texParam->floatCount != kTexFloats) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 44 -- the registry gave the texture "
                     "parameter %u floats, its shape says %u\n",
                     texParam->floatCount, kTexFloats);
        return false;
    }

    ohao::diff::GradientArena gradArena;
    if (!gradArena.build(ctx.allocator(), gradReg.layout())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 44 gradient arena build\n");
        return false;
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
        return false;
    }

    ohao::diff::WavefrontBuffers wf;
    if (!wf.build(ctx.allocator(), kW * kH, kEnvW, kEnvH) ||
        !wf.uploadEnvironment(gradEnvCdf.marginalSpan(), gradEnvCdf.conditionalSpan(),
                              gradEnvCdf.integral())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 44 buffers build / env CDF "
                              "upload\n");
        wf.destroy(ctx.allocator());
        gradArena.destroy(ctx.allocator());
        return false;
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
        return false;
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
            return false;
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
        return false;
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
            return false;
        }
        const std::vector<float> arenaAfterScalar = gradArena.readbackAll(ctx.allocator());
        if (arenaAfterScalar.size() != static_cast<std::size_t>(kGradArenaFloats)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 44 scalar-run arena readback "
                                  "size\n");
            wf.destroy(ctx.allocator());
            gradArena.destroy(ctx.allocator());
            return false;
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
                return false;
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
        return false;
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
            return false;
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
            return false;
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
        return false;
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
            return false;
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
            return false;
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
        return false;
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
        return false;
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
        return false;
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
            return false;
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
            return false;
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
            return false;
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
            return false;
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
            return false;
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
    return true;
}

}  // namespace ohao::diff::probe
