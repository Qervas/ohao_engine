// Stage 3, check 64: a radiance jump that varies along the edge.
#include "probe/checks_boundary_field.hpp"

#include "probe/coverage_render.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ohao::diff::probe {

namespace {

constexpr std::uint32_t kW = 8u, kH = 8u, kSub = 192u;

// THE PRE-REGISTERED CRITERION, fixed before the first run.
//
// The tolerance is check 57's, unchanged, and deliberately not retuned: the
// same oracle at the same supersampling rate against the same geometry
// earns the same bar. What differs is only the field.
constexpr double kOracleTol = 0.05;
constexpr double kStep = 1.0 / 32.0;

/// L(x) = value + grad . x, in GLOBAL screen coordinates.
struct Field {
    double value;
    double gx;
    double gy;
    [[nodiscard]] double at(double x, double y) const { return value + gx * x + gy * y; }
};

// Chosen so the jump swings by more than its own mean across the triangle --
// asserted below rather than trusted -- while staying positive, so that a
// component near zero cannot be mistaken for agreement.
constexpr Field kIn = {3.0, -0.28, 0.19};
constexpr Field kOut = {0.5, 0.12, -0.07};

/// The ORACLE's primitive, and the only thing it knows: whether a point is
/// inside the triangle, and what the two fields are there. No edge, no
/// chord, no normal, no weight, no moment.
double imageTotal(const std::vector<float>& tri, const Field& in, const Field& out) {
    double total = 0.0;
    const double invSub = 1.0 / static_cast<double>(kSub);
    const double invSamples = invSub * invSub;
    for (std::uint32_t py = 0; py < kH; ++py) {
        for (std::uint32_t px = 0; px < kW; ++px) {
            double acc = 0.0;
            for (std::uint32_t sy = 0; sy < kSub; ++sy) {
                for (std::uint32_t sx = 0; sx < kSub; ++sx) {
                    const double x = px + (sx + 0.5) * invSub;
                    const double y = py + (sy + 0.5) * invSub;
                    acc += coverageInsideTriangle(tri, x, y) ? in.at(x, y) : out.at(x, y);
                }
            }
            total += acc * invSamples;
        }
    }
    return total;
}

/// Worst relative disagreement between a gradient and the supersampled image
/// derivative, over all six vertex components.
double worstAgainstOracle(const std::vector<float>& tri, const std::vector<float>& grad,
                          std::size_t& worstIdx) {
    double worst = 0.0;
    worstIdx = 0;
    for (std::size_t k = 0; k < tri.size(); ++k) {
        std::vector<float> plus = tri, minus = tri;
        plus[k] = static_cast<float>(tri[k] + kStep);
        minus[k] = static_cast<float>(tri[k] - kStep);
        const double fd =
            (imageTotal(plus, kIn, kOut) - imageTotal(minus, kIn, kOut)) / (2.0 * kStep);
        const double rel = std::fabs(static_cast<double>(grad[k]) - fd) / (std::fabs(fd) + 1.0);
        if (rel > worst) {
            worst = rel;
            worstIdx = k;
        }
    }
    return worst;
}

}  // namespace

bool checkBoundaryField(ohao::diff::GpuProbeContext& ctx) {
    // The same triangle checks 57 and 60 use, so that the only thing that has
    // changed between this gate and those is the field.
    const std::vector<float> tri = {1.7f, 1.3f, 2.1f, 6.4f, 6.8f, 3.9f};
    const std::vector<std::uint32_t> edges = {0u, 1u, 1u, 2u, 2u, 0u};

    // --- NON-VACUITY OF THE FIELD, before anything is dispatched. A jump
    // that barely varies would be approximated well by any constant, and the
    // control below would fail to fail -- which would leave this gate saying
    // only what check 57 already says.
    double jumpMin = 1e30, jumpMax = -1e30, jumpSum = 0.0;
    for (std::size_t v = 0; v < 3u; ++v) {
        const double j = kIn.at(tri[v * 2u], tri[v * 2u + 1u]) -
                         kOut.at(tri[v * 2u], tri[v * 2u + 1u]);
        jumpMin = std::min(jumpMin, j);
        jumpMax = std::max(jumpMax, j);
        jumpSum += j;
    }
    const double jumpMean = jumpSum / 3.0;
    if (!(jumpMin > 0.0) || !(jumpMax - jumpMin > 0.5 * jumpMean)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 64 -- the jump runs %.6g to %.6g with mean "
                     "%.6g. It must stay positive (so a near-zero component cannot pass for "
                     "agreement) and must swing by more than half its mean, or a constant would "
                     "approximate it well and the control could not fail\n",
                     jumpMin, jumpMax, jumpMean);
        return false;
    }

    ohao::diff::GpuProbeContext::BoundaryRadiance varying;
    varying.lIn = static_cast<float>(kIn.value);
    varying.lOut = static_cast<float>(kOut.value);
    varying.gradIn[0] = static_cast<float>(kIn.gx);
    varying.gradIn[1] = static_cast<float>(kIn.gy);
    varying.gradOut[0] = static_cast<float>(kOut.gx);
    varying.gradOut[1] = static_cast<float>(kOut.gy);

    std::vector<float> grad;
    if (!ctx.runBoundaryProbe(tri, edges, kW, kH, varying, {}, {}, nullptr, 0u, 0u, grad)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 64 boundary dispatch\n");
        return false;
    }
    if (grad.size() != tri.size()) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 64 -- the boundary pass returned %zu floats "
                     "for %zu vertex components\n",
                     grad.size(), tri.size());
        return false;
    }

    std::size_t worstIdx = 0;
    const double worst = worstAgainstOracle(tri, grad, worstIdx);
    if (!(worst <= kOracleTol)) {
        std::fprintf(
            stderr,
            "[diff_gpu_probe] FAIL: check 64 -- with a VARYING jump the GPU disagrees with a "
            "supersampled image derivative at component %zu: %.9g against the oracle, worst "
            "relative %.6g above the PRE-REGISTERED %.6g.\n"
            "  Check 57 runs this same dispatch on the same triangle with a CONSTANT jump and "
            "is green, so the clip, the normal's side, the scatter and the arena addressing are "
            "not at fault. What is new here is the moment integral and the evaluation of the "
            "field in GLOBAL screen coordinates -- a field evaluated as though every pixel sat "
            "at the origin is invisible for a constant jump and wrong for every other.\n",
            worstIdx, static_cast<double>(grad[worstIdx]), worst, kOracleTol);
        return false;
    }

    // --- THE CONTROL: the kernel's own previous form, given its best shot.
    // The constants are the fields at the triangle's CENTROID, which is the
    // single best constant the earlier kernel could have been handed -- not a
    // strawman constant chosen to fail.
    const double cx = (tri[0] + tri[2] + tri[4]) / 3.0;
    const double cy = (tri[1] + tri[3] + tri[5]) / 3.0;
    ohao::diff::GpuProbeContext::BoundaryRadiance constant;
    constant.lIn = static_cast<float>(kIn.at(cx, cy));
    constant.lOut = static_cast<float>(kOut.at(cx, cy));

    std::vector<float> constGrad;
    if (!ctx.runBoundaryProbe(tri, edges, kW, kH, constant, {}, {}, nullptr, 0u, 0u, constGrad)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 64 control dispatch\n");
        return false;
    }
    std::size_t controlIdx = 0;
    const double controlWorst = worstAgainstOracle(tri, constGrad, controlIdx);
    if (!(controlWorst > kOracleTol)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 64 -- THE CONTROL PASSED. The best single "
                     "constant (the fields at the centroid) agrees with the oracle to %.6g, "
                     "inside the %.6g this gate requires of the varying form. Then the varying "
                     "form is not NECESSARY in this scene, and this gate is measuring what "
                     "check 57 already measures\n",
                     controlWorst, kOracleTol);
        return false;
    }

    std::printf(
        "[diff_gpu_probe] OK: check 64 -- A RADIANCE JUMP THAT VARIES ALONG THE EDGE. The "
        "radiance on each side is now an AFFINE FIELD of screen position rather than one pushed "
        "constant, so the jump varies along every chord and the kernel integrates it as a "
        "MOMENT -- INTEGRAL (j0 + j1 u)(1-u) du and INTEGRAL (j0 + j1 u) u du -- instead of a "
        "weight times a scalar. Against a supersampled image derivative the worst relative "
        "disagreement is %.6g at component %zu, inside the PRE-REGISTERED %.4g, which is check "
        "57's tolerance unchanged: the same oracle at the same rate on the same triangle earns "
        "the same bar, and retuning it would be the tell. THE ORACLE SURVIVES THE "
        "GENERALISATION because these fields depend on screen position but NOT on theta -- so "
        "spec 4.1's interior term is still exactly zero and the supersampled difference is "
        "still purely this integral. The jump runs %.4g to %.4g about a mean of %.4g, asserted "
        "before the dispatch, because a jump that barely varied would be approximated well by "
        "any constant. AND THE CONTROL IS THE KERNEL'S OWN PREVIOUS FORM, given its best shot: "
        "the same dispatch with the gradients zeroed and the constants set to the fields at the "
        "triangle's CENTROID -- the single best constant the earlier kernel could express -- "
        "misses the oracle by %.6g at component %zu, %.1fx the bar the varying form meets. So "
        "the generalisation is NECESSARY here, not merely present. STILL OWED: a traced "
        "radiance is not affine, so the two-point exactness above becomes a two-point quadrature "
        "with a truncation term. This layer is the jump varying; the trace is the next.\n",
        worst, worstIdx, kOracleTol, jumpMin, jumpMax, jumpMean, controlWorst, controlIdx,
        controlWorst / kOracleTol);
    return true;
}

}  // namespace ohao::diff::probe
