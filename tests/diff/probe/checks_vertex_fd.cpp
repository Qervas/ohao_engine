// Stage 3 Task 4, check 56: the vertex-position finite difference, and the
// measurement that says why Stage 3 exists.
#include "probe/checks_vertex_fd.hpp"

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

bool checkVertexFiniteDifference(ohao::diff::GpuProbeContext& ctx) {
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 8;
    constexpr uint32_t kCapacity = kW * kH;
    constexpr uint32_t kEnvW = 64;
    constexpr uint32_t kEnvH = 32;
    constexpr uint32_t kBounces = 3u;
    constexpr uint32_t kSeed = 20260828u;
    constexpr float kAlbedo = 0.6f;
    // Coarse, and deliberately so. A vertex perturbation MOVES GEOMETRY, so
    // the film is not a smooth function of it at any scale -- the point of
    // this check is to measure that, not to defeat it with a small step.
    constexpr float kStep = 0.03125f;  // 2^-5

    ohao::diff::ParamRegistry reg;
    const auto regAlbedo = reg.registerScalarBlock("albedo", 1);
    if (!regAlbedo.ok) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 56 registry setup: %s\n",
                     regAlbedo.error.c_str());
        return false;
    }
    const ohao::diff::DiffParam* albedoParam = reg.find("albedo");
    ohao::diff::GradientArena arena;
    if (albedoParam == nullptr || !arena.build(ctx.allocator(), reg.layout())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 56 arena/param setup\n");
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
    ohao::diff::WavefrontBuffers wf;
    if (!cdf.valid() || !wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH) ||
        !wf.uploadEnvironment(cdf.marginalSpan(), cdf.conditionalSpan(), cdf.integral())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 56 scene/buffers setup\n");
        wf.destroy(ctx.allocator());
        arena.destroy(ctx.allocator());
        return false;
    }

    const ohao::diff::WavefrontScatterMaterial kMaterial{1.0f, 0.0f, 0.0f};

    /// Render at a given geometry, returning J and the forward vertex trace.
    auto render = [&](const std::vector<float>& pos, double& outJ,
                      std::vector<float>& outTrace) -> bool {
        ohao::diff::WavefrontGradientOptions options;
        options.diffParam = 0u;
        options.outForwardTrace = &outTrace;
        std::vector<float> film;
        if (!ctx.runWavefrontGradientProbe(wf, kW, kH, kBounces, camera,
                                           std::span<const float>(pos),
                                           std::span<const uint32_t>(indices), kAlbedo, kMaterial,
                                           kSeed, arena, kArenaFloats, kOffset, film, options)) {
            return false;
        }
        outJ = filmTotal(film);
        return true;
    };

    // THE VERTEX. Component 1 (y) of vertex 0 -- a corner of the box the
    // interior checks trace. Which one matters less than that it is a real
    // vertex of the scene those checks depend on, so the contrast measured
    // below is against the same geometry.
    constexpr std::size_t kVertexFloat = 1u;
    if (positions.size() <= kVertexFloat) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 56 -- the scene has too few floats\n");
        wf.destroy(ctx.allocator());
        arena.destroy(ctx.allocator());
        return false;
    }

    std::vector<float> plus = positions;
    std::vector<float> minus = positions;
    plus[kVertexFloat] += kStep;
    minus[kVertexFloat] -= kStep;

    double jCentre = 0.0, jPlus = 0.0, jMinus = 0.0;
    std::vector<float> traceCentre, tracePlus, traceMinus;
    const bool ok = render(positions, jCentre, traceCentre) &&
                    render(plus, jPlus, tracePlus) && render(minus, jMinus, traceMinus);
    wf.destroy(ctx.allocator());
    arena.destroy(ctx.allocator());
    if (!ok) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 56 dispatch\n");
        return false;
    }

    const double hActual = 0.5 * (static_cast<double>(plus[kVertexFloat]) -
                                  static_cast<double>(minus[kVertexFloat]));
    const double fd = (jPlus - jMinus) / (2.0 * hActual);

    // THE MEASUREMENT THIS CHECK EXISTS FOR. For every parameter Stages 1-2
    // differentiate, a +/-h perturbation leaves the PATHS untouched -- checks
    // 37 and 42 assert exactly 0 trace mismatches, and that is what makes
    // their finite difference the derivative of ONE realisation of the
    // estimator rather than a difference of two Monte Carlo means. A VERTEX
    // is the first parameter for which that is false by construction: moving
    // geometry moves every ray that hits it.
    const std::size_t mismatchPlus = traceGeometryMismatches(tracePlus, traceCentre, kCapacity);
    const std::size_t mismatchMinus = traceGeometryMismatches(traceMinus, traceCentre, kCapacity);
    const std::size_t mismatches = mismatchPlus + mismatchMinus;

    if (!(jCentre > 0.0) || !std::isfinite(fd)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 56 -- J = %.9g and the finite difference is "
                     "%.9g. Both must be finite and J strictly positive, or the scene is not "
                     "lit and nothing below means anything\n",
                     jCentre, fd);
        return false;
    }
    // NON-VACUITY: the geometry must MATTER. A vertex whose motion left J
    // unchanged would make the mismatch count below the only content, and a
    // zero derivative is what a boundary term would have to reproduce
    // trivially.
    if (!(std::fabs(fd) > 1.0)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 56 -- dJ/d(vertex) is %.9g, indistinguishable "
                     "from zero against a J of %.9g. Moving a corner of the box the camera is "
                     "inside must change the image; if it does not, either the acceleration "
                     "structure is not being rebuilt from the perturbed positions or the "
                     "perturbed vertex is not one this view can see\n",
                     fd, jCentre);
        return false;
    }
    // THE CONTRAST, ASSERTED. If a vertex perturbation left the paths alone,
    // this check would be measuring the same kind of quantity checks 37-49
    // do, and Stage 3 would not need to exist.
    // EACH SIDE SEPARATELY, not their sum. Asserting only the total was a
    // weaker check than it looked: a demonstration that perturbed the -h
    // render and left +h at the centre geometry still produced a nonzero
    // total, a nonzero derivative, and a PASS -- while computing a one-sided
    // difference scaled as though it were central, i.e. wrong by a factor of
    // two with nothing complaining. Requiring both sides to move closes that.
    if (mismatchPlus == 0u || mismatchMinus == 0u) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 56 -- the +h render differs from the centre "
                     "in %zu trace slots and the -h render in %zu, and BOTH must be nonzero. A "
                     "zero on one side means that render used the UNPERTURBED geometry, which "
                     "makes the quotient below a one-sided difference divided by 2h -- wrong by "
                     "a factor of two, and large and plausible enough that neither the "
                     "magnitude assertion above nor a total-mismatch count would notice\n",
                     mismatchPlus, mismatchMinus);
        return false;
    }
    if (mismatches == 0u) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 56 -- moving a vertex by %.9g left ALL %u "
                     "trace slots identical. That cannot be right for geometry that the camera "
                     "sees: it means the acceleration structure was built once and reused "
                     "across the three renders, so the finite difference above is measuring "
                     "nothing about geometry at all\n",
                     static_cast<double>(kStep), kCapacity * 18u);
        return false;
    }

    std::printf(
        "[diff_gpu_probe] OK: check 56 -- dJ/d(VERTEX POSITION), MEASURED, and the measurement "
        "is why Stage 3 exists. Perturbing float %zu of the scene's positions by +/-%.9g gives "
        "a central difference of %.6g against J = %.9g -- the geometry plainly matters. WHAT IS "
        "DIFFERENT FROM EVERY PARAMETER BEFORE IT: %zu vertex-trace slots differ between the "
        "perturbed renders and the centre one (%zu at +h, %zu at -h), where checks 37 and 42 "
        "assert EXACTLY ZERO for the albedo and the emission. Those parameters do not move the "
        "paths, which is what makes their finite difference the derivative of ONE realisation "
        "of the estimator and their tolerance pure arithmetic. A vertex moves the geometry every "
        "ray hits, so this difference is NOT that -- it is a difference across a discontinuity, "
        "and the boundary term of spec 4.1 is exactly what accounts for it. The acceleration "
        "structure is rebuilt from the perturbed positions on every call, which this nonzero "
        "count also confirms: a structure built once and reused would leave every trace slot "
        "identical.\n",
        kVertexFloat, static_cast<double>(kStep), fd, jCentre, mismatches, mismatchPlus,
        mismatchMinus);
    return true;
}

}  // namespace ohao::diff::probe
