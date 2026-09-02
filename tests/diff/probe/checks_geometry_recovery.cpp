// Stage 3 Task 5, check 60: GATE 5 FOR GEOMETRY.
#include "probe/checks_geometry_recovery.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ohao::diff::probe {

namespace {

constexpr std::uint32_t kImage = 8u;
constexpr std::uint32_t kSub = 64u;
constexpr double kLTri = 3.0;
constexpr double kLEnv = 0.5;

bool insideTriangle(const std::vector<float>& v, double px, double py) {
    auto cross = [](double ax, double ay, double bx, double by) { return ax * by - ay * bx; };
    const double s0 = cross(v[2] - v[0], v[3] - v[1], px - v[0], py - v[1]);
    const double s1 = cross(v[4] - v[2], v[5] - v[3], px - v[2], py - v[3]);
    const double s2 = cross(v[0] - v[4], v[1] - v[5], px - v[4], py - v[5]);
    return (s0 >= 0.0 && s1 >= 0.0 && s2 >= 0.0) || (s0 <= 0.0 && s1 <= 0.0 && s2 <= 0.0);
}

/// The forward render: a coverage image, one float per pixel.
///
/// SUPERSAMPLED ON THE HOST rather than traced. That is the whole reason
/// this gate is measurable: with constant radiance inside and outside, moving
/// a vertex changes NOTHING but coverage, so the interior term of spec 4.1 is
/// exactly zero and the derivative being descended is PURELY the boundary
/// term. Against a shaded scene the two are summed and a failed recovery
/// could not be attributed to either.
std::vector<float> renderCoverage(const std::vector<float>& tri) {
    std::vector<float> image(static_cast<std::size_t>(kImage) * kImage, 0.0f);
    for (std::uint32_t py = 0; py < kImage; ++py) {
        for (std::uint32_t px = 0; px < kImage; ++px) {
            std::uint32_t in = 0;
            for (std::uint32_t sy = 0; sy < kSub; ++sy) {
                for (std::uint32_t sx = 0; sx < kSub; ++sx) {
                    if (insideTriangle(tri, px + (sx + 0.5) / kSub, py + (sy + 0.5) / kSub)) {
                        ++in;
                    }
                }
            }
            const double cov = static_cast<double>(in) / (kSub * kSub);
            image[static_cast<std::size_t>(py) * kImage + px] =
                static_cast<float>(cov * kLTri + (1.0 - cov) * kLEnv);
        }
    }
    return image;
}

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

    const std::vector<float> target = renderCoverage(star);
    const std::size_t pixels = target.size();

    ohao::diff::GpuProbeContext::AdamOptions adam;
    adam.alpha = kAlpha;
    std::vector<float> adamState(theta.size() * 2u, 0.0f);
    double firstLoss = 0.0;
    double lastLoss = 0.0;

    for (std::uint32_t it = 1; it <= kIterations; ++it) {
        const std::vector<float> image = renderCoverage(theta);
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
        if (!ctx.runBoundaryProbe(theta, edges, kImage, kImage, static_cast<float>(kLTri),
                                  static_cast<float>(kLEnv), {}, seed, grad)) {
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
