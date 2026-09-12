// Check 74. The argument for the binding, and for why the control is the
// load-bearing half, is in the header.
#include "probe/checks_env_image.hpp"

#include "probe/scene.hpp"

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

constexpr std::uint32_t kW = 64u, kH = 8u;
constexpr std::uint32_t kCapacity = kW * kH;
constexpr std::uint32_t kEnvW = 64u, kEnvH = 32u;
// THREE BOUNCES, so the environment is looked up at several vertices by both
// strategies. One bounce would exercise a single lookup per path and a
// binning error that only bit at grazing angles could hide.
constexpr std::uint32_t kBounces = 3u;
constexpr std::uint32_t kSeed = 20260914u;
constexpr float kAlbedo = 0.5f;

// THE PRE-REGISTERED CRITERION. Both routes compute the same radiance, but
// not by the same arithmetic: one reads a stored float, the other multiplies
// a density by an integral and a constant. So the films agree to float
// rounding over a product of about six factors, not bit-exactly.
constexpr double kAgreeRelTol = 1e-5;
// The control's films must differ by MUCH more than that. Doubling every
// texel's radiance doubles the direct-lighting estimate, so the expected
// separation is order 1 -- this bound is deliberately far below that, because
// what it is testing is "the image is read", not "by how much".
constexpr double kControlMinRel = 0.1;

double filmRelativeDifference(const std::vector<float>& a, const std::vector<float>& b) {
    double worst = 0.0;
    double scale = 0.0;
    for (float v : a) scale = std::fmax(scale, std::fabs(static_cast<double>(v)));
    if (!(scale > 0.0)) return 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::fmax(worst, std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
    }
    return worst / scale;
}

}  // namespace

bool checkEnvImageRadiance(ohao::diff::GpuProbeContext& ctx) {
    ohao::diff::DiffRenderer renderer;
    if (!renderer.init(ctx.device(), ctx.physicalDevice())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 74 -- init\n");
        return false;
    }
    const ohao::diff::RegisterResult reg = renderer.registerScalarBlock("albedo", 1u);
    if (!reg.ok || !renderer.build(ctx.allocator())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 74 -- register/build: %s\n",
                     reg.error.c_str());
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }
    const ohao::diff::DiffParam* albedoParam = renderer.registry().get(reg.id);
    const std::uint32_t kArenaFloats =
        static_cast<std::uint32_t>(renderer.registry().layout().totalBytes() / sizeof(float));
    const std::uint32_t kOffset = static_cast<std::uint32_t>(
        renderer.registry().layout().block(albedoParam->gradBlock).offsetBytes / sizeof(float));

    // ONE environment, built once, so the CDF and the image cannot describe
    // different things by accident -- buildParityEnvironment produces the
    // RGBA the CDF consumes and the grey luminance in the SAME loop.
    std::vector<float> envRgba;
    std::vector<double> envLum;
    buildParityEnvironment(kEnvW, kEnvH, envRgba, envLum);
    std::vector<float> envImage(envLum.size(), 0.0f);
    double minL = 1e30, maxL = -1e30;
    for (std::size_t i = 0; i < envLum.size(); ++i) {
        envImage[i] = static_cast<float>(envLum[i]);
        minL = std::fmin(minL, envLum[i]);
        maxL = std::fmax(maxL, envLum[i]);
    }
    // NON-VACUITY: the map must VARY, or every binning agrees and a lookup
    // that read the wrong texel every time would pass.
    if (!(maxL > 2.0 * minL) || !(minL > 0.0)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 74 -- the environment runs [%.6g, %.6g], a "
                     "contrast of %.3gx. A map this flat makes the texel binning untestable: "
                     "every lookup agrees with every other, so a wrong texel index would pass\n",
                     minL, maxL, (minL > 0.0) ? maxL / minL : 0.0);
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }

    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    buildParityScene(positions, indices);
    const ohao::diff::WavefrontGenerateCamera camera = parityCamera();

    ohao::EnvCDF cdf;
    cdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
    ohao::diff::WavefrontBuffers wf;
    if (!cdf.valid() || !wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH) ||
        !wf.uploadEnvironment(cdf.marginalSpan(), cdf.conditionalSpan(), cdf.integral())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 74 -- scene setup\n");
        wf.destroy(ctx.allocator());
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }
    const ohao::diff::WavefrontScatterMaterial kMaterial{1.0f, 0.0f, 0.0f};

    ohao::diff::GpuProbeContext::OwnedScene scene;
    if (!ctx.buildOwnedScene(std::span<const float>(positions),
                             std::span<const std::uint32_t>(indices), scene)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 74 -- buildOwnedScene\n");
        wf.destroy(ctx.allocator());
        (void)renderer.shutdown(&ctx.allocator());
        return false;
    }
    const ohao::diff::WavefrontGradientOptions::PrebuiltScene sceneHandle = scene.handle();
    ohao::diff::GradientStages stages;

    // SAME SEED for all three renders, so the paths are identical and the
    // films differ only where the radiance does. Without common random
    // numbers the comparison below would be two Monte Carlo estimates and
    // the tolerance would have to absorb sampling noise instead of float
    // rounding.
    auto render = [&](const std::vector<float>& image, std::vector<float>& outFilm) -> bool {
        ohao::diff::WavefrontGradientOptions options;
        options.diffParam = 0u;
        options.scene = &sceneHandle;
        options.stages = &stages;
        options.envImage = image;
        return ctx.runWavefrontGradientProbe(
            wf, kW, kH, kBounces, camera, std::span<const float>(positions),
            std::span<const std::uint32_t>(indices), kAlbedo, kMaterial, kSeed, renderer.arena(),
            kArenaFloats, kOffset, outFilm, options);
    };

    std::vector<float> filmInverted, filmImage, filmWrong;
    std::vector<float> wrongImage(envImage.size(), 0.0f);
    for (std::size_t i = 0; i < envImage.size(); ++i) wrongImage[i] = envImage[i] * 2.0f;

    const bool ran = render({}, filmInverted) && render(envImage, filmImage) &&
                     render(wrongImage, filmWrong);
    ctx.destroyOwnedStages(stages);
    ctx.destroyOwnedScene(scene);
    wf.destroy(ctx.allocator());
    (void)renderer.shutdown(&ctx.allocator());

    if (!ran || filmInverted.empty() || filmInverted.size() != filmImage.size() ||
        filmInverted.size() != filmWrong.size()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 74 -- one of the three renders\n");
        return false;
    }
    double filmMax = 0.0;
    for (float v : filmInverted) {
        if (!std::isfinite(v) || v < 0.0f) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 74 -- the fallback film holds %.9g, not a "
                         "finite non-negative radiance\n",
                         static_cast<double>(v));
            return false;
        }
        filmMax = std::fmax(filmMax, static_cast<double>(v));
    }
    // There must BE a film. Comparing two black images proves nothing.
    if (!(filmMax > 0.0)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 74 -- the film is entirely zero, so "
                             "neither radiance route was exercised\n");
        return false;
    }

    // --- THE CONTROL FIRST, because it is what makes the agreement mean
    // anything. A doubled environment must produce a different film; if it
    // does not, the image is not being read and the agreement below is two
    // fallback renders agreeing with each other.
    const double controlRel = filmRelativeDifference(filmInverted, filmWrong);
    if (!(controlRel >= kControlMinRel)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 74 -- THE ENVIRONMENT IMAGE IS NOT BEING READ. "
                     "A render with every texel DOUBLED differs from the fallback by only %.6g of "
                     "the film's peak, under the pre-registered %.6g.\n"
                     "  Both renders must therefore be taking the same path, which means "
                     "diffEnvImageRadiance returned its -1 sentinel even with an image bound. "
                     "Suspect, in order: pc.envImageTexels arriving 0 (the push field is LAST in "
                     "ScatterPush and the canonical field list in ties.cpp must name it); the "
                     "buffer bound at binding 14 being the placeholder; or the idx >= "
                     "envImageTexels bounds guard rejecting every lookup.\n"
                     "  WITHOUT THIS CONTROL the agreement check below would PASS in exactly that "
                     "state, because two fallback renders agree perfectly.\n",
                     controlRel, kControlMinRel);
        return false;
    }

    // --- THE AGREEMENT. Same environment, two routes to its radiance.
    const double agreeRel = filmRelativeDifference(filmInverted, filmImage);
    if (!(agreeRel <= kAgreeRelTol)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 74 -- THE TWO RADIANCE ROUTES DISAGREE.\n"
                     "  worst film difference %.6g of the peak, against a pre-registered %.6g\n"
                     "  (the doubled-environment control differs by %.6g, so the image IS being "
                     "read and this is a real disagreement rather than a dead binding)\n"
                     "  These must agree: inverting the CDF's density recovers L exactly for a "
                     "grey environment -- check 31 asserts that texel by texel -- so reading the "
                     "image is a different route to the same number. A disagreement localises to "
                     "the BINNING: diffEnvImageRadiance must floor-and-clamp acos(dir.y) and "
                     "atan2(dir.z, dir.x) into the same texel env_sampling.glsl's envTexelPdfUV "
                     "does, because the density the estimator divides by is that texel's. A "
                     "radiance from a neighbouring texel is a mismatch no check of either half "
                     "alone can see.\n",
                     agreeRel, kAgreeRelTol, controlRel);
        return false;
    }

    std::printf(
        "[diff_gpu_probe] check 74 OK -- THE ENVIRONMENT'S RADIANCE, FROM AN IMAGE INSTEAD OF "
        "FROM ITS OWN SAMPLING DENSITY. Binding 14 bound, the same environment rendered both "
        "ways at one seed over %u bounces: worst film difference %.3g of the peak, inside a "
        "pre-registered %.3g, which is float rounding over a product of about six factors rather "
        "than bit-exactness. WHY THE BINDING EXISTS is not the chroma that nee.glsl names -- it "
        "is that inverting the CDF makes the radiance and the SAMPLING DISTRIBUTION one array, "
        "and spec 6.3 differentiates at FIXED directions, so an environment PARAMETER needs the "
        "radiance to move while the density stays put. AND THE CONTROL IS THE LOAD-BEARING HALF: "
        "with every texel DOUBLED the film moves %.4g, so the image is genuinely read -- without "
        "that, a dead binding would make the agreement above perfect and meaningless, which is "
        "the defect class that has bitten this subsystem three times. The environment varies "
        "%.3gx across its texels, asserted before the renders, because on a flat map every "
        "binning agrees and a lookup that read the wrong texel every time would pass.\n",
        kBounces, agreeRel, kAgreeRelTol, controlRel, maxL / minL);
    return true;
}

}  // namespace ohao::diff::probe
