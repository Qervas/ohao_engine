// Stage 3, check 57: the boundary kernel on the GPU.
#include "probe/checks_boundary_gpu.hpp"

#include "diff/geom/boundary_integrand.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ohao::diff::probe {

namespace {

struct Tri {
    float v[3][2];
};

bool insideTriangle(const Tri& t, double px, double py) {
    auto cross = [](double ax, double ay, double bx, double by) { return ax * by - ay * bx; };
    const double s0 = cross(t.v[1][0] - t.v[0][0], t.v[1][1] - t.v[0][1], px - t.v[0][0],
                            py - t.v[0][1]);
    const double s1 = cross(t.v[2][0] - t.v[1][0], t.v[2][1] - t.v[1][1], px - t.v[1][0],
                            py - t.v[1][1]);
    const double s2 = cross(t.v[0][0] - t.v[2][0], t.v[0][1] - t.v[2][1], px - t.v[2][0],
                            py - t.v[2][1]);
    return (s0 >= 0.0 && s1 >= 0.0 && s2 >= 0.0) || (s0 <= 0.0 && s1 <= 0.0 && s2 <= 0.0);
}

/// The ORACLE's primitive: J by supersampled coverage. It knows only whether
/// a point is inside the triangle -- no edge, no chord, no normal, no weight.
double imageTotal(const Tri& t, std::uint32_t w, std::uint32_t h, double lTri, double lEnv,
                  std::uint32_t sub) {
    double total = 0.0;
    for (std::uint32_t py = 0; py < h; ++py) {
        for (std::uint32_t px = 0; px < w; ++px) {
            std::uint32_t in = 0;
            for (std::uint32_t sy = 0; sy < sub; ++sy) {
                for (std::uint32_t sx = 0; sx < sub; ++sx) {
                    if (insideTriangle(t, px + (sx + 0.5) / sub, py + (sy + 0.5) / sub)) ++in;
                }
            }
            const double cov = static_cast<double>(in) / (sub * sub);
            total += cov * lTri + (1.0 - cov) * lEnv;
        }
    }
    return total;
}

}  // namespace

bool checkBoundaryGpu(ohao::diff::GpuProbeContext& ctx) {
    constexpr std::uint32_t kW = 8, kH = 8, kSub = 192;
    constexpr double kLTri = 3.0, kLEnv = 0.5;

    const Tri tri = {{{1.7f, 1.3f}, {2.1f, 6.4f}, {6.8f, 3.9f}}};
    const std::vector<float> positions = {tri.v[0][0], tri.v[0][1], tri.v[1][0],
                                          tri.v[1][1], tri.v[2][0], tri.v[2][1]};
    // The triangle's three edges, in winding order -- which is what fixes the
    // normal's side and therefore which radiance is "in".
    const std::vector<std::uint32_t> edges = {0u, 1u, 1u, 2u, 2u, 0u};

    std::vector<float> gpuGrad;
    if (!ctx.runBoundaryProbe(positions, edges, kW, kH, static_cast<float>(kLTri),
                              static_cast<float>(kLEnv), {}, gpuGrad)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 57 boundary dispatch\n");
        return false;
    }
    if (gpuGrad.size() != positions.size()) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 57 -- the boundary pass returned %zu floats "
                     "for %zu vertex components\n",
                     gpuGrad.size(), positions.size());
        return false;
    }

    // --- THE HOST FORM, over the same edges and pixels. Not the oracle --
    // the oracle is below -- but the comparison that localises a
    // disagreement to the GPU rather than to the mathematics, since this
    // function is what the unit tests already gate against a supersampled
    // derivative.
    std::vector<double> cpuGrad(positions.size(), 0.0);
    for (std::uint32_t e = 0; e < 3u; ++e) {
        const std::uint32_t a = edges[e * 2u + 0u];
        const std::uint32_t b = edges[e * 2u + 1u];
        for (std::uint32_t py = 0; py < kH; ++py) {
            for (std::uint32_t px = 0; px < kW; ++px) {
                ohao::diff::PixelEdge pe;
                pe.p0[0] = positions[a * 2u + 0u] - static_cast<float>(px);
                pe.p0[1] = positions[a * 2u + 1u] - static_cast<float>(py);
                pe.p1[0] = positions[b * 2u + 0u] - static_cast<float>(px);
                pe.p1[1] = positions[b * 2u + 1u] - static_cast<float>(py);
                for (int comp = 0; comp < 2; ++comp) {
                    const float d[2] = {comp == 0 ? 1.0f : 0.0f, comp == 1 ? 1.0f : 0.0f};
                    cpuGrad[a * 2u + comp] +=
                        ohao::diff::boundaryTermMovingP0(pe, d, kLTri, kLEnv);
                    cpuGrad[b * 2u + comp] +=
                        ohao::diff::boundaryTermMovingP1(pe, d, kLTri, kLEnv);
                }
            }
        }
    }

    double worstAbs = 0.0;
    std::size_t worstIdx = 0;
    double scale = 0.0;
    for (std::size_t k = 0; k < cpuGrad.size(); ++k) {
        scale = std::max(scale, std::fabs(cpuGrad[k]));
    }
    if (!(scale > 0.1)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 57 -- the host boundary term is %.9g at its "
                     "largest component, indistinguishable from zero. Agreeing with the GPU on "
                     "nothing is not agreement\n",
                     scale);
        return false;
    }
    for (std::size_t k = 0; k < cpuGrad.size(); ++k) {
        const double diff = std::fabs(static_cast<double>(gpuGrad[k]) - cpuGrad[k]);
        if (diff > worstAbs) {
            worstAbs = diff;
            worstIdx = k;
        }
    }
    // float32 on the GPU against double on the host, over a sum of at most
    // 3 * 64 contributions per component.
    const double kHostTol = 1e-4 * scale;
    if (!(worstAbs <= kHostTol)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 57 -- the GPU boundary pass disagrees with "
                     "the host form at component %zu: %.9g against %.9g, |diff| %.6g above "
                     "%.6g.\n"
                     "  The host form is gated by diff_unit_tests against a SUPERSAMPLED image "
                     "derivative, so a disagreement here is the DISPATCH -- the clip, the "
                     "normal's side, the barycentric weights, or the arena addressing -- and "
                     "not the mathematics.\n",
                     worstIdx, static_cast<double>(gpuGrad[worstIdx]), cpuGrad[worstIdx],
                     worstAbs, kHostTol);
        return false;
    }

    // --- THE ORACLE, applied to the GPU's OWN numbers. Without this the GPU
    // is only ever compared to another implementation; this ties it to a
    // supersampled coverage difference that shares no line with either.
    constexpr double kStep = 1.0 / 32.0;
    double worstOracleRel = 0.0;
    std::size_t worstOracleIdx = 0;
    for (std::uint32_t v = 0; v < 3u; ++v) {
        for (int comp = 0; comp < 2; ++comp) {
            Tri plus = tri, minus = tri;
            plus.v[v][comp] = static_cast<float>(tri.v[v][comp] + kStep);
            minus.v[v][comp] = static_cast<float>(tri.v[v][comp] - kStep);
            const double fd = (imageTotal(plus, kW, kH, kLTri, kLEnv, kSub) -
                               imageTotal(minus, kW, kH, kLTri, kLEnv, kSub)) /
                              (2.0 * kStep);
            const double got = static_cast<double>(gpuGrad[v * 2u + comp]);
            const double rel = std::fabs(got - fd) / (std::fabs(fd) + 1.0);
            if (rel > worstOracleRel) {
                worstOracleRel = rel;
                worstOracleIdx = v * 2u + comp;
            }
            if (!(rel <= 0.05)) {
                std::fprintf(
                    stderr,
                    "[diff_gpu_probe] FAIL: check 57 -- vertex %u component %d: the GPU gives "
                    "%.9g and a SUPERSAMPLED image derivative gives %.9g, relative %.6g above "
                    "0.05.\n"
                    "  This oracle counts points inside the triangle and differences the "
                    "result. It has no edge, no chord, no normal and no barycentric weight, so "
                    "it cannot share an error with the pass it checks -- which is how the host "
                    "form's own sign error, and then a 3x orientation error, were both "
                    "found.\n",
                    v, comp, got, fd, rel);
                return false;
            }
        }
    }

    std::printf(
        "[diff_gpu_probe] OK: check 57 -- THE BOUNDARY KERNEL ON THE GPU. spec 4.1's second "
        "term as a dispatch: one invocation per (edge, pixel), the edge clipped to the pixel, "
        "the integrand evaluated over the chord and scattered to the edge's TWO VERTICES with "
        "the barycentric weights that integral produces. A %ux%u image, 3 edges, %zu vertex "
        "components. CHECKED TWICE, and the second is what matters: against the host form to "
        "within %.3g of the largest component -- which localises a disagreement to the DISPATCH, "
        "since diff_unit_tests already gates that host form -- and against a SUPERSAMPLED IMAGE "
        "DERIVATIVE, worst relative %.4g at component %zu. That oracle counts points inside the "
        "triangle and differences the result; it has no edge, no chord, no normal and no "
        "weight, so it cannot share an error with what it checks. It is how this term's sign "
        "error and then a 3x orientation error were both found. THE RADIANCES ARE PUSHED, NOT "
        "TRACED: this is the coverage case, where moving a vertex changes nothing but coverage "
        "so the interior term is exactly zero and the measured derivative IS this integral. "
        "Substituting two ray traces for the two constants is the next layer; the clip, the "
        "weights and the scatter are what this dispatch exists to get right first.\n",
        kW, kH, positions.size(), worstAbs / scale, worstOracleRel, worstOracleIdx);
    return true;
}

}  // namespace ohao::diff::probe
