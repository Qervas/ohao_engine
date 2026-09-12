// Gate 4 leg 2 (spec 8.4), check 72. The long argument for why this gate is
// not redundant with checks 33-34 and 37 is in the header; the argument for
// the environment-resolution ladder is below, at kEnvHeights.
#include "probe/checks_mitsuba_scale.hpp"

#include "diff/diff_renderer.hpp"
#include "diff/wavefront/gradient_stages.hpp"
#include "diff/wavefront/wavefront_buffers.hpp"
#include "render/rt/env_cdf.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace ohao::diff::probe {

namespace {

// ===========================================================================
// THE PRE-REGISTERED CRITERION, fixed before the first comparison.
// ===========================================================================
//
// kAlbedo and kEnvRadiance MUST equal ALBEDO and ENV_RADIANCE in
// tests/diff/tools/mitsuba_gate.py. That script asserts it against this
// source rather than trusting the coincidence: two renderers agreeing about
// different scenes is not agreement.
constexpr float kAlbedo = 0.5f;
constexpr float kEnvRadiance = 1.0f;

// ONE BOUNCE, which is what makes the closed form exact. A second bounce
// would add interreflection the analytic answer does not contain -- though on
// this scene there is none to add, the plate being flat and alone, which is
// itself why the scene is a plate alone.
constexpr std::uint32_t kBounces = 1u;

// The film. `runWavefrontGradientProbe` fixes height at 8 and requires
// width*height == capacity, which is also its one-sample-per-pixel condition,
// so MORE SAMPLES MEANS MORE PIXELS OR MORE SEEDS and never more spp. Pixels
// are much the cheaper of the two -- a seed is a whole dispatch, a fence and
// a readback -- so the film is a wide strip and the seed count is modest.
constexpr std::uint32_t kW = 256u, kH = 8u;
constexpr std::uint32_t kCapacity = kW * kH;
// 512 * 2048 = 1048576 independent primary paths per configuration. If the
// resolution gate below ever refuses a verdict, raise THIS rather than the
// tolerance.
constexpr std::uint32_t kSeeds = 512u;
constexpr std::uint32_t kSeedBase = 20260912u;

// ===========================================================================
// THE ENVIRONMENT-RESOLUTION LADDER, and the bias it exists to measure
// ===========================================================================
//
// THIS GATE FOUND A REAL BIAS ON ITS FIRST RUN, and the ladder is what turns
// that finding into an assertion instead of a widened tolerance.
//
// At a 64x32 environment the film came out +0.124% above a*L, which is 5.1
// standard errors at two million samples -- small, systematic, and exactly
// the class of error every other check in this suite is blind to.
//
// WHERE IT COMES FROM. `sampleEnvMap` (shaders/includes/rt/env_sampling.glsl)
// emits TEXEL CENTRES: having chosen a texel from the CDF it returns
// equirectPixelToDir of that texel's midpoint, never a point inside it. The
// environment strategy's expectation is therefore a MIDPOINT QUADRATURE over
// the map rather than the direct-lighting integral. That is not a new
// discovery -- site/content/units/sampling/env-cdf.md states it in those
// words -- but nothing had ever MEASURED it, because measuring it needs an
// absolute oracle and every oracle this subsystem had was a relative one.
//
// WHAT THE QUADRATURE COSTS, derived rather than fitted. The integrand over
// the map is f*cos(theta)*L, and for this scene the surface normal is +Y,
// which is exactly the environment's own polar axis (equirectPixelToDir puts
// theta = 0 at +Y), so the integrand depends on theta alone and the error is
// the composite midpoint rule's in that one variable. With Delta = pi/H,
// numerator and denominator each picking up Delta^2/12, the environment
// strategy's estimate comes out
//
//     a * L * (1 + Delta^2 / 6) + O(Delta^4),
//
// = +0.16% at H = 32, against the +0.124% measured for the MIS combination of
// this strategy with the unbiased BSDF one. THE WIDTH OF THE MAP DOES NOT
// ENTER -- the integrand has no phi dependence here -- which is why the
// ladder below varies H alone and holds W fixed. That isolates the polar
// quadrature: a bias from somewhere else would not care which of the two
// resolutions moved.
//
// SO THE LADDER ASSERTS THE ORDER, exactly as check 70 does for the boundary
// term's own midpoint rule, and the ABSOLUTE comparison is made at a
// resolution where the derived bias is provably far below the sampling error.
// Halving Delta must quarter the excess.
constexpr std::uint32_t kEnvW = 64u;
constexpr std::uint32_t kEnvHeights[3] = {8u, 16u, 32u};
// The gate's own resolution. Delta^2/6 at H = 256 is 2.5e-05, which is a
// fifth of one standard error at the sample count above -- so the closed form
// is the answer to this configuration to well inside what the gate can see.
constexpr std::uint32_t kEnvHeightFine = 256u;
constexpr double kPredictedRatio = 4.0;
// Same shape as check 70's: generous enough that the O(Delta^4) term at the
// coarse end does not decide the verdict, tight enough that 2 (a first-order
// error) and 1 (no convergence at all) both fail.
constexpr double kRatioTol = 1.0;
// The coarsest excess must be PRESENT before any ratio is taken. An error
// already at the sampling floor has no order and the ratios would report
// whatever the noise did -- check 70's precondition, for check 70's reason.
constexpr double kMinCoarseSigmas = 20.0;

// tan of the vertical half-angle. With aspect 32 the horizontal half-angle is
// atan(1.6) = 58 degrees; the geometric guard below turns both into a landing
// point and requires margin against the plate. NOT shared with the Python
// side: a*L is view-independent, so the two renderers need not agree about
// the camera, and pretending they do would be a tie that is not one.
constexpr float kTanHalfFov = 0.05f;
// The camera sits this far above the plate, looking straight down.
constexpr float kCameraHeight = 1.0f;
// Half-extent of the plate.
constexpr float kPlateHalfExtent = 8.0f;
constexpr double kMinPlateMargin = 1.0;

// THE GATE. A multiple of the standard error MEASURED across seeds, plus a
// floor that refuses a verdict the sampling could not have resolved -- the
// same two-part structure as check 37, for the same reason: a pass at a
// resolution coarser than the effect is compatible with there being nothing
// the check could have detected.
constexpr double kSigmas = 4.0;
constexpr double kMaxResolution = 0.005;
// a * gradient == film is an identity at one bounce (see the header), broken
// only by the order two atomicAdds accumulated the same terms in.
constexpr double kLinearityTol = 1e-4;
// Non-vacuity: no pixel may have missed the plate (which would read exactly
// 0 every seed) or been lit by something other than the environment. Loose on
// purpose -- the geometric guard is what pins the framing; this catches a
// whole pixel going dark or bright, not a sampling fluctuation.
constexpr double kPerPixelLow = 0.2;
constexpr double kPerPixelHigh = 5.0;

/// The plate: one quad at y = 0, wound so the geometric normal is -Y, which
/// is what wf_intersect.comp FLIPS to +Y for a downward primary ray. Same
/// winding as buildParityScene's floor, deliberately -- a quad wound the
/// other way would be shaded with an inverted normal and the closed form
/// would not apply.
void buildPlate(std::vector<float>& positions, std::vector<std::uint32_t>& indices) {
    const float r = kPlateHalfExtent;
    positions = {-r, 0.0f, -r, r, 0.0f, -r, r, 0.0f, r, -r, 0.0f, r};
    indices = {0u, 1u, 2u, 0u, 2u, 3u};
}

/// A constant environment of grey radiance `kEnvRadiance`. RGB all equal, so
/// the CDF's grey channel (0.2126 R + 0.7152 G + 0.0722 B, the only
/// environment data the scatter stage has) is exactly that radiance.
void buildConstantEnvironment(std::uint32_t envH, std::vector<float>& outRgba) {
    outRgba.assign(static_cast<std::size_t>(kEnvW) * envH * 4u, kEnvRadiance);
    for (std::size_t k = 3u; k < outRgba.size(); k += 4u) outRgba[k] = 1.0f;
}

ohao::diff::WavefrontGenerateCamera plateCamera() {
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
    return camera;
}

/// Where the most extreme primary ray lands, in the plate's own plane.
///
/// Derived here rather than inferred from the rendered film: "every pixel saw
/// the surface" is the precondition the closed form rests on, and reading it
/// back out of the film would make the check's own framing depend on the
/// numbers it is about to judge. camera_ray.glsl's construction, transcribed:
/// ndc in [-1,1] at pixel centres, x scaled by the aspect ratio, y scaled by
/// tanHalfFov alone.
void extremePrimaryHit(double& outAbsX, double& outAbsZ) {
    const double aspect = static_cast<double>(kW) / static_cast<double>(kH);
    const double ndcX = 2.0 * (static_cast<double>(kW - 1u) + 0.5) / static_cast<double>(kW) - 1.0;
    const double ndcY = 1.0 - 2.0 * 0.5 / static_cast<double>(kH);
    const double sx = ndcX * aspect * static_cast<double>(kTanHalfFov);
    const double sz = ndcY * static_cast<double>(kTanHalfFov);
    // dir = normalize(forward + right*sx + up*sz) = normalize(sx, -1, -sz).
    // The plate is at y = 0 and the camera at y = kCameraHeight, so the
    // normalisation divides out: the landing point is height * (sx, ., -sz).
    outAbsX = std::fabs(static_cast<double>(kCameraHeight) * sx);
    outAbsZ = std::fabs(static_cast<double>(kCameraHeight) * sz);
}

/// One environment resolution's worth of measurement.
struct Measurement {
    std::uint32_t envH{0};
    double filmMean{0.0};
    double filmStdErr{0.0};
    double gradMean{0.0};
    double gradStdErr{0.0};
    /// Smallest per-film-float average over seeds, and the largest. Non-
    /// vacuity: a pixel that missed the plate reads exactly 0 every seed.
    double minPixelMean{0.0};
    double maxPixelMean{0.0};
};

void meanAndStdErr(const std::vector<double>& v, double& outMean, double& outStdErr) {
    const double n = static_cast<double>(v.size());
    double sum = 0.0;
    for (double x : v) sum += x;
    outMean = sum / n;
    double ss = 0.0;
    for (double x : v) ss += (x - outMean) * (x - outMean);
    // Bessel's correction, then the standard error of the MEAN.
    outStdErr = (v.size() > 1u) ? std::sqrt(ss / (n - 1.0) / n) : 0.0;
}

}  // namespace

bool checkMitsubaScale(ohao::diff::GpuProbeContext& ctx) {
    // --- THE FRAMING, before any Vulkan work. If the extreme primary ray
    // lands off the plate, some pixel reads the environment directly rather
    // than a*L and the closed form is not the answer to this scene.
    double extremeX = 0.0, extremeZ = 0.0;
    extremePrimaryHit(extremeX, extremeZ);
    const double margin = static_cast<double>(kPlateHalfExtent) - std::fmax(extremeX, extremeZ);
    if (!(margin >= kMinPlateMargin)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 72 -- the extreme primary ray lands at "
                     "|x| = %.6g, |z| = %.6g on a plate of half-extent %.6g, a margin of %.6g "
                     "against the pre-registered %.6g. SOME PIXEL DOES NOT SEE THE SURFACE, so "
                     "it reads the environment radiance %.6g instead of a*L = %.6g and the "
                     "closed form this check compares against is not the answer to this "
                     "scene. Widen kPlateHalfExtent or narrow the frame\n",
                     extremeX, extremeZ, static_cast<double>(kPlateHalfExtent), margin,
                     kMinPlateMargin, static_cast<double>(kEnvRadiance),
                     static_cast<double>(kAlbedo) * static_cast<double>(kEnvRadiance));
        return false;
    }

    // --- The facade owns the registry and the arena, as an engine would.
    ohao::diff::DiffRenderer renderer;
    if (!renderer.init(ctx.device(), ctx.physicalDevice())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 72 -- init\n");
        return false;
    }
    const ohao::diff::RegisterResult reg = renderer.registerScalarBlock("albedo", 1u);
    if (!reg.ok || !renderer.build(ctx.allocator())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 72 -- register/build: %s\n",
                     reg.error.c_str());
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }
    const ohao::diff::DiffParam* albedoParam = renderer.registry().get(reg.id);
    const std::uint32_t kArenaFloats =
        static_cast<std::uint32_t>(renderer.registry().layout().totalBytes() / sizeof(float));
    const std::uint32_t kOffset = static_cast<std::uint32_t>(
        renderer.registry().layout().block(albedoParam->gradBlock).offsetBytes / sizeof(float));

    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    buildPlate(positions, indices);
    const ohao::diff::WavefrontGenerateCamera camera = plateCamera();
    // Pure Lambert: roughness 1, metallic 0, specular weight 0. Not a
    // default -- it is the only configuration in which bsdf.glsl's per-bounce
    // weight is EXACTLY the albedo, which is what makes f = a/pi and the
    // closed form a/pi * pi * L.
    const ohao::diff::WavefrontScatterMaterial kMaterial{1.0f, 0.0f, 0.0f};

    // The geometry and the five pipelines are the same for every environment
    // resolution, so they are built once and the environment alone varies.
    // That is also what makes the ladder's conclusion attributable: nothing
    // else differs between its rungs.
    ohao::diff::GpuProbeContext::OwnedScene scene;
    if (!ctx.buildOwnedScene(std::span<const float>(positions),
                             std::span<const std::uint32_t>(indices), scene)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 72 -- buildOwnedScene\n");
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }
    const ohao::diff::WavefrontGradientOptions::PrebuiltScene sceneHandle = scene.handle();
    ohao::diff::GradientStages stages;

    auto tearDown = [&]() {
        ctx.destroyOwnedStages(stages);
        ctx.destroyOwnedScene(scene);
        (void)renderer.shutdown(&ctx.allocator());
    };

    const std::size_t kFilmFloats = static_cast<std::size_t>(kW) * kH * 3u;
    const double expectedFilm = static_cast<double>(kAlbedo) * static_cast<double>(kEnvRadiance);
    const double expectedGrad = static_cast<double>(kEnvRadiance);
    const std::uint32_t samples = kSeeds * kCapacity;

    // Render `kSeeds` independent frames at one environment resolution.
    auto measure = [&](std::uint32_t envH, Measurement& out) -> bool {
        out = Measurement{};
        out.envH = envH;

        std::vector<float> envRgba;
        buildConstantEnvironment(envH, envRgba);
        ohao::EnvCDF cdf;
        cdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(envH));
        ohao::diff::WavefrontBuffers wf;
        if (!cdf.valid() || !wf.build(ctx.allocator(), kCapacity, kEnvW, envH) ||
            !wf.uploadEnvironment(cdf.marginalSpan(), cdf.conditionalSpan(), cdf.integral())) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 72 -- environment setup at %ux%u\n",
                         kEnvW, envH);
            wf.destroy(ctx.allocator());
            return false;
        }

        std::vector<double> perSeedFilmMean(kSeeds, 0.0);
        std::vector<double> perSeedGradMean(kSeeds, 0.0);
        std::vector<double> perPixelSum(kFilmFloats, 0.0);

        for (std::uint32_t s = 0; s < kSeeds; ++s) {
            ohao::diff::WavefrontGradientOptions options;
            options.diffParam = 0u;  // DIFF_PARAM_BASECOLOR
            options.scene = &sceneHandle;
            options.stages = &stages;
            std::vector<float> film;
            if (!ctx.runWavefrontGradientProbe(
                    wf, kW, kH, kBounces, camera, std::span<const float>(positions),
                    std::span<const std::uint32_t>(indices), kAlbedo, kMaterial, kSeedBase + s,
                    renderer.arena(), kArenaFloats, kOffset, film, options)) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 72 -- render at seed %u, env %ux%u\n",
                             kSeedBase + s, kEnvW, envH);
                wf.destroy(ctx.allocator());
                return false;
            }
            if (film.size() != kFilmFloats) {
                std::fprintf(
                    stderr, "[diff_gpu_probe] FAIL: check 72 -- film of %zu floats, expected %zu\n",
                    film.size(), kFilmFloats);
                wf.destroy(ctx.allocator());
                return false;
            }
            double filmSum = 0.0;
            for (std::size_t i = 0; i < kFilmFloats; ++i) {
                const double v = static_cast<double>(film[i]);
                if (!std::isfinite(v) || v < 0.0) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: check 72 -- film float %zu at seed %u is "
                                 "%.9g, which is not a finite non-negative radiance\n",
                                 i, kSeedBase + s, v);
                    wf.destroy(ctx.allocator());
                    return false;
                }
                filmSum += v;
                perPixelSum[i] += v;
            }
            const std::vector<float> gradBlock =
                renderer.arena().readback(ctx.allocator(), albedoParam->gradBlock);
            if (gradBlock.empty() || !std::isfinite(static_cast<double>(gradBlock[0]))) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 72 -- gradient block read back empty "
                             "or non-finite at seed %u\n",
                             kSeedBase + s);
                wf.destroy(ctx.allocator());
                return false;
            }
            perSeedFilmMean[s] = filmSum / static_cast<double>(kFilmFloats);
            perSeedGradMean[s] =
                static_cast<double>(gradBlock[0]) / static_cast<double>(kFilmFloats);
        }
        wf.destroy(ctx.allocator());

        meanAndStdErr(perSeedFilmMean, out.filmMean, out.filmStdErr);
        meanAndStdErr(perSeedGradMean, out.gradMean, out.gradStdErr);
        out.minPixelMean = perPixelSum[0] / static_cast<double>(kSeeds);
        out.maxPixelMean = out.minPixelMean;
        for (std::size_t i = 1; i < kFilmFloats; ++i) {
            const double m = perPixelSum[i] / static_cast<double>(kSeeds);
            if (m < out.minPixelMean) out.minPixelMean = m;
            if (m > out.maxPixelMean) out.maxPixelMean = m;
        }
        return true;
    };

    // --- The ladder, coarse to fine, then the fine configuration the gate
    // itself is taken at.
    Measurement ladder[3]{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!measure(kEnvHeights[i], ladder[i])) {
            tearDown();
            return false;
        }
    }
    Measurement fine{};
    if (!measure(kEnvHeightFine, fine)) {
        tearDown();
        return false;
    }
    tearDown();

    // THE LINE tests/diff/tools/mitsuba_gate.py PARSES, from the FINE
    // configuration -- the one the closed form is the answer to. Printed
    // before any verdict below, so that a FAILING run still hands the Python
    // side the numbers to compare: a gate whose output disappears on failure
    // is a gate you cannot debug from the other renderer's side.
    std::printf(
        "[diff_gpu_probe] MITSUBA-GATE albedo=%.9g env=%.9g envH=%u samples=%u meanFilm=%.9g "
        "seMeanFilm=%.9g dMeanDAlbedo=%.9g seDMeanDAlbedo=%.9g\n",
        static_cast<double>(kAlbedo), static_cast<double>(kEnvRadiance), kEnvHeightFine, samples,
        fine.filmMean, fine.filmStdErr, fine.gradMean, fine.gradStdErr);

    // --- NON-VACUITY 1: every pixel saw the lit surface, at every rung. A
    // pixel whose primary ray missed the plate accumulates nothing at all and
    // reads exactly 0, which no comparison of the MEAN would notice if the
    // rest of the frame compensated.
    const Measurement* all[4] = {&ladder[0], &ladder[1], &ladder[2], &fine};
    for (const Measurement* m : all) {
        if (!(m->minPixelMean > kPerPixelLow * expectedFilm) ||
            !(m->maxPixelMean < kPerPixelHigh * expectedFilm)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 72 at env %ux%u -- per-pixel averages over "
                         "%u seeds run [%.9g, %.9g], outside [%.6g, %.6g] around the expected "
                         "a*L = %.9g. A pixel at exactly 0 missed the plate (the geometric guard "
                         "above says none should); one far above it is being lit by something "
                         "other than a constant environment\n",
                         kEnvW, m->envH, kSeeds, m->minPixelMean, m->maxPixelMean,
                         kPerPixelLow * expectedFilm, kPerPixelHigh * expectedFilm, expectedFilm);
            return false;
        }
    }

    // --- THE IDENTITY, asserted rather than assumed, at every rung. At one
    // bounce under pure Lambert the film is exactly a times the gradient; if
    // it is not, the two quantities below are not the two quantities this
    // check thinks it measured and neither comparison means what it says.
    for (const Measurement* m : all) {
        const double lhs = static_cast<double>(kAlbedo) * m->gradMean;
        const double rel = std::fabs(lhs - m->filmMean) / m->filmMean;
        if (!(rel <= kLinearityTol)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 72 at env %ux%u -- a * gradient = %.12g "
                         "but the film mean is %.12g, a relative %.4g against a tolerance of "
                         "%.4g. These are the SAME SUM over the same samples at one bounce (the "
                         "MIS weights and both densities are independent of the albedo under "
                         "pure Lambert), so they can differ only by the order two atomicAdds "
                         "accumulated them in. A larger disagreement means the gradient is not "
                         "the derivative of THIS film\n",
                         kEnvW, m->envH, lhs, m->filmMean, rel, kLinearityTol);
            return false;
        }
    }

    // --- THE LADDER: the environment strategy's midpoint quadrature, and its
    // order. See kEnvHeights for the derivation of the predicted 4.
    double excess[3] = {0.0, 0.0, 0.0};
    for (std::size_t i = 0; i < 3; ++i) excess[i] = ladder[i].filmMean / expectedFilm - 1.0;

    const double coarseSigmas = excess[0] * expectedFilm / ladder[0].filmStdErr;
    if (!(coarseSigmas >= kMinCoarseSigmas)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 72 -- the coarsest rung's excess over a*L is "
                     "%.6g, only %.2f standard errors, below the pre-registered %.1f. THE "
                     "RATIOS BELOW WOULD BE MEASURING NOISE: an error at the sampling floor has "
                     "no order, and a convergence check run on one reports whatever the noise "
                     "did. Either the environment strategy no longer samples texel centres -- in "
                     "which case this whole ladder is obsolete and should be deleted rather than "
                     "loosened -- or kSeeds is too low to see a %ux%u map's quadrature error\n",
                     excess[0], coarseSigmas, kMinCoarseSigmas, kEnvW, kEnvHeights[0]);
        return false;
    }

    double ratios[2] = {0.0, 0.0};
    for (std::size_t i = 0; i < 2; ++i) {
        if (!(excess[i + 1] > 0.0)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 72 -- the excess over a*L at env height %u "
                         "is %.6g, which is not positive. The midpoint quadrature over the "
                         "environment map OVERESTIMATES this integrand (the derivation at "
                         "kEnvHeights gives +Delta^2/6), so a non-positive excess means the bias "
                         "being measured is not the one the ladder is about\n",
                         kEnvHeights[i + 1], excess[i + 1]);
            return false;
        }
        ratios[i] = excess[i] / excess[i + 1];
    }
    // READ FROM THE FINEST PAIR, as check 70 does: the coarse end carries an
    // O(Delta^4) term that pulls the ratio away from 4 without saying anything
    // about the order.
    const double orderRatio = ratios[1];
    if (!(std::fabs(orderRatio - kPredictedRatio) <= kRatioTol)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 72 -- THE ENVIRONMENT BIAS IS NOT SECOND "
                     "ORDER.\n"
                     "  excess over a*L: %.6g at H=%u, %.6g at H=%u, %.6g at H=%u\n"
                     "  ratios: %.4g (coarse pair), %.4g (fine pair, the one judged)\n"
                     "  predicted %.4g +/- %.4g\n"
                     "  The prediction is DERIVED: sampleEnvMap returns texel centres, so the "
                     "environment strategy is a composite midpoint rule in the polar angle, "
                     "whose error is Delta^2/6 with Delta = pi/H. Halving Delta must quarter it.\n"
                     "  A ratio near 2 means the error is FIRST order, which a midpoint rule is "
                     "not -- suspect the binning in envTexelPdfUV rather than the sampling. A "
                     "ratio near 1 means the bias does not depend on the environment resolution "
                     "at all, so it is NOT this quadrature, and the absolute comparison below "
                     "has a different cause entirely.\n",
                     excess[0], kEnvHeights[0], excess[1], kEnvHeights[1], excess[2],
                     kEnvHeights[2], ratios[0], ratios[1], kPredictedRatio, kRatioTol);
        return false;
    }

    // --- NON-VACUITY 2: the gate's resolution, pre-registered. Refuse a
    // verdict the sampling could not have resolved.
    const double filmResolution = (kSigmas * fine.filmStdErr) / expectedFilm;
    const double gradResolution = (kSigmas * fine.gradStdErr) / expectedGrad;
    const double worstResolution = std::fmax(filmResolution, gradResolution);
    if (!(worstResolution <= kMaxResolution)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 72 REFUSES TO CLAIM A VERDICT: the widest "
                     "tolerance this sampling earns is %.4g of the answer, above the "
                     "pre-registered %.4g. film %.9g +/- %.4g, gradient %.9g +/- %.4g, over %u "
                     "samples. A pass at this resolution would be compatible with a scale error "
                     "of that size, which is exactly the kind of error this gate exists to "
                     "catch. RAISE kSeeds, not the tolerance\n",
                     worstResolution, kMaxResolution, fine.filmMean, kSigmas * fine.filmStdErr,
                     fine.gradMean, kSigmas * fine.gradStdErr, samples);
        return false;
    }

    // --- THE GATE, both quantities against the closed form, at the fine
    // environment.
    struct Leg {
        const char* name;
        double measured;
        double expected;
        double stdErr;
        const char* meaning;
    };
    const Leg legs[2] = {
        {"mean pixel", fine.filmMean, expectedFilm, fine.filmStdErr,
         "the FORWARD scale: the Lambertian 1/pi, the reconstructed environment radiance, and "
         "the camera's sample weight, multiplied together"},
        {"d(mean pixel)/d(albedo)", fine.gradMean, expectedGrad, fine.gradStdErr,
         "the same scale carried through the adjoint, which is the number an optimiser steps on"},
    };
    for (const Leg& leg : legs) {
        const double absError = std::fabs(leg.measured - leg.expected);
        const double allowed = kSigmas * leg.stdErr;
        if (!(absError <= allowed)) {
            std::fprintf(
                stderr,
                "[diff_gpu_probe] FAIL: check 72 -- %s IS OFF BY A SCALE FACTOR.\n"
                "  measured    %.12g +/- %.6g (1 standard error), over %u samples\n"
                "  closed form %.12g\n"
                "  ratio       %.9g\n"
                "  |difference| %.6g against an allowance of %.6g (%.1f standard errors)\n"
                "  What this number is: %s.\n"
                "  An ideal Lambertian of albedo %.6g under a CONSTANT environment of radiance "
                "%.6g reflects exactly a*L, so this comparison has no quadrature error in it "
                "and the discrepancy is a scale. tests/diff/tools/mitsuba_gate.py confirms the "
                "closed form against Mitsuba 3 independently (leg 1), so a failure here is this "
                "renderer's and not the derivation's.\n"
                "  THE ENVIRONMENT'S OWN QUADRATURE IS ALREADY ACCOUNTED FOR: the ladder above "
                "passed, so the map's midpoint bias is second order in pi/H, and at H = %u that "
                "is 2.5e-05 -- far under this allowance. Whatever this is, it is not that.\n"
                "  NO OTHER CHECK IN THIS SUITE CAN SEE THIS. Checks 33-34 compare the film "
                "against a reference integrator that shares the derivation; check 37 compares "
                "the gradient against a finite difference of this same film. A uniform "
                "rescaling moves both sides of both and leaves them green. Look at the ratio "
                "above first: pi, 1/pi, 2, 1/2 and 4*pi each name a different suspect.\n",
                leg.name, leg.measured, leg.stdErr, samples, leg.expected,
                leg.measured / leg.expected, absError, allowed, kSigmas, leg.meaning,
                static_cast<double>(kAlbedo), static_cast<double>(kEnvRadiance), kEnvHeightFine);
            return false;
        }
    }

    std::printf(
        "[diff_gpu_probe] check 72 OK: at env %ux%u the mean pixel is %.9g against a*L = %.9g and "
        "d/d(albedo) is %.9g against L = %.9g, over %u samples, a %.1f-sigma gate resolving "
        "%.3g of the answer (plate margin %.4g). THE ENVIRONMENT MAP'S OWN MIDPOINT QUADRATURE, "
        "measured: excess over a*L %.4g at H=%u, %.4g at H=%u, %.4g at H=%u -- ratios %.4g and "
        "%.4g against a DERIVED %.4g, second order in pi/H exactly as texel-centre sampling "
        "predicts.\n",
        kEnvW, kEnvHeightFine, fine.filmMean, expectedFilm, fine.gradMean, expectedGrad, samples,
        kSigmas, worstResolution, margin, excess[0], kEnvHeights[0], excess[1], kEnvHeights[1],
        excess[2], kEnvHeights[2], ratios[0], ratios[1], kPredictedRatio);
    return true;
}

}  // namespace ohao::diff::probe
