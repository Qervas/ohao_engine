// The gradients, checks 37-43: dJ/d(albedo) and its null test, the GGX
// roughness/metallic adjoints with the detached-sampling bias measured,
// and dJ/d(emission) with its own null test.
//
// Lifted verbatim out of diff_gpu_probe.cpp, commentary and all.
#include "probe/checks_gradients.hpp"

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
#include <string>
#include <vector>

namespace ohao::diff::probe {

// The oracles, scene and finite-difference harnesses these checks call are
// in this same namespace, so the `using ohao::diff::probe::...` block
// diff_gpu_probe.cpp needed to reach them is not repeated here.

bool checkAlbedoGradient(ohao::diff::GpuProbeContext& ctx) {
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
        return false;
    }
    const ohao::diff::DiffParam* albedoParam = gradReg.find("albedo");
    const ohao::diff::DiffParam* unusedParam = gradReg.find("unused_scalar");
    if (albedoParam == nullptr || unusedParam == nullptr) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 37 registered params not found\n");
        return false;
    }

    ohao::diff::GradientArena gradArena;
    if (!gradArena.build(ctx.allocator(), gradReg.layout())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 37 gradient arena build\n");
        return false;
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
        return false;
    }

    ohao::diff::WavefrontBuffers wf;
    if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH) ||
        !wf.uploadEnvironment(gradEnvCdf.marginalSpan(), gradEnvCdf.conditionalSpan(),
                              gradEnvCdf.integral())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 37 buffers build / env CDF upload\n");
        wf.destroy(ctx.allocator());
        gradArena.destroy(ctx.allocator());
        return false;
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
            return false;
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
            return false;
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
            return false;
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
            return false;
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
        return false;
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
        return false;
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
            return false;
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
            return false;
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
    return true;
}

bool checkGgxGradients(ohao::diff::GpuProbeContext& ctx) {
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
        return false;
    }
    const ohao::diff::DiffParam* ggxParam = ggxReg.find("ggx_scalar");
    if (ggxParam == nullptr) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 39 registered param not found\n");
        return false;
    }

    ohao::diff::GradientArena ggxArena;
    if (!ggxArena.build(ctx.allocator(), ggxReg.layout())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 39 gradient arena build\n");
        return false;
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
        return false;
    }

    ohao::diff::WavefrontBuffers wf;
    if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH) ||
        !wf.uploadEnvironment(ggxEnvCdf.marginalSpan(), ggxEnvCdf.conditionalSpan(),
                              ggxEnvCdf.integral())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 39 buffers build / env CDF upload\n");
        wf.destroy(ctx.allocator());
        ggxArena.destroy(ctx.allocator());
        return false;
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
        return false;
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
        return false;
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
                return false;
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
                return false;
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
            return false;
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
    return true;
}

bool checkEmissionGradient(ohao::diff::GpuProbeContext& ctx) {
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
        return false;
    }
    const ohao::diff::DiffParam* emissionParam = gradReg.find("emission");
    const ohao::diff::DiffParam* unusedParam = gradReg.find("unused_scalar_emission");
    if (emissionParam == nullptr || unusedParam == nullptr) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 42 registered params not found\n");
        return false;
    }

    ohao::diff::GradientArena gradArena;
    if (!gradArena.build(ctx.allocator(), gradReg.layout())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 42 gradient arena build\n");
        return false;
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
        return false;
    }

    ohao::diff::WavefrontBuffers wf;
    if (!wf.build(ctx.allocator(), kW * kH, kEnvW, kEnvH) ||
        !wf.uploadEnvironment(gradEnvCdf.marginalSpan(), gradEnvCdf.conditionalSpan(),
                              gradEnvCdf.integral())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 42 buffers build / env CDF upload\n");
        wf.destroy(ctx.allocator());
        gradArena.destroy(ctx.allocator());
        return false;
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
            return false;
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
            return false;
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
            return false;
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
            return false;
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
            return false;
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
        return false;
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
        return false;
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
            return false;
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
            return false;
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
    return true;
}

}  // namespace ohao::diff::probe
