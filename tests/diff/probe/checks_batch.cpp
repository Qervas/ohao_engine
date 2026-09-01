// Stage 2 Task 4, check 53: a multi-view batch accumulates.
#include "probe/checks_batch.hpp"

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

bool checkMultiViewBatch(ohao::diff::GpuProbeContext& ctx) {
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 8;
    constexpr uint32_t kCapacity = kW * kH;
    constexpr uint32_t kEnvW = 64;
    constexpr uint32_t kEnvH = 32;
    constexpr float kAlbedo = 0.6f;
    constexpr uint32_t kGradientSeed = 20260828u;
    constexpr uint32_t kBounces = 3u;
    constexpr double kRelTol = 1e-5;

    ohao::diff::ParamRegistry reg;
    const auto regAlbedo = reg.registerScalarBlock("albedo", 1);
    if (!regAlbedo.ok) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 53 registry setup: %s\n",
                     regAlbedo.error.c_str());
        return false;
    }
    const ohao::diff::DiffParam* albedoParam = reg.find("albedo");
    if (albedoParam == nullptr) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 53 registered param not found\n");
        return false;
    }
    ohao::diff::GradientArena arena;
    if (!arena.build(ctx.allocator(), reg.layout())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 53 gradient arena build\n");
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

    // TWO VIEWS. The second looks along a different axis from a different
    // height, so it sees different geometry at different angles -- the check
    // below requires their gradients to DIFFER, which is what makes "the
    // batch is the sum" a statement about two things rather than one thing
    // counted twice.
    const ohao::diff::WavefrontGenerateCamera viewA = parityCamera();
    ohao::diff::WavefrontGenerateCamera viewB = parityCamera();
    // ROTATED, not merely moved. Moving the camera inside a closed box under
    // a near-uniform environment changes the total radiance almost not at all
    // -- a first attempt that only shifted the origin and widened the fov gave
    // two gradients 0.17%% apart, and the non-vacuity assertion below
    // correctly refused it. Looking along a different AXIS puts different
    // walls at different angles, which is what actually makes the two views
    // two measurements.
    viewB.origin[0] = 1.0f;
    viewB.origin[1] = 1.0f;
    viewB.origin[2] = -1.5f;
    viewB.forward[0] = 1.0f;
    viewB.forward[1] = 0.0f;
    viewB.forward[2] = 0.0f;
    viewB.right[0] = 0.0f;
    viewB.right[1] = 0.0f;
    viewB.right[2] = 1.0f;
    viewB.up[0] = 0.0f;
    viewB.up[1] = 1.0f;
    viewB.up[2] = 0.0f;
    viewB.tanHalfFov = 0.6f;

    ohao::EnvCDF cdf;
    cdf.build(envRgba, static_cast<int>(kEnvW), static_cast<int>(kEnvH));
    if (!cdf.valid()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 53 EnvCDF::build produced no CDF\n");
        arena.destroy(ctx.allocator());
        return false;
    }
    ohao::diff::WavefrontBuffers wf;
    if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH) ||
        !wf.uploadEnvironment(cdf.marginalSpan(), cdf.conditionalSpan(), cdf.integral())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 53 buffers build / env CDF upload\n");
        wf.destroy(ctx.allocator());
        arena.destroy(ctx.allocator());
        return false;
    }

    const ohao::diff::WavefrontScatterMaterial kMaterial{1.0f, 0.0f, 0.0f};

    auto run = [&](const ohao::diff::WavefrontGenerateCamera& camera, bool accumulate,
                   double& out) -> bool {
        ohao::diff::WavefrontGradientOptions options;
        options.diffParam = 0u;  // DIFF_PARAM_BASECOLOR
        options.accumulate = accumulate;
        std::vector<float> film;
        if (!ctx.runWavefrontGradientProbe(wf, kW, kH, kBounces, camera,
                                           std::span<const float>(positions),
                                           std::span<const uint32_t>(indices), kAlbedo, kMaterial,
                                           kGradientSeed, arena, kArenaFloats, kOffset, film,
                                           options)) {
            return false;
        }
        const std::vector<float> block = arena.readback(ctx.allocator(), albedoParam->gradBlock);
        if (block.empty()) return false;
        out = static_cast<double>(block[0]);
        return true;
    };

    double gA = 0.0;
    double gB = 0.0;
    double gBatch = 0.0;
    bool dispatched = run(viewA, /*accumulate=*/false, gA) &&
                      run(viewB, /*accumulate=*/false, gB);
    // THE BATCH: view A into a cleared arena, then view B into the SAME
    // arena without clearing. One iteration, two views -- spec 4.4.
    if (dispatched) {
        double ignored = 0.0;
        dispatched = run(viewA, /*accumulate=*/false, ignored) &&
                     run(viewB, /*accumulate=*/true, gBatch);
    }
    wf.destroy(ctx.allocator());
    arena.destroy(ctx.allocator());
    if (!dispatched) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 53 dispatch\n");
        return false;
    }

    const double sum = gA + gB;
    const double tol = kRelTol * std::fabs(sum);

    // --- NON-VACUITY 1: both views see something.
    if (!(gA > 0.0) || !(gB > 0.0) || !std::isfinite(gA) || !std::isfinite(gB)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 53 -- the two views give %.9g and %.9g, and "
                     "both must be finite and strictly positive or the sum below is not a sum "
                     "of two things\n",
                     gA, gB);
        return false;
    }

    // --- NON-VACUITY 2: THE VIEWS DIFFER. Two identical views would make
    // the sum 2*gA, which a batch would also produce -- and the check would
    // pass while testing nothing about accumulating DIFFERENT contributions.
    if (!(std::fabs(gA - gB) > 0.01 * std::fabs(gA))) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 53 -- the two views give %.9g and %.9g, "
                     "differing by less than 1%%. They are meant to see the scene from "
                     "different places; if they agree this closely the batch is accumulating "
                     "one contribution twice and the identity below would hold for a reason "
                     "that has nothing to do with batching\n",
                     gA, gB);
        return false;
    }

    // --- THE FAILURE MODE THE SPEC NAMES, ASSERTED DIRECTLY. A clear that
    // ran per VIEW instead of per ITERATION reduces the batch to its LAST
    // view exactly -- and the resulting gradient is entirely plausible, being
    // a real gradient of a real view. Nothing but this comparison would
    // notice.
    if (!(std::fabs(gBatch - gB) > tol)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 53 -- the two-view batch gives %.12g and the "
                     "SECOND VIEW ALONE gives %.12g, and they agree. That is precisely what a "
                     "clear running per VIEW rather than per ITERATION produces: the batch is "
                     "reduced to its last view, and the gradient still looks entirely plausible "
                     "because it IS a real gradient -- of one view instead of two. Spec 4.4 "
                     "says the arena is cleared per iteration\n",
                     gBatch, gB);
        return false;
    }

    // --- THE IDENTITY.
    const double err = std::fabs(gBatch - sum);
    if (!(err <= tol)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 53 -- A BATCH IS NOT THE SUM OF ITS VIEWS.\n"
                     "  view A alone      = %.12g\n"
                     "  view B alone      = %.12g\n"
                     "  their sum         = %.12g\n"
                     "  A and B batched   = %.12g\n"
                     "  |difference| = %.6g, above the %.3g x |sum| = %.6g floor\n"
                     "  dL/dtheta is a SUM over paths and the arena is an atomicAdd "
                     "accumulator, so accumulating two views into one arena and adding two "
                     "separately-accumulated arenas are the same arithmetic in a different "
                     "order. Only the float32 atomic order distinguishes them, which is what "
                     "the floor covers.\n",
                     gA, gB, sum, gBatch, err, kRelTol, tol);
        return false;
    }

    std::printf(
        "[diff_gpu_probe] OK: check 53 -- A MULTI-VIEW BATCH IS THE SUM OF ITS VIEWS. Two "
        "cameras over one scene: %.9g and %.9g separately, %.9g accumulated into one arena "
        "against a sum of %.9g, agreeing to %.3g of a %.3g x |sum| floor. The arena is cleared "
        "per ITERATION and not per view (spec 4.4), and the check asserts the failure that "
        "would cause DIRECTLY: a per-view clear reduces the batch to its LAST view, whose "
        "gradient is entirely plausible because it is a real gradient -- of one view instead of "
        "two -- so the batch is separately required NOT to equal view B alone. The two views "
        "are also required to differ by more than 1%%, without which the sum would be 2*gA and "
        "the identity would hold while testing nothing about batching. Seed %u, %u paths per "
        "view at ONE SAMPLE PER PIXEL, %u bounces -- see WavefrontLoop::Config's header for why "
        "1 spp per dispatch is the resolution of spec 4.5's film hazard, and for what it does "
        "NOT make deterministic.\n",
        gA, gB, gBatch, sum, err / tol, kRelTol, kGradientSeed, kCapacity, kBounces);
    return true;
}

}  // namespace ohao::diff::probe
