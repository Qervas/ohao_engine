// Stage 2 Task 5, check 54: Gate 5. See the header for the pre-registered
// criterion, which was written before this was run.
#include "probe/checks_recovery.hpp"

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

bool checkRecovery(ohao::diff::GpuProbeContext& ctx) {
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 8;
    constexpr uint32_t kCapacity = kW * kH;
    constexpr uint32_t kEnvW = 64;
    constexpr uint32_t kEnvH = 32;
    constexpr uint32_t kBounces = 3u;
    constexpr uint32_t kSeed = 20260828u;

    // THE PRE-REGISTERED CRITERION. See the header; these are the numbers
    // fixed before the first run.
    constexpr float kThetaStar = 0.6f;
    constexpr float kTheta0 = 0.3f;
    constexpr uint32_t kIterations = 100u;
    constexpr float kAlpha = 0.01f;
    constexpr double kRecoveredWithin = 0.03;  // 3 * kAlpha

    ohao::diff::ParamRegistry reg;
    const auto regAlbedo = reg.registerScalarBlock("albedo", 1);
    if (!regAlbedo.ok) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 54 registry setup: %s\n",
                     regAlbedo.error.c_str());
        return false;
    }
    const ohao::diff::DiffParam* albedoParam = reg.find("albedo");
    if (albedoParam == nullptr) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 54 registered param not found\n");
        return false;
    }
    ohao::diff::GradientArena arena;
    if (!arena.build(ctx.allocator(), reg.layout())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 54 gradient arena build\n");
        return false;
    }
    const uint32_t kArenaFloats =
        static_cast<uint32_t>(reg.layout().totalBytes() / sizeof(float));
    const uint32_t kOffset = static_cast<uint32_t>(
        reg.layout().block(albedoParam->gradBlock).offsetBytes / sizeof(float));

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
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 54 EnvCDF::build produced no CDF\n");
        arena.destroy(ctx.allocator());
        return false;
    }
    ohao::diff::WavefrontBuffers wf;
    if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH) ||
        !wf.uploadEnvironment(cdf.marginalSpan(), cdf.conditionalSpan(), cdf.integral())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 54 buffers build / env CDF upload\n");
        wf.destroy(ctx.allocator());
        arena.destroy(ctx.allocator());
        return false;
    }

    const ohao::diff::WavefrontScatterMaterial kMaterial{1.0f, 0.0f, 0.0f};

    /// One render at `albedo`. `seed` empty renders the film and ignores the
    /// gradient; `seed` non-empty is the backward pass whose gradient lands
    /// in the arena. BOTH use the same iteration seed, so under common random
    /// numbers they walk identical paths and the gradient belongs to the film
    /// the loss was computed from.
    auto render = [&](float albedo, const std::vector<float>& seed, std::vector<float>& outFilm,
                      double& outGrad) -> bool {
        ohao::diff::WavefrontGradientOptions options;
        options.diffParam = 0u;
        options.adjointSeed = seed;
        if (!ctx.runWavefrontGradientProbe(wf, kW, kH, kBounces, camera,
                                           std::span<const float>(positions),
                                           std::span<const uint32_t>(indices), albedo, kMaterial,
                                           kSeed, arena, kArenaFloats, kOffset, outFilm,
                                           options)) {
            return false;
        }
        const std::vector<float> block = arena.readback(ctx.allocator(), albedoParam->gradBlock);
        if (block.empty()) return false;
        outGrad = static_cast<double>(block[0]);
        return true;
    };

    auto cleanup = [&]() {
        wf.destroy(ctx.allocator());
        arena.destroy(ctx.allocator());
    };

    // --- THE TARGET, rendered at theta* with the SAME seed. Under CRN the
    // loss at theta* is exactly 0, so the minimiser is theta* itself.
    std::vector<float> target;
    double ignoredGrad = 0.0;
    const std::vector<float> noSeed;
    if (!render(kThetaStar, noSeed, target, ignoredGrad)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 54 target render\n");
        cleanup();
        return false;
    }

    // --- THE LOOP.
    ohao::diff::GpuProbeContext::AdamOptions adam;
    adam.alpha = kAlpha;
    std::vector<float> theta{kTheta0};
    std::vector<float> adamState(2u, 0.0f);
    double firstLoss = 0.0;
    double lastLoss = 0.0;
    double lastGrad = 0.0;

    for (uint32_t it = 1; it <= kIterations; ++it) {
        // 1. FORWARD: the film at the current theta.
        std::vector<float> film;
        double unused = 0.0;
        if (!render(theta[0], noSeed, film, unused)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 54 forward render at iter %u\n",
                         it);
            cleanup();
            return false;
        }
        // 2. LOSS: dL/d(film) and the scalar, against the target.
        std::vector<float> seed;
        double loss = 0.0;
        if (!ctx.runLossL2Probe(film, target, seed, loss)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 54 loss at iter %u\n", it);
            cleanup();
            return false;
        }
        if (it == 1u) firstLoss = loss;
        lastLoss = loss;
        // 3. BACKWARD: the same render again, now seeded with dL/d(film).
        std::vector<float> ignoredFilm;
        double grad = 0.0;
        if (!render(theta[0], seed, ignoredFilm, grad)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 54 backward at iter %u\n", it);
            cleanup();
            return false;
        }
        lastGrad = grad;
        // 4. STEP.
        const std::vector<float> grads{static_cast<float>(grad)};
        if (!ctx.runAdamProbe(theta, grads, adamState, adam, it)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 54 Adam step at iter %u\n", it);
            cleanup();
            return false;
        }
    }
    cleanup();

    const double thetaFinal = static_cast<double>(theta[0]);
    const double err = std::fabs(thetaFinal - static_cast<double>(kThetaStar));

    // --- NON-VACUITY: the run started somewhere the answer was not.
    if (!(firstLoss > 0.0) || !std::isfinite(firstLoss)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 54 -- the loss at theta_0 = %.9g is %.9g. It "
                     "must be finite and strictly positive, or the optimiser started at the "
                     "answer and recovering it means nothing\n",
                     static_cast<double>(kTheta0), firstLoss);
        return false;
    }

    // --- THE GATE.
    if (!(err <= kRecoveredWithin)) {
        std::fprintf(
            stderr,
            "[diff_gpu_probe] FAIL: check 54 -- GATE 5: theta DID NOT RECOVER theta*.\n"
            "  theta*  = %.9g\n"
            "  theta_0 = %.9g\n"
            "  theta   = %.12g after %u iterations of Adam at alpha = %.9g\n"
            "  |theta - theta*| = %.6g, above the PRE-REGISTERED %.6g (3 * alpha)\n"
            "  loss %.9g -> %.9g, final gradient %.9g\n"
            "  ATTRIBUTION. Checks 37-53 pass on every piece of this loop separately: the\n"
            "  gradient against a finite difference and a convergence law, the loss against\n"
            "  an FD on itself, Adam against the paper's trajectory, and the batch against\n"
            "  its own sum. So a failure HERE is the loop -- the order of the four steps, the\n"
            "  seed reaching the backward pass, or the sign of the step.\n"
            "  IT IS NOT THE DETACHED-SAMPLING BIAS. That is real (spec 6.3, measured by\n"
            "  check 41 at 1-4x the gradient) but it is ABSENT for this parameter: at\n"
            "  metallic 0 the base colour enters no sampling decision, which is the\n"
            "  precondition runWavefrontGradientProbe refuses to run without. This parameter\n"
            "  was chosen first for exactly that reason.\n",
            static_cast<double>(kThetaStar), static_cast<double>(kTheta0), thetaFinal,
            kIterations, static_cast<double>(kAlpha), err, kRecoveredWithin, firstLoss, lastLoss,
            lastGrad);
        return false;
    }

    // --- AND THE LOSS FELL. Recovering theta while the loss rose would mean
    // the two are not measuring the same thing.
    if (!(lastLoss < firstLoss)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 54 -- theta reached %.9g but the loss went "
                     "%.9g -> %.9g, which did not fall. The loss is what was optimised; theta "
                     "arriving while it rose would mean the two are not measuring the same "
                     "scene\n",
                     thetaFinal, firstLoss, lastLoss);
        return false;
    }

    std::printf(
        "[diff_gpu_probe] OK: check 54 -- GATE 5, THE STAGE'S GATE: a known theta* RECOVERED "
        "from a synthetic target. theta* = %.9g, started at %.9g, reached %.12g after %u "
        "iterations of Adam at alpha = %.9g -- |theta - theta*| = %.6g against a PRE-REGISTERED "
        "%.6g, which is 3 * alpha because Adam oscillates about the optimum with an amplitude "
        "of roughly alpha and a tighter criterion would be unmeetable by construction. The loss "
        "fell %.9g -> %.9g. This is the first check that exercises every piece at once -- "
        "forward render, loss kernel, adjoint seed, backward pass and Adam, closed into a loop "
        "-- and the target is rendered at theta* with the SAME seed, so under common random "
        "numbers L(theta*) is exactly 0 and the minimiser is theta* itself rather than a noisy "
        "neighbourhood of it. THE ALBEDO WAS CHOSEN FIRST DELIBERATELY: it is the one parameter "
        "where spec 6.3's detached-sampling bias is ABSENT, since at metallic 0 the base colour "
        "enters no sampling decision, so a failure here could not have been blamed on the "
        "method.\n",
        static_cast<double>(kThetaStar), static_cast<double>(kTheta0), thetaFinal, kIterations,
        static_cast<double>(kAlpha), err, kRecoveredWithin, firstLoss, lastLoss);
    return true;
}

}  // namespace ohao::diff::probe
