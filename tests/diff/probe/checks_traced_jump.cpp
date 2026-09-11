// Item 6 task 2, check 69: the radiance, traced.
#include "probe/checks_traced_jump.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ohao::diff::probe {

namespace {

constexpr std::uint32_t kImage = 8u;
constexpr float kEmission = 3.0f;
constexpr float kBackground = 0.5f;
// screen = world.xy * kScale + kOffset, the same affine map check 59 uses.
// The quad's [-1,1] square lands on [2,6] of an 8x8 image, inside it with
// margin so no edge is clipped by the frame.
// OFF-INTEGER, because an edge lying exactly on a pixel seam is clipped
// into BOTH adjacent pixels and its contribution scattered twice -- see the
// note in checks_shaded_order.cpp, whose oracle found it. This check compares
// two paths through the same kernel, so the doubling would cancel and go
// unnoticed here; the scene steps off the boundary so that what it measures
// is the non-degenerate case.
constexpr float kScale = 1.7f;
constexpr float kOffset = 4.05f;

/// A quad at z = 0, as two triangles. Its four boundary edges ARE its
/// silhouette -- an open mesh seen face-on -- so no silhouette pass is
/// needed and the scene stays the smallest thing that can be traced.
std::vector<float> quadWorld() {
    return {-1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, -1.0f, 1.0f, 0.0f};
}

std::vector<std::uint32_t> quadIndices() { return {0u, 1u, 2u, 0u, 2u, 3u}; }

/// Twice the signed area, in SCREEN space. Negative is the clockwise winding
/// this pass's normal convention requires; a reversed one negates the whole
/// boundary term.
double signedArea(const std::vector<float>& screen) {
    const std::size_t n = screen.size() / 2u;
    double acc = 0.0;
    for (std::size_t i = 0, j = n - 1u; i < n; j = i++) {
        acc += static_cast<double>(screen[j * 2u + 0u]) * static_cast<double>(screen[i * 2u + 1u]) -
               static_cast<double>(screen[i * 2u + 0u]) * static_cast<double>(screen[j * 2u + 1u]);
    }
    return acc;
}

}  // namespace

bool checkTracedJump(ohao::diff::GpuProbeContext& ctx) {
    const std::vector<float> world = quadWorld();
    const std::vector<std::uint32_t> indices = quadIndices();

    // The quad's corners in screen space, and its four boundary edges. The
    // ORDER is chosen to wind clockwise and asserted below rather than
    // trusted -- see the note on signedArea.
    std::vector<float> screen;
    for (std::size_t v = 0; v < 4u; ++v) {
        screen.push_back(world[v * 3u + 0u] * kScale + kOffset);
        screen.push_back(world[v * 3u + 1u] * kScale + kOffset);
    }
    if (!(signedArea(screen) < 0.0)) {
        // The quad as listed winds counter-clockwise in screen space, so
        // reverse the EDGE order rather than the vertex order -- the vertex
        // indices must keep matching the triangles the BLAS is built from.
        std::vector<float> reversed;
        for (std::size_t v = 4u; v > 0u; --v) {
            reversed.push_back(screen[(v - 1u) * 2u + 0u]);
            reversed.push_back(screen[(v - 1u) * 2u + 1u]);
        }
        screen = reversed;
    }
    if (!(signedArea(screen) < 0.0)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 69 -- the quad winds %.6g in screen space "
                     "even after reversal; it must be negative (clockwise), or the boundary "
                     "term is negated and the comparison below measures a sign error twice\n",
                     signedArea(screen));
        return false;
    }
    const std::vector<std::uint32_t> edges = {0u, 1u, 1u, 2u, 2u, 3u, 3u, 0u};

    // --- THE PUSHED FORM, which is exactly right on this scene: one emissive
    // surface, one background, so two constants are the whole truth.
    ohao::diff::GpuProbeContext::BoundaryRadiance pushed;
    pushed.lIn = kEmission;
    pushed.lOut = kBackground;
    std::vector<float> pushedGrad;
    if (!ctx.runBoundaryProbe(screen, edges, kImage, kImage, pushed, {}, {}, nullptr, 0u, 0u,
                              pushedGrad)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 69 pushed dispatch\n");
        return false;
    }

    // --- THE TRACED FORM. Same edges, same pixels; the two radiances now come
    // from a ray hitting the quad or missing it.
    ohao::diff::GpuProbeContext::OwnedScene scene;
    if (!ctx.buildOwnedScene(std::span<const float>(world),
                             std::span<const std::uint32_t>(indices), scene)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 69 -- buildOwnedScene\n");
        return false;
    }
    const std::vector<float> perPrimitive(indices.size() / 3u, kEmission);
    GpuBuffer emissionBuffer = ctx.allocator().createBufferFromSpan<float>(
        std::span<const float>(perPrimitive), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    ohao::diff::GpuProbeContext::BoundaryRadiance traced;
    traced.trace = true;
    traced.tlas = scene.tlas;
    traced.emission = emissionBuffer.buffer;
    traced.primitiveCount = static_cast<std::uint32_t>(perPrimitive.size());
    traced.screenScale = kScale;
    traced.screenOffset[0] = kOffset;
    traced.screenOffset[1] = kOffset;
    traced.rayOriginZ = 10.0f;   // above the quad, looking down -z
    traced.traceEps = 0.25f;     // screen units: a quarter pixel
    traced.background = kBackground;

    std::vector<float> tracedGrad;
    const bool ok = ctx.runBoundaryProbe(screen, edges, kImage, kImage, traced, {}, {}, nullptr,
                                         0u, 0u, tracedGrad);
    ctx.allocator().destroyBuffer(emissionBuffer);
    ctx.destroyOwnedScene(scene);
    if (!ok) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 69 traced dispatch\n");
        return false;
    }

    // --- NON-VACUITY: the pushed gradient must be substantial, or agreeing
    // on two vectors of zeros would pass.
    double scale = 0.0;
    for (const float g : pushedGrad) scale = std::max(scale, std::fabs(static_cast<double>(g)));
    if (!(scale > 0.1)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 69 -- the pushed boundary term is %.9g at its "
                     "largest component, indistinguishable from zero. Agreeing on nothing is not "
                     "agreement\n",
                     scale);
        return false;
    }

    if (tracedGrad.size() != pushedGrad.size()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 69 -- %zu floats against %zu\n",
                     tracedGrad.size(), pushedGrad.size());
        return false;
    }
    double worst = 0.0;
    std::size_t worstIdx = 0;
    for (std::size_t k = 0; k < pushedGrad.size(); ++k) {
        const double d =
            std::fabs(static_cast<double>(tracedGrad[k]) - static_cast<double>(pushedGrad[k]));
        if (d > worst) {
            worst = d;
            worstIdx = k;
        }
    }
    const double kTol = 1e-4 * scale;
    if (!(worst <= kTol)) {
        std::fprintf(
            stderr,
            "[diff_gpu_probe] FAIL: check 69 -- THE TRACED JUMP DISAGREES WITH THE PUSHED ONE.\n"
            "  component %zu: traced %.9g, pushed %.9g, |diff| %.6g above %.6g\n"
            "  On this scene the pushed constants are EXACTLY right -- one emissive surface at "
            "%.4g against a background of %.4g -- so the pushed form is the answer and the trace "
            "is what is being measured.\n"
            "  A SIGN-SIZED disagreement means the two samples landed on the wrong sides of the "
            "edge: 'inside' is the NEGATIVE side of the normal, and the winding is what fixes "
            "which that is. A SMALLER one means the screen-to-world map is off, so the rays "
            "sample the right scene in the wrong place -- screen = world * %.4g + %.4g here.\n",
            worstIdx, static_cast<double>(tracedGrad[worstIdx]),
            static_cast<double>(pushedGrad[worstIdx]), worst, kTol,
            static_cast<double>(kEmission), static_cast<double>(kBackground),
            static_cast<double>(kScale), static_cast<double>(kOffset));
        return false;
    }

    std::printf(
        "[diff_gpu_probe] OK: check 69 -- THE RADIANCE, TRACED. The boundary term's two "
        "radiances now come from a ray hitting an emissive quad or missing it, rather than from "
        "two push constants, and they agree with the pushed form to %.3g of its largest "
        "component (%zu components, tolerance %.3g). THE CONTROL IS INVERTED relative to check "
        "64's, and that is the design: there the pushed form had to FAIL because the claim was "
        "that a varying jump is necessary, whereas here it must AGREE because the claim is that "
        "tracing finds what a human previously typed in. On one emissive surface at %.4g "
        "against a background of %.4g, two constants are the whole truth -- so a disagreement "
        "would mean the trace is wrong, not that the layer is needed. Two specific errors land "
        "here with numbers known exactly: sampling the wrong side of the edge negates the jump "
        "(the winding convention, which has bitten this subsystem three times), and a wrong "
        "screen-to-world map traces the right scene in the wrong place, which otherwise looks "
        "like a plausible gradient. THE JUMP IS PIECEWISE CONSTANT because emission is per "
        "primitive, so the moment integral stays EXACT and there is no truncation term -- which "
        "is why this layer gets a fixed tolerance and not the convergence-order gate the "
        "roadmap assumed. THE INTERIOR TERM IS STILL EXACTLY ZERO: emission does not depend on "
        "where the vertices are, so tracing changed where the number comes from and nothing "
        "else. STILL OWED: a SHADED radiance, which varies within a surface and does make the "
        "two-point rule approximate.\n",
        worst / scale, pushedGrad.size(), kTol, static_cast<double>(kEmission),
        static_cast<double>(kBackground));
    return true;
}

}  // namespace ohao::diff::probe
