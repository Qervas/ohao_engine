// Stage 2 Task 1, check 50. See the header for why the oracle is a partition
// identity rather than a finite difference.
#include "probe/checks_adjoint_seed.hpp"

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

bool checkAdjointSeed(ohao::diff::GpuProbeContext& ctx) {
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 8;  // wf_generate's 1-D dispatch requires this exactly.
    constexpr uint32_t kCapacity = kW * kH;
    constexpr uint32_t kEnvW = 64;
    constexpr uint32_t kEnvH = 32;
    // Check 37's scene, seed and material, so this measures the same
    // gradient that check is about and a disagreement cannot be blamed on a
    // different configuration.
    constexpr float kAlbedo = 0.6f;
    constexpr uint32_t kGradientSeed = 20260828u;
    constexpr uint32_t kBounces = 3u;
    // The arena's float atomicAdd is non-associative, so three runs do not
    // agree bit for bit. This is the same pre-registered relative floor the
    // Task 6 convergence gates use, and for the same reason.
    constexpr double kRelTol = 1e-5;

    ohao::diff::ParamRegistry reg;
    const auto regAlbedo = reg.registerScalarBlock("albedo", 1);
    if (!regAlbedo.ok) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 50 registry setup: %s\n",
                     regAlbedo.error.c_str());
        return false;
    }
    const ohao::diff::DiffParam* albedoParam = reg.find("albedo");
    if (albedoParam == nullptr) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 50 registered param not found\n");
        return false;
    }
    ohao::diff::GradientArena arena;
    if (!arena.build(ctx.allocator(), reg.layout())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 50 gradient arena build\n");
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
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 50 EnvCDF::build produced no CDF\n");
        arena.destroy(ctx.allocator());
        return false;
    }
    ohao::diff::WavefrontBuffers wf;
    if (!wf.build(ctx.allocator(), kCapacity, kEnvW, kEnvH) ||
        !wf.uploadEnvironment(cdf.marginalSpan(), cdf.conditionalSpan(), cdf.integral())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 50 buffers build / env CDF upload\n");
        wf.destroy(ctx.allocator());
        arena.destroy(ctx.allocator());
        return false;
    }

    const ohao::diff::WavefrontScatterMaterial kMaterial{1.0f, 0.0f, 0.0f};
    const std::size_t kSeedFloats = static_cast<std::size_t>(kCapacity) * 3u;
    // THE SPLIT: the first half of the pixels against the second. A split by
    // pixel INDEX and not by value, so which pixels land on which side does
    // not depend on the render -- a value-dependent split would make the two
    // sides' sum trivially the whole for the wrong reason.
    const uint32_t kHalf = kCapacity / 2u;

    std::vector<float> seedOnes(kSeedFloats, 1.0f);
    std::vector<float> seedLeft(kSeedFloats, 0.0f);
    std::vector<float> seedRight(kSeedFloats, 0.0f);
    for (uint32_t p = 0; p < kCapacity; ++p) {
        std::vector<float>& dst = (p < kHalf) ? seedLeft : seedRight;
        for (uint32_t c = 0; c < 3u; ++c) dst[static_cast<std::size_t>(p) * 3u + c] = 1.0f;
    }

    struct Run {
        const char* name;
        const std::vector<float>* seed;  // nullptr = empty options.adjointSeed
        double gradient;
    };
    std::vector<float> empty;
    Run runs[4] = {
        {"no seed bound (the sum-of-film objective)", nullptr, 0.0},
        {"an explicit all-ones seed", &seedOnes, 0.0},
        {"the first half of the pixels", &seedLeft, 0.0},
        {"the second half", &seedRight, 0.0},
    };

    bool ok = true;
    for (Run& r : runs) {
        ohao::diff::WavefrontGradientOptions options;
        options.diffParam = 0u;  // DIFF_PARAM_BASECOLOR
        if (r.seed != nullptr) options.adjointSeed = *r.seed;
        std::vector<float> film;
        // runWavefrontGradientProbe zeroes the arena itself -- check 38 depends
        // on that, since it reads the whole arena back and requires every float
        // but one to be exactly 0 after a run.
        if (!ctx.runWavefrontGradientProbe(wf, kW, kH, kBounces, camera,
                                           std::span<const float>(positions),
                                           std::span<const uint32_t>(indices), kAlbedo, kMaterial,
                                           kGradientSeed, arena, kArenaFloats, kOffset, film,
                                           options)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 50 dispatch failed for %s\n",
                         r.name);
            wf.destroy(ctx.allocator());
            arena.destroy(ctx.allocator());
            return false;
        }
        const std::vector<float> block = arena.readback(ctx.allocator(), albedoParam->gradBlock);
        if (block.empty()) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 50 arena readback empty for %s\n",
                         r.name);
            wf.destroy(ctx.allocator());
            arena.destroy(ctx.allocator());
            return false;
        }
        r.gradient = static_cast<double>(block[0]);
    }

    const double gAll = runs[0].gradient;
    const double gOnes = runs[1].gradient;
    const double gLeft = runs[2].gradient;
    const double gRight = runs[3].gradient;
    const double tol = kRelTol * std::fabs(gAll);

    // --- NON-VACUITY 1: there is a gradient at all.
    if (!(gAll > 0.0) || !std::isfinite(gAll)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 50 -- the unseeded gradient is %.9g. Every "
                     "factor is non-negative and the scene is lit, so a zero or non-finite "
                     "value here means nothing was accumulated and every comparison below is "
                     "0 against 0\n",
                     gAll);
        ok = false;
    }

    // --- EMPTY MEANS ONES. Not a convenience: it is the claim that the
    // sum-of-film objective every Stage 1 check measures is the SPECIAL CASE
    // w = 1 of this one, rather than a separate code path beside it.
    if (ok && !(std::fabs(gOnes - gAll) <= tol)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 50 -- an EMPTY seed gives %.12g and an "
                     "explicit all-ones seed gives %.12g, differing by %.6g against a %.6g "
                     "floor. These must be the same number: 'no seed bound' is not a disabled "
                     "state, it is dL/d(film) = 1, which is what J being the sum of the film "
                     "means. If they differ, the two are separate code paths and every Stage 1 "
                     "check measures the one a loss will not use\n",
                     gAll, gOnes, std::fabs(gOnes - gAll), tol);
        ok = false;
    }

    // --- NON-VACUITY 2: THE SEED SELECTS. Without this the identity below
    // is satisfiable by a shader that ignores the seed entirely -- it would
    // return gAll three times, and gAll + gAll = gAll is false, but a shader
    // returning ZERO for every seeded run would satisfy the identity while
    // measuring nothing. Both halves must be strictly inside (0, gAll).
    if (ok && !(gLeft > 0.0 && gRight > 0.0 && gLeft < gAll && gRight < gAll)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 50 -- the halves are %.12g and %.12g against "
                     "a whole of %.12g, and each must lie STRICTLY between 0 and the whole. A "
                     "half equal to the whole means the seed is being ignored; a half equal to "
                     "zero means it is being read as all-zero, and either way the partition "
                     "identity below would hold for a reason that has nothing to do with "
                     "dL/dpixel reaching the adjoint\n",
                     gLeft, gRight, gAll);
        ok = false;
    }

    // --- THE IDENTITY.
    const double sum = gLeft + gRight;
    const double err = std::fabs(sum - gAll);
    if (ok && !(err <= tol)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 50 -- dL/dtheta IS NOT LINEAR IN THE SEED.\n"
                     "  first half  = %.12g\n"
                     "  second half = %.12g\n"
                     "  their sum   = %.12g\n"
                     "  whole       = %.12g\n"
                     "  |difference| = %.6g, above the %.3g x |g| = %.6g floor\n"
                     "  The two halves are disjoint and together cover every pixel, and\n"
                     "  dL/dtheta = SUM_p SUM_c w[p][c] * dI/dtheta is linear in w, so this is\n"
                     "  an algebraic identity and not a numerical agreement. It holds whatever\n"
                     "  the gradient's VALUE is -- which is why it tests the seed rather than\n"
                     "  the gradient, and why checks 37-49 are what test the gradient.\n"
                     "  A sum of twice the whole means the seed is ignored. Anything else\n"
                     "  means a pixel is being counted in neither half or in both, so the\n"
                     "  seed's index does not agree with the film's.\n",
                     gLeft, gRight, sum, gAll, err, kRelTol, tol);
        ok = false;
    }

    wf.destroy(ctx.allocator());
    arena.destroy(ctx.allocator());
    if (!ok) return false;

    std::printf(
        "[diff_gpu_probe] OK: check 50 -- THE ADJOINT SEED IS dL/d(film), and the sum-of-film "
        "objective every earlier gradient check measures is its SPECIAL CASE w = 1 rather than "
        "a separate path beside it: an empty seed and an explicit all-ones seed give %.9g and "
        "%.9g, agreeing to %.3g. THE ORACLE IS AN ALGEBRAIC IDENTITY, NOT A FINITE DIFFERENCE: "
        "dL/dtheta = SUM_p SUM_c w[p][c] * dI/dtheta is linear in w, so two disjoint halves of "
        "the film that together cover it must sum to the whole -- %.9g + %.9g = %.9g against "
        "%.9g, to %.3g of a %.3g x |g| floor. No step size, no truncation term. It holds "
        "whatever the gradient's value is, which is exactly why it tests the SEED; checks 37-49 "
        "test the gradient. The halves are also required to lie STRICTLY inside (0, whole), "
        "without which a shader that ignored the seed and one that read it as all-zero would "
        "both satisfy the identity. Seed %u, %u paths at one sample per pixel, %u bounces, "
        "split at pixel index %u.\n",
        gAll, gOnes, std::fabs(gOnes - gAll), gLeft, gRight, sum, gAll, err / tol, kRelTol,
        kGradientSeed, kCapacity, kBounces, kHalf);
    return true;
}

}  // namespace ohao::diff::probe
