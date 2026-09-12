// Spec 10.3, check 75. The argument for holding the sampling distribution
// fixed -- which is why binding 14 exists at all -- is in the header.
#include "probe/checks_env_recovery.hpp"

#include "probe/scene.hpp"

#include "diff/grad/gradient_arena.hpp"
#include "diff/param/param_registry.hpp"
#include "diff/wavefront/gradient_stages.hpp"
#include "diff/wavefront/wavefront_buffers.hpp"
#include "render/rt/env_cdf.hpp"

#include <algorithm>
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
// A SMALL environment, so every texel that is reachable gets enough samples
// to carry a usable gradient at one sample per pixel. 8x4 is 32 parameters,
// which is twice check 55's 17 and is the largest this harness's sampling
// supports without the run becoming a study of Monte Carlo noise.
constexpr std::uint32_t kEnvW = 8u, kEnvH = 4u;
constexpr std::uint32_t kEnvTexels = kEnvW * kEnvH;
constexpr std::uint32_t kBounces = 2u;
constexpr std::uint32_t kSeed = 20260915u;
constexpr float kMatAlbedo = 0.6f;

// THE PRE-REGISTERED CRITERION, fixed before the first run.
constexpr float kThetaStar = 1.5f;   // the environment to be recovered
constexpr float kTheta0 = 0.5f;      // where the optimiser starts, 1.0 away
constexpr std::uint32_t kIterations = 200u;
constexpr float kAlpha = 0.05f;
constexpr double kRecoveredWithin = 0.15;  // 3 * kAlpha, for kAlpha's reason
// An element counts as CARRYING GRADIENT if |dL/dtheta_i| at theta_0 exceeds
// this fraction of the largest. Below it Adam cannot move the element far in
// kIterations steps, and requiring recovery would be requiring the
// impossible.
constexpr double kCarriesGradient = 0.05;

/// The environment the CDF is built from: UNIFORM, and deliberately not the
/// one being recovered.
///
/// The CDF is a SAMPLING distribution here and nothing else. Building it from
/// theta* would let the sampler know the answer; building it from theta and
/// rebuilding each iteration would make the density depend on the parameter,
/// which is the dependence this adjoint does not carry. Uniform is the one
/// choice that is neither.
void buildUniformEnvRgba(std::vector<float>& outRgba) {
    outRgba.assign(static_cast<std::size_t>(kEnvTexels) * 4u, 1.0f);
    for (std::size_t k = 3u; k < outRgba.size(); k += 4u) outRgba[k] = 1.0f;
}

}  // namespace

bool checkEnvRecovery(ohao::diff::GpuProbeContext& ctx) {
    ohao::diff::ParamRegistry reg;
    const auto regEnv = reg.registerScalarBlock("env_radiance", kEnvTexels);
    if (!regEnv.ok) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 75 registry setup: %s\n",
                     regEnv.error.c_str());
        return false;
    }
    const ohao::diff::DiffParam* envParam = reg.find("env_radiance");
    if (envParam == nullptr) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 75 registered param not found\n");
        return false;
    }
    ohao::diff::GradientArena arena;
    if (!arena.build(ctx.allocator(), reg.layout())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 75 gradient arena build\n");
        return false;
    }
    const std::uint32_t kArenaFloats =
        static_cast<std::uint32_t>(reg.layout().totalBytes() / sizeof(float));
    const std::uint32_t kOffset = static_cast<std::uint32_t>(
        reg.layout().block(envParam->gradBlock).offsetBytes / sizeof(float));

    std::vector<float> envRgba;
    buildUniformEnvRgba(envRgba);
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    buildParityScene(positions, indices);
    const ohao::diff::WavefrontGenerateCamera camera = parityCamera();

    ohao::EnvCDF cdf;
    cdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
    ohao::diff::WavefrontBuffers wf;
    if (!cdf.valid() || !wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH) ||
        !wf.uploadEnvironment(cdf.marginalSpan(), cdf.conditionalSpan(), cdf.integral())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 75 scene setup\n");
        wf.destroy(ctx.allocator());
        arena.destroy(ctx.allocator());
        return false;
    }
    const ohao::diff::WavefrontScatterMaterial kMaterial{1.0f, 0.0f, 0.0f};

    ohao::diff::GpuProbeContext::OwnedScene scene;
    if (!ctx.buildOwnedScene(std::span<const float>(positions),
                             std::span<const std::uint32_t>(indices), scene)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 75 buildOwnedScene\n");
        wf.destroy(ctx.allocator());
        arena.destroy(ctx.allocator());
        return false;
    }
    const ohao::diff::WavefrontGradientOptions::PrebuiltScene sceneHandle = scene.handle();
    ohao::diff::GradientStages stages;
    auto cleanup = [&]() {
        ctx.destroyOwnedStages(stages);
        ctx.destroyOwnedScene(scene);
        wf.destroy(ctx.allocator());
        arena.destroy(ctx.allocator());
    };

    // SAME SEED EVERY RENDER: under common random numbers the loss at theta*
    // is exactly zero, so the minimiser is theta* itself and a failure to
    // reach it is the loop rather than the estimator.
    auto render = [&](const std::vector<float>& env, const std::vector<float>& seed,
                      std::vector<float>& outFilm, std::vector<float>& outGrad) -> bool {
        ohao::diff::WavefrontGradientOptions options;
        options.diffParam = 5u;  // DIFF_PARAM_ENV_IMAGE
        options.envImage = env;
        options.adjointSeed = seed;
        options.scene = &sceneHandle;
        options.stages = &stages;
        if (!ctx.runWavefrontGradientProbe(wf, kW, kH, kBounces, camera,
                                           std::span<const float>(positions),
                                           std::span<const std::uint32_t>(indices), kMatAlbedo,
                                           kMaterial, kSeed, arena, kArenaFloats, kOffset,
                                           outFilm, options)) {
            return false;
        }
        outGrad = arena.readback(ctx.allocator(), envParam->gradBlock);
        return outGrad.size() >= kEnvTexels;
    };

    const std::vector<float> envStar(kEnvTexels, kThetaStar);
    std::vector<float> target, ignoredGrad;
    if (!render(envStar, {}, target, ignoredGrad) || target.empty()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 75 target render\n");
        cleanup();
        return false;
    }

    std::vector<float> theta(kEnvTexels, kTheta0);
    std::vector<float> adamState(kEnvTexels * 2u, 0.0f);
    ohao::diff::GpuProbeContext::AdamOptions adam;
    adam.alpha = kAlpha;
    std::vector<float> gradAtStart;
    double firstLoss = 0.0, lastLoss = 0.0;

    for (std::uint32_t it = 1u; it <= kIterations; ++it) {
        std::vector<float> film, grad;
        if (!render(theta, {}, film, grad)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 75 forward at iter %u\n", it);
            cleanup();
            return false;
        }
        std::vector<float> seed;
        double loss = 0.0;
        if (!ctx.runLossL2Probe(film, target, seed, loss)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 75 loss at iter %u\n", it);
            cleanup();
            return false;
        }
        if (it == 1u) firstLoss = loss;
        lastLoss = loss;
        std::vector<float> ignoredFilm;
        if (!render(theta, seed, ignoredFilm, grad)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 75 backward at iter %u\n", it);
            cleanup();
            return false;
        }
        if (it == 1u) gradAtStart = grad;
        if (!ctx.runAdamProbe(theta, grad, adamState, adam, it)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 75 Adam at iter %u\n", it);
            cleanup();
            return false;
        }
    }
    cleanup();

    // --- NON-VACUITY 1: something carried gradient at all.
    double maxGrad = 0.0;
    for (std::uint32_t k = 0; k < kEnvTexels; ++k) {
        maxGrad = std::max(maxGrad, std::fabs(static_cast<double>(gradAtStart[k])));
    }
    if (!(maxGrad > 0.0)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 75 -- every one of the %u environment texels "
                     "has a zero gradient at theta_0, so nothing could have been recovered and "
                     "every assertion below would hold vacuously. The likeliest cause is that "
                     "the scatter never ran: DIFF_PARAM_ENV_IMAGE reaches diffScatterEnvImage "
                     "through its OWN branch in wf_scatter_replay.comp's hook, not through the "
                     "scalar atomicAdd every other parameter uses\n",
                     kEnvTexels);
        return false;
    }

    // --- THE PARTITION, read off the FIRST iteration's gradient.
    std::size_t constrained = 0, unconstrained = 0, movedThatShouldNot = 0;
    double worstErr = 0.0;
    std::size_t worstIndex = 0;
    for (std::uint32_t k = 0; k < kEnvTexels; ++k) {
        const double g = std::fabs(static_cast<double>(gradAtStart[k]));
        const double err = std::fabs(static_cast<double>(theta[k]) - static_cast<double>(kThetaStar));
        if (g >= kCarriesGradient * maxGrad) {
            ++constrained;
            if (err > worstErr) {
                worstErr = err;
                worstIndex = k;
            }
        } else {
            ++unconstrained;
            // EXACTLY unmoved, compared as floats. Adam at g = 0 steps
            // alpha*0/(0+eps), which is zero and not merely small.
            if (gradAtStart[k] == 0.0f && theta[k] != kTheta0) ++movedThatShouldNot;
        }
    }

    if (constrained == 0) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 75 -- no texel carries at least %.3g of the "
                     "largest gradient, so the recovery assertion has an empty subject\n",
                     kCarriesGradient);
        return false;
    }
    if (movedThatShouldNot != 0) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 75 -- %zu texels with an EXACTLY zero gradient "
                     "at theta_0 nonetheless moved off theta_0 = %.6g. Adam at g = 0 has m = v = "
                     "0 and a step of alpha*0/(0+eps), which is exactly zero, so a texel that "
                     "drifted is the optimiser writing a parameter this scene does not depend "
                     "on\n",
                     movedThatShouldNot, static_cast<double>(kTheta0));
        return false;
    }
    if (!(lastLoss < firstLoss)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 75 -- the loss went %.9g -> %.9g. It must "
                     "fall: under common random numbers theta* is an exact minimiser at zero\n",
                     firstLoss, lastLoss);
        return false;
    }
    if (!(worstErr <= kRecoveredWithin)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 75 -- THE ENVIRONMENT WAS NOT RECOVERED. The "
                     "worst constrained texel is %zu, %.9g from theta* = %.6g, against a "
                     "PRE-REGISTERED %.6g (3 * alpha). %zu of %u texels carry gradient; loss "
                     "%.9g -> %.9g over %u iterations from theta_0 = %.6g.\n"
                     "  The adjoint credits the texel each STRATEGY sampled, and the two sample "
                     "different directions -- so a systematic miss on half the constrained texels "
                     "points at one of the two scatters in diffScatterEnvImage rather than at the "
                     "coefficient. A miss on ALL of them by a common factor points at the "
                     "coefficient: d(term)/dL is fCosine*V/pOwn, and for strategy B the density "
                     "cancels, so bsdfRadianceCoeff is weight*visBsdf with no division.\n",
                     worstIndex, worstErr, static_cast<double>(kThetaStar), kRecoveredWithin,
                     constrained, kEnvTexels, firstLoss, lastLoss, kIterations,
                     static_cast<double>(kTheta0));
        return false;
    }

    std::printf(
        "[diff_gpu_probe] check 75 OK -- GATE 5 FOR THE ENVIRONMENT (spec 10.3). Its radiance "
        "recovered texel by texel: %zu of %u texels carry gradient and the worst finished %.6g "
        "from theta* = %.3g, against a PRE-REGISTERED %.3g (3 * alpha), having started %.3g away. "
        "Loss %.9g -> %.9g over %u Adam iterations at alpha %.3g. THIS IS THE FIRST PARAMETER "
        "THAT IS NOT A PROPERTY OF A SURFACE: the environment is read along a DIRECTION, by two "
        "strategies that sample two different ones, so the adjoint scatters TWICE per vertex at "
        "generally different texels -- crediting both to one would put the BSDF strategy's "
        "contribution in the light sampler's bin. THE SAMPLING DISTRIBUTION IS HELD FIXED, built "
        "once from a UNIFORM environment and never rebuilt: spec 6.3 differentiates at fixed "
        "directions, so the density must not depend on the parameter, and that separation is the "
        "whole reason binding 14 exists. The sampler is therefore UNINFORMED about the "
        "environment it is recovering, which costs variance and costs nothing in correctness. "
        "AND THE UNCONSTRAINED TEXELS ARE REQUIRED TO BE EXACTLY UNMOVED: %zu of them, compared "
        "as floats -- a floor-only scene sends no ray below the horizon, so those texels are "
        "unreachable by construction, and Adam at g = 0 steps exactly zero.\n",
        constrained, kEnvTexels, worstErr, static_cast<double>(kThetaStar), kRecoveredWithin,
        std::fabs(static_cast<double>(kThetaStar - kTheta0)), firstLoss, lastLoss, kIterations,
        static_cast<double>(kAlpha), unconstrained);
    return true;
}

}  // namespace ohao::diff::probe
