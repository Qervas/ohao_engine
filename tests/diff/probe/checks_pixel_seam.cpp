// Check 71: an edge exactly on a pixel seam is counted once.
#include "probe/checks_pixel_seam.hpp"

#include "probe/coverage_render.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ohao::diff::probe {

namespace {

constexpr std::uint32_t kImage = 8u;
constexpr std::uint32_t kSub = 192u;
constexpr double kLIn = 3.0;
constexpr double kLOut = 0.5;
constexpr double kStep = 1.0 / 32.0;
// Check 57's tolerance, unchanged: same oracle, same supersampling rate.
constexpr double kOracleTol = 0.05;

/// A quad whose edges lie EXACTLY on integer coordinates, wound clockwise --
/// the degenerate case, chosen deliberately.
std::vector<float> seamQuad() {
    return {2.0f, 2.0f, 2.0f, 6.0f, 6.0f, 6.0f, 6.0f, 2.0f};
}

double imageTotal(const std::vector<float>& poly) {
    const std::vector<float> img = renderPolygonCoverage(poly, kImage, kSub, kLIn, kLOut);
    double total = 0.0;
    for (const float v : img) total += static_cast<double>(v);
    return total;
}

}  // namespace

bool checkPixelSeam(ohao::diff::GpuProbeContext& ctx) {
    const std::vector<float> quad = seamQuad();
    const std::vector<std::uint32_t> edges = {0u, 1u, 1u, 2u, 2u, 3u, 3u, 0u};

    // --- NON-VACUITY OF THE SCENE: every coordinate must actually BE on a
    // seam, or this check is quietly testing the ordinary case that the rest
    // of the suite already covers.
    for (std::size_t k = 0; k < quad.size(); ++k) {
        if (quad[k] != std::floor(quad[k])) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 71 -- coordinate %zu is %.9g, not an "
                         "integer. The whole point of this scene is that its edges lie ON pixel "
                         "seams; off them it measures what checks 57 and 69 already do\n",
                         k, static_cast<double>(quad[k]));
            return false;
        }
    }

    std::vector<float> grad;
    if (!ctx.runBoundaryProbe(quad, edges, kImage, kImage,
                              {static_cast<float>(kLIn), static_cast<float>(kLOut)}, {}, {},
                              nullptr, 0u, 0u, grad)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 71 boundary dispatch\n");
        return false;
    }

    double scale = 0.0;
    for (const float g : grad) scale = std::max(scale, std::fabs(static_cast<double>(g)));
    if (!(scale > 0.1)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 71 -- the boundary term is %.9g at its "
                     "largest component, indistinguishable from zero. A half-open clip that "
                     "rejected BOTH sides of every seam would land here, and agreeing with an "
                     "oracle on nothing is not agreement\n",
                     scale);
        return false;
    }

    double worst = 0.0;
    std::size_t worstIdx = 0;
    double worstGpu = 0.0, worstFd = 0.0;
    for (std::size_t k = 0; k < quad.size(); ++k) {
        std::vector<float> plus = quad, minus = quad;
        plus[k] = static_cast<float>(quad[k] + kStep);
        minus[k] = static_cast<float>(quad[k] - kStep);
        const double fd = (imageTotal(plus) - imageTotal(minus)) / (2.0 * kStep);
        const double rel = std::fabs(static_cast<double>(grad[k]) - fd) / (std::fabs(fd) + 1.0);
        if (rel > worst) {
            worst = rel;
            worstIdx = k;
            worstGpu = static_cast<double>(grad[k]);
            worstFd = fd;
        }
    }

    if (!(worst <= kOracleTol)) {
        std::fprintf(
            stderr,
            "[diff_gpu_probe] FAIL: check 71 -- AN EDGE ON A PIXEL SEAM IS NOT COUNTED ONCE.\n"
            "  component %zu: GPU %.9g, supersampled oracle %.9g, ratio %.6g\n"
            "  worst relative %.6g above %.4g\n"
            "  A ratio near 2 is the original defect: the pixel below-left of a seam and the "
            "one above-right both accept the edge as a chord, so its contribution is scattered "
            "twice. clipToPixel treats the pixel as half-open, [0,1) on each axis, to make "
            "exactly one of them keep it.\n"
            "  A ratio near 0 is the overcorrection: both sides reject it and the seam "
            "contributes nothing at all.\n",
            worstIdx, worstGpu, worstFd, worstFd == 0.0 ? 0.0 : worstGpu / worstFd, worst,
            kOracleTol);
        return false;
    }

    std::printf(
        "[diff_gpu_probe] OK: check 71 -- AN EDGE EXACTLY ON A PIXEL SEAM IS COUNTED ONCE. A "
        "quad whose four edges lie on integer coordinates -- the degenerate case, chosen on "
        "purpose because it is what a hand-written test scene naturally looks like -- agrees "
        "with a SUPERSAMPLED IMAGE DERIVATIVE to %.6g at its worst component, inside check 57's "
        "unchanged %.4g. Before `clipToPixel` was made half-open, [0,1) on each axis, both "
        "pixels adjacent to a seam accepted the edge as a chord and the gradient came out at "
        "EXACTLY DOUBLE: check 70's oracle measured 2.00007, 1.99987 and 1.99773 times the "
        "truth. THIS CHECK EXISTS BECAUSE THE FIX IS OTHERWISE UNTESTABLE -- only the "
        "parallel-to-a-boundary case changes, and once the bug was understood the other scenes "
        "were moved OFF integer coordinates, so the whole suite reproduces byte for byte "
        "whether the fix is present or not. A correctness fix no check can distinguish from its "
        "absence is not fixed, only written. AND THE ORACLE IS THE ONLY KIND THAT COULD SEE IT: "
        "any comparison between two forms of this kernel shares the doubling and cancels it "
        "exactly, which is why check 69 -- traced against pushed -- was green throughout.\n",
        worst, kOracleTol);
    return true;
}

}  // namespace ohao::diff::probe
