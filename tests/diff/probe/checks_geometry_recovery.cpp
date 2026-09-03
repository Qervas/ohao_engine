// Stage 3 Task 5, check 60: GATE 5 FOR GEOMETRY.
#include "probe/checks_geometry_recovery.hpp"

#include "probe/coverage_render.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ohao::diff::probe {

namespace {

// The forward model lives in probe/coverage_render.hpp, shared with check 62
// -- one supersampled coverage render, so the two geometry gates descend the
// same thing and the only difference between them is what they optimise.
constexpr std::uint32_t kImage = 8u;
constexpr std::uint32_t kSub = 64u;
constexpr double kLTri = 3.0;
constexpr double kLEnv = 0.5;

}  // namespace

bool checkGeometryRecovery(ohao::diff::GpuProbeContext& ctx) {
    // THE PRE-REGISTERED CRITERION, fixed before the first run.
    const std::vector<float> star = {1.7f, 1.3f, 2.1f, 6.4f, 6.8f, 3.9f};
    constexpr float kOffsetX = 0.5f, kOffsetY = -0.4f;
    constexpr std::uint32_t kIterations = 150u;
    constexpr float kAlpha = 0.05f;
    constexpr double kRecoveredWithin = 0.15;  // 3 * kAlpha, for Adam's
                                               // terminal oscillation
    const std::vector<std::uint32_t> edges = {0u, 1u, 1u, 2u, 2u, 0u};

    std::vector<float> theta = star;
    for (std::size_t k = 0; k < theta.size(); ++k) {
        theta[k] += (k % 2u == 0u) ? kOffsetX : kOffsetY;
    }
    const std::vector<float> theta0 = theta;

    const std::vector<float> target = renderTriangleCoverage(star, kImage, kSub, kLTri, kLEnv);
    const std::size_t pixels = target.size();

    ohao::diff::GpuProbeContext::AdamOptions adam;
    adam.alpha = kAlpha;
    std::vector<float> adamState(theta.size() * 2u, 0.0f);
    double firstLoss = 0.0;
    double lastLoss = 0.0;

    for (std::uint32_t it = 1; it <= kIterations; ++it) {
        const std::vector<float> image = renderTriangleCoverage(theta, kImage, kSub, kLTri, kLEnv);
        // L = (1/N) SUM (I - T)^2, so dL/dI_p = 2 (I_p - T_p) / N. Computed
        // on the HOST rather than through loss_l2.comp: that kernel is
        // written for a 3-channel film and this is a 1-channel coverage
        // image. The formula is check 51's, and check 51 is what gates it.
        double loss = 0.0;
        std::vector<float> seed(pixels, 0.0f);
        for (std::size_t p = 0; p < pixels; ++p) {
            const double d = static_cast<double>(image[p]) - static_cast<double>(target[p]);
            loss += d * d / static_cast<double>(pixels);
            seed[p] = static_cast<float>(2.0 * d / static_cast<double>(pixels));
        }
        if (it == 1u) firstLoss = loss;
        lastLoss = loss;

        // dL/dv, from the boundary pass. A lone triangle against a background
        // has a real jump across ALL THREE of its edges, so no silhouette
        // filter applies -- every edge is a silhouette edge here.
        std::vector<float> grad;
        if (!ctx.runBoundaryProbe(theta, edges, kImage, kImage,
                                  {static_cast<float>(kLTri), static_cast<float>(kLEnv)}, {}, seed, nullptr, 0u, 0u, grad)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 60 boundary dispatch at iter %u\n",
                         it);
            return false;
        }
        if (!ctx.runAdamProbe(theta, grad, adamState, adam, it)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 60 Adam step at iter %u\n", it);
            return false;
        }
    }

    double worstErr = 0.0;
    std::size_t worstIdx = 0;
    double startErr = 0.0;
    for (std::size_t k = 0; k < theta.size(); ++k) {
        const double err = std::fabs(static_cast<double>(theta[k]) - static_cast<double>(star[k]));
        if (err > worstErr) {
            worstErr = err;
            worstIdx = k;
        }
        startErr = std::max(startErr, std::fabs(static_cast<double>(theta0[k]) -
                                                static_cast<double>(star[k])));
    }

    if (!(firstLoss > 0.0) || !std::isfinite(firstLoss)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 60 -- the loss at theta_0 is %.9g. It must be "
                     "finite and strictly positive, or the optimiser started at the answer\n",
                     firstLoss);
        return false;
    }
    if (!(worstErr <= kRecoveredWithin)) {
        std::fprintf(
            stderr,
            "[diff_gpu_probe] FAIL: check 60 -- GATE 5 FOR GEOMETRY: the vertices did not "
            "recover.\n"
            "  worst component %zu: theta* = %.9g, theta_0 = %.9g, theta = %.9g\n"
            "  |error| = %.6g, above the PRE-REGISTERED %.6g (3 * alpha)\n"
            "  loss %.9g -> %.9g over %u iterations\n"
            "  ATTRIBUTION. The interior term is EXACTLY ZERO in this scene -- constant\n"
            "  radiance on each side, so moving a vertex changes nothing but coverage -- so\n"
            "  the gradient descended here is purely the boundary term, and a failure cannot\n"
            "  be the interior machinery. Checks 57 and 59 gate that boundary term against a\n"
            "  supersampled image derivative and an exact null test, and check 52 gates Adam\n"
            "  against the paper. So a failure HERE is the loop: the seed reaching the pass,\n"
            "  the sign of the step, or the order of the four stages.\n",
            worstIdx, static_cast<double>(star[worstIdx]),
            static_cast<double>(theta0[worstIdx]), static_cast<double>(theta[worstIdx]),
            worstErr, kRecoveredWithin, firstLoss, lastLoss, kIterations);
        return false;
    }
    if (!(lastLoss < firstLoss)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 60 -- the vertices arrived but the loss went "
                     "%.9g -> %.9g, which did not fall. That would mean the two are not "
                     "measuring the same image\n",
                     firstLoss, lastLoss);
        return false;
    }

    std::printf(
        "[diff_gpu_probe] OK: check 60 -- GATE 5 FOR GEOMETRY: a triangle's %zu vertex "
        "components recovered from a synthetic target by descending the BOUNDARY TERM. Started "
        "%.4g from theta* at its worst component and finished %.6g away, against a "
        "PRE-REGISTERED %.4g (3 * alpha, because Adam oscillates about the optimum with an "
        "amplitude of roughly alpha). Loss %.9g -> %.9g over %u iterations of Adam at alpha = "
        "%.4g. THE INTERIOR TERM IS EXACTLY ZERO HERE, and that is what makes this gate "
        "attributable rather than merely green: constant radiance on each side means moving a "
        "vertex changes nothing but coverage, so spec 4.1's interior integral vanishes and the "
        "gradient being descended is PURELY the boundary term. Against a shaded scene the two "
        "would sum and a failure could not be blamed on either. The loop is: supersampled "
        "coverage render, L2 loss, the GPU boundary pass seeded with dL/dpixel, one Adam step "
        "-- and every piece of it is separately gated (57 and 59 for the boundary term, 52 for "
        "Adam), so a failure here would have been the LOOP.\n",
        theta.size(), startErr, worstErr, kRecoveredWithin, firstLoss, lastLoss, kIterations,
        static_cast<double>(kAlpha));
    return true;
}

}  // namespace ohao::diff::probe
