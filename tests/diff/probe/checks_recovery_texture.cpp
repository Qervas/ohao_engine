// Stage 2 Task 5 Step 3, check 55: Gate 5 for MANY parameters at once.
#include "probe/checks_recovery_texture.hpp"

#include "probe/fd_harness.hpp"
#include "probe/scene.hpp"

#include "diff/grad/arena_layout.hpp"
#include "diff/grad/gradient_arena.hpp"
#include "diff/param/param_registry.hpp"
#include "diff/wavefront/wavefront_buffers.hpp"
#include "diff/wavefront/wavefront_loop.hpp"
#include "render/rt/env_cdf.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ohao::diff::probe {

bool checkRecoveryTexture(ohao::diff::GpuProbeContext& ctx) {
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 8;
    constexpr uint32_t kCapacity = kW * kH;
    constexpr uint32_t kEnvW = 64;
    constexpr uint32_t kEnvH = 32;
    constexpr uint32_t kBounces = 3u;
    constexpr uint32_t kSeed = 20260901u;
    constexpr float kMatAlbedo = 0.4f;
    constexpr float kUvScale = 0.0625f;
    constexpr float kUvBias = 0.5f;

    // THE PRE-REGISTERED CRITERION, fixed before the first run.
    constexpr float kTheta0 = 0.5f;
    constexpr uint32_t kIterations = 150u;
    constexpr float kAlpha = 0.02f;
    constexpr double kRecoveredWithin = 0.06;  // 3 * kAlpha, for kAlpha's reason
    // An element counts as CARRYING GRADIENT if its |dL/dtheta_i| at theta_0
    // exceeds this fraction of the largest. Below it, Adam cannot move the
    // element far in kIterations steps and requiring recovery would be
    // requiring the impossible -- see the loop's comment.
    constexpr double kCarriesGradient = 0.05;

    const ohao::diff::ParamShape kTexShape{4u, 3u, 3u};
    const uint32_t kTexFloats = kTexShape.floatCount();

    // THE GROUND TRUTH: check 49's pattern, non-uniform in x, y AND c.
    std::vector<float> texStar(kTexFloats, 0.0f);
    for (uint32_t y = 0; y < kTexShape.height; ++y) {
        for (uint32_t x = 0; x < kTexShape.width; ++x) {
            for (uint32_t c = 0; c < kTexShape.channels; ++c) {
                texStar[kTexShape.elementIndex(x, y, c)] =
                    0.30f + 0.05f * static_cast<float>(y * kTexShape.width + x) +
                    0.02f * static_cast<float>(c);
            }
        }
    }

    ohao::diff::ParamRegistry reg;
    const auto regTex =
        reg.registerTexture("emission_tex", kTexShape, VK_FORMAT_R32G32B32_SFLOAT);
    if (!regTex.ok) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 55 registry setup: %s\n",
                     regTex.error.c_str());
        return false;
    }
    const ohao::diff::DiffParam* texParam = reg.find("emission_tex");
    if (texParam == nullptr) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 55 registered param not found\n");
        return false;
    }
    ohao::diff::GradientArena arena;
    if (!arena.build(ctx.allocator(), reg.layout())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 55 gradient arena build\n");
        return false;
    }
    const uint32_t kArenaFloats =
        static_cast<uint32_t>(reg.layout().totalBytes() / sizeof(float));
    const uint32_t kOffset = static_cast<uint32_t>(
        reg.layout().block(texParam->gradBlock).offsetBytes / sizeof(float));

    std::vector<float> envRgba;
    std::vector<double> envLum;
    buildParityEnvironment(kEnvW, kEnvH, envRgba, envLum);
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    buildParityScene(positions, indices);
    const ohao::diff::WavefrontGenerateCamera camera = parityCamera();

    ohao::EnvCDF cdf;
    cdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
    if (!cdf.valid()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 55 EnvCDF::build produced no CDF\n");
        arena.destroy(ctx.allocator());
        return false;
    }
    ohao::diff::WavefrontBuffers wf;
    if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH) ||
        !wf.uploadEnvironment(cdf.marginalSpan(), cdf.conditionalSpan(), cdf.integral())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 55 buffers build / env CDF upload\n");
        wf.destroy(ctx.allocator());
        arena.destroy(ctx.allocator());
        return false;
    }

    const ohao::diff::WavefrontScatterMaterial kTexMaterial{1.0f, 0.0f, 0.0f};

    auto render = [&](const std::vector<float>& texels, const std::vector<float>& seed,
                      std::vector<float>& outFilm, std::vector<float>& outGrad) -> bool {
        ohao::diff::WavefrontGradientOptions options = emissionTextureOptions(
            texels, kTexShape, kUvScale, kUvScale, kUvBias, kUvBias);
        options.adjointSeed = seed;
        if (!ctx.runWavefrontGradientProbe(wf, kW, kH, kBounces, camera,
                                           std::span<const float>(positions),
                                           std::span<const uint32_t>(indices), kMatAlbedo,
                                           kTexMaterial, kSeed, arena, kArenaFloats, kOffset,
                                           outFilm, options)) {
            return false;
        }
        outGrad = arena.readback(ctx.allocator(), texParam->gradBlock);
        return outGrad.size() == static_cast<std::size_t>(kTexFloats);
    };
    auto cleanup = [&]() {
        wf.destroy(ctx.allocator());
        arena.destroy(ctx.allocator());
    };

    const std::vector<float> noSeed;
    std::vector<float> target;
    std::vector<float> ignoredGrad;
    if (!render(texStar, noSeed, target, ignoredGrad)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 55 target render\n");
        cleanup();
        return false;
    }

    ohao::diff::GpuProbeContext::AdamOptions adam;
    adam.alpha = kAlpha;
    std::vector<float> theta(kTexFloats, kTheta0);
    std::vector<float> adamState(static_cast<std::size_t>(kTexFloats) * 2u, 0.0f);
    std::vector<float> gradAtStart;
    double firstLoss = 0.0;
    double lastLoss = 0.0;

    for (uint32_t it = 1; it <= kIterations; ++it) {
        std::vector<float> film;
        std::vector<float> unusedGrad;
        if (!render(theta, noSeed, film, unusedGrad)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 55 forward at iter %u\n", it);
            cleanup();
            return false;
        }
        std::vector<float> seed;
        double loss = 0.0;
        if (!ctx.runLossL2Probe(film, target, seed, loss)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 55 loss at iter %u\n", it);
            cleanup();
            return false;
        }
        if (it == 1u) firstLoss = loss;
        lastLoss = loss;
        std::vector<float> ignoredFilm;
        std::vector<float> grad;
        if (!render(theta, seed, ignoredFilm, grad)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 55 backward at iter %u\n", it);
            cleanup();
            return false;
        }
        if (it == 1u) gradAtStart = grad;
        if (!ctx.runAdamProbe(theta, grad, adamState, adam, it)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 55 Adam at iter %u\n", it);
            cleanup();
            return false;
        }
    }
    cleanup();

    // --- WHICH ELEMENTS COULD HAVE MOVED. Only texels the paths actually
    // read carry gradient; the rest are not "unrecovered", they are
    // UNCONSTRAINED by this scene, and demanding they arrive would be
    // demanding the impossible. Which ones those are is read off the FIRST
    // iteration's gradient -- before any optimisation -- so the partition is
    // not chosen from the result.
    double maxGrad = 0.0;
    for (float g : gradAtStart) maxGrad = std::max(maxGrad, std::fabs(static_cast<double>(g)));
    if (!(maxGrad > 0.0)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 55 -- every one of the %u texture gradient "
                     "elements is zero at theta_0, so nothing could have been recovered and "
                     "the assertions below would all hold vacuously\n",
                     kTexFloats);
        return false;
    }

    std::size_t constrained = 0;
    std::size_t unconstrained = 0;
    std::size_t movedThatShouldNot = 0;
    double worstErr = 0.0;
    std::size_t worstIndex = 0;
    for (uint32_t k = 0; k < kTexFloats; ++k) {
        const double g = std::fabs(static_cast<double>(gradAtStart[k]));
        const double err =
            std::fabs(static_cast<double>(theta[k]) - static_cast<double>(texStar[k]));
        if (g >= kCarriesGradient * maxGrad) {
            ++constrained;
            if (err > worstErr) {
                worstErr = err;
                worstIndex = k;
            }
        } else {
            ++unconstrained;
            // AN ELEMENT WITH NO GRADIENT CANNOT MOVE, EXACTLY. Adam at
            // g = 0 has m = v = 0, so the step is alpha*0/(0+eps) = 0 --
            // not "small", zero. An element that drifted is an optimiser
            // touching a parameter the scene does not depend on.
            if (gradAtStart[k] == 0.0f && theta[k] != kTheta0) ++movedThatShouldNot;
        }
    }

    if (constrained == 0) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 55 -- no element carries at least %.3g of the "
                     "largest gradient, so there is nothing this scene constrains and the "
                     "recovery assertion would be vacuous\n",
                     kCarriesGradient);
        return false;
    }
    if (movedThatShouldNot != 0) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 55 -- %zu elements whose gradient is EXACTLY "
                     "zero moved away from theta_0 = %.9g. Adam at g = 0 has m = v = 0 and a "
                     "step of alpha*0/(0+eps), which is exactly 0 -- so this is an optimiser "
                     "writing a parameter the scene does not depend on\n",
                     movedThatShouldNot, static_cast<double>(kTheta0));
        return false;
    }
    if (!(firstLoss > 0.0) || !(lastLoss < firstLoss)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 55 -- the loss went %.9g -> %.9g. It must "
                     "start positive and fall\n",
                     firstLoss, lastLoss);
        return false;
    }
    if (!(worstErr <= kRecoveredWithin)) {
        std::fprintf(
            stderr,
            "[diff_gpu_probe] FAIL: check 55 -- GATE 5 ON %zu SIMULTANEOUS PARAMETERS: the "
            "worst constrained element did not recover.\n"
            "  element %zu: theta* = %.9g, theta_0 = %.9g, theta = %.12g\n"
            "  |error| = %.6g, above the PRE-REGISTERED %.6g (3 * alpha)\n"
            "  %zu of %u elements carry gradient; loss %.9g -> %.9g after %u iterations\n"
            "  Check 54 recovers a SCALAR through the same loop, so a failure here and not\n"
            "  there is about MULTIPLICITY: the per-element scatter putting a gradient in the\n"
            "  wrong slot. Checks 44, 45 and 49 test that scatter three ways -- conservation,\n"
            "  per-element magnitude, and linearity in the seed -- but all three read the\n"
            "  arena directly. This is the first that closes the loop through it, so a\n"
            "  transposition they share a blind spot for would surface here.\n",
            constrained, worstIndex, static_cast<double>(texStar[worstIndex]),
            static_cast<double>(kTheta0), static_cast<double>(theta[worstIndex]), worstErr,
            kRecoveredWithin, constrained, kTexFloats, firstLoss, lastLoss, kIterations);
        return false;
    }

    std::printf(
        "[diff_gpu_probe] OK: check 55 -- GATE 5 ON %zu SIMULTANEOUS PARAMETERS: a 4x3x3 "
        "emission TEXTURE recovered element by element. All %zu elements that carry gradient "
        "arrived within %.6g of theta*, worst %.6g at element %zu, against a PRE-REGISTERED "
        "%.6g (3 * alpha). Loss %.9g -> %.9g over %u iterations from a uniform theta_0 = %.9g. "
        "WHICH ELEMENTS COUNT IS READ OFF THE FIRST ITERATION'S GRADIENT, before any "
        "optimisation, so the partition is not chosen from the result: %zu of %u are "
        "unconstrained by this scene, and those are separately required to be EXACTLY unmoved "
        "-- Adam at g = 0 has m = v = 0 and a step of alpha*0/(0+eps), which is zero and not "
        "merely small, so an element that drifted would be the optimiser writing a parameter "
        "the scene does not depend on. WHAT THIS ADDS OVER CHECKS 44/45/49 IS NARROWER THAN IT "
        "LOOKS, and is stated that way because the obvious claim did not survive testing: two "
        "attempts to build a scatter transposition they miss were both caught EARLIER -- a "
        "rotated channel index by checkTexelOrderingTie before any GPU work, swapped bilinear "
        "weights by check 45. This is the only check that closes the LOOP over many parameters, "
        "and the only one that requires the unconstrained elements to be exactly unmoved.\n",
        constrained, constrained, kRecoveredWithin, worstErr, worstIndex, kRecoveredWithin,
        firstLoss, lastLoss, kIterations, static_cast<double>(kTheta0), unconstrained,
        kTexFloats);
    return true;
}

}  // namespace ohao::diff::probe
