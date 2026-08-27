// Standalone GPU probe for the differentiable renderer scaffolding.
// Requires a working Vulkan device. Returns 0 on success.
//
// Checks:
//   1. GradientArena allocates, zeroes, and reads back.
//   2. atomicAdd on a float SSBO accumulates correctly under contention.
//   3. rayQueryEXT visibility matches a closed-form plane intersection.
//   4. A half-quad's hit/miss pattern pins the camera's Y orientation --
//      check 3's distance formula is even in dy, so it can't catch a flipped
//      up-vector or NDC-Y sign on its own.

#include "gpu_probe_context.hpp"

#include "diff/device_caps.hpp"
#include "diff/grad/arena_layout.hpp"
#include "diff/grad/gradient_arena.hpp"
#include "diff/param/param_registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main() {
    ohao::diff::GpuProbeContext ctx;
    if (!ctx.init()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: could not initialise Vulkan\n");
        return 1;
    }

    const ohao::diff::DeviceCaps caps = ohao::diff::queryDeviceCaps(ctx.physicalDevice());
    if (!caps.sufficient()) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: device lacks ray query or float atomics\n");
        return 1;
    }

    ohao::diff::ArenaLayout layout;
    const std::size_t blockA = layout.add(16);
    const std::size_t blockB = layout.add(4);

    ohao::diff::GradientArena arena;
    if (!arena.build(ctx.allocator(), layout)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: arena build\n");
        return 1;
    }

    // 1. zero + readback
    ctx.runImmediate([&](VkCommandBuffer cmd) { arena.zero(cmd); });
    for (std::size_t b : {blockA, blockB}) {
        const std::vector<float> values = arena.readback(ctx.allocator(), b);
        if (values.empty()) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: block %zu readback returned no data "
                         "(this check would otherwise pass having verified nothing)\n",
                         b);
            return 1;
        }
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (values[i] != 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: block %zu element %zu = %f, expected 0\n",
                             b, i, values[i]);
                return 1;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: arena zero + readback\n");

    // 2. float atomics under contention
    constexpr uint32_t kInvocations = 4096;
    if (!ctx.runAtomicProbe(arena, /*targetIndex=*/0, kInvocations)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: atomic probe dispatch\n");
        return 1;
    }
    const std::vector<float> after = arena.readback(ctx.allocator(), blockA);
    if (after.size() < 2) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: block %zu readback returned %zu floats, expected "
                     "at least 2 (a readback regression here would otherwise be undefined "
                     "behaviour, not a caught failure)\n",
                     blockA, after.size());
        return 1;
    }
    if (after[0] != static_cast<float>(kInvocations)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: atomicAdd gave %f, expected %u "
                     "(lost updates = non-atomic accumulation)\n",
                     after[0], kInvocations);
        return 1;
    }
    if (after[1] != 0.0f) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: atomicAdd wrote outside target index\n");
        return 1;
    }
    std::printf("[diff_gpu_probe] OK: atomicAdd accumulated %u contended adds exactly\n",
                kInvocations);

    // 3. Ray-query visibility against a plane whose intersections are analytic.
    //
    // kW != kH deliberately: a square resolution makes the closed form
    // (even in both dx and dy, see check 4's comment) blind to a transposed
    // pixel index, and leaves the ndcX*aspect term in the shader untested
    // since aspect == 1 would hide a missing/backwards aspect multiply.
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 48;
    constexpr float kPlaneDistance = 2.0f;
    constexpr float kTanHalfFov = 0.2f;  // narrow, so every ray hits the quad
    constexpr float kAspect = static_cast<float>(kW) / static_cast<float>(kH);
    // Widened horizontal extent still lands inside the +-1 quad:
    // kTanHalfFov * kAspect ~= 0.267, so max |x| at z=-2 is ~0.53.

    std::vector<float> hitsT;
    if (!ctx.runVisibilityProbe(kPlaneDistance, kW, kH, kTanHalfFov, hitsT)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: visibility probe dispatch\n");
        return 1;
    }
    if (hitsT.size() != static_cast<std::size_t>(kW) * kH) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: hit buffer size %zu, expected %u\n",
                     hitsT.size(), kW * kH);
        return 1;
    }

    // The centre pixel looks straight down -Z, so t is exactly the plane distance.
    const std::size_t centre = static_cast<std::size_t>(kH / 2) * kW + (kW / 2);
    if (std::fabs(hitsT[centre] - kPlaneDistance) > 1e-4f) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: centre ray t = %f, expected %f\n",
                     hitsT[centre], kPlaneDistance);
        return 1;
    }

    // Off-axis rays are longer by exactly 1/cos(theta), and every ray must hit.
    // Tolerance is 1e-4: float32 at t~=2 supports ~1e-5, so this still leaves
    // ~10x headroom over float noise while catching e.g. a dropped +0.5
    // pixel-centre offset (worst-case error ~1.19e-3 at this FOV, comfortably
    // above 1e-4). Do not widen this to make a failure pass -- report the
    // actual max error and stop.
    float maxAbsError = 0.0f;
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * kW + x;
            if (hitsT[i] < 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: pixel (%u,%u) missed a quad that "
                             "covers the whole frustum\n", x, y);
                return 1;
            }
            const float ndcX = 2.0f * (static_cast<float>(x) + 0.5f) / kW - 1.0f;
            const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / kH;
            const float dx = ndcX * kAspect * kTanHalfFov;
            const float dy = ndcY * kTanHalfFov;
            const float expected = kPlaneDistance * std::sqrt(1.0f + dx * dx + dy * dy);
            const float err = std::fabs(hitsT[i] - expected);
            maxAbsError = std::max(maxAbsError, err);
            if (err > 1e-4f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: pixel (%u,%u) t = %f, closed form %f "
                             "(|err| = %g)\n",
                             x, y, hitsT[i], expected, err);
                return 1;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: ray query matches closed-form plane intersection "
                "over %u pixels (max |err| = %g)\n", kW * kH, maxAbsError);

    // 4. A half-quad (y in [0,1] only) makes the hit/miss pattern asymmetric
    // in Y. The closed form sqrt(1+dx^2+dy^2) above is even in dy, so a
    // flipped up-vector or a flipped NDC-Y sign is invisible to it -- every
    // ray still lands at the "right" distance from *some* consistent-looking
    // convention, even a backwards one. This check pins the actual convention
    // Stage 0b's integrator inherits: ndcY > 0 for y < kH/2, i.e. the top
    // half of the image must hit and the bottom half must miss.
    std::vector<float> halfHits;
    if (!ctx.runVisibilityProbe(kPlaneDistance, kW, kH, kTanHalfFov, halfHits,
                                /*quadMinY=*/0.0f)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: half-quad visibility probe dispatch\n");
        return 1;
    }
    if (halfHits.size() != static_cast<std::size_t>(kW) * kH) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: half-quad hit buffer size %zu, expected %u\n",
                     halfHits.size(), kW * kH);
        return 1;
    }
    for (uint32_t y = 0; y < kH; ++y) {
        const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(kH);
        const bool expectHit = (ndcY > 0.0f);
        for (uint32_t x = 0; x < kW; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * kW + x;
            const bool didHit = (halfHits[i] >= 0.0f);
            if (didHit != expectHit) {
                std::fprintf(stderr,
                    "[diff_gpu_probe] FAIL: half-quad orientation at (%u,%u): expected %s, got %s "
                    "(camera up-vector or NDC-Y sign is inverted)\n",
                    x, y, expectHit ? "hit" : "miss", didHit ? "hit" : "miss");
                return 1;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: half-quad pins camera Y orientation over %u pixels\n",
                kW * kH);

    // 4. The seam Stage 1 depends on most: a block index handed out by the registry
    //    must resolve correctly against an arena built from that registry's layout.
    //    Both hold ArenaLayout by value, so this proves the positional indices survive
    //    the copy -- previously true only by inspection.
    {
        ohao::diff::ParamRegistry reg;
        const auto reg1 = reg.registerTexture("albedo", {8, 8, 3}, VK_FORMAT_R32G32B32A32_SFLOAT);
        const auto reg2 = reg.registerScalarBlock("ssao_params", 4);
        if (!reg1.ok || !reg2.ok) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: registry setup: %s %s\n",
                         reg1.error.c_str(), reg2.error.c_str());
            return 1;
        }

        ohao::diff::GradientArena regArena;
        if (!regArena.build(ctx.allocator(), reg.layout())) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: arena build from registry layout\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { regArena.zero(cmd); });

        const ohao::diff::DiffParam* albedo = reg.find("albedo");
        const ohao::diff::DiffParam* ssao = reg.find("ssao_params");
        if (albedo == nullptr || ssao == nullptr) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: registered params not found\n");
            regArena.destroy(ctx.allocator());
            return 1;
        }

        struct Expect { const char* name; std::size_t block; std::size_t floats; };
        const Expect expects[] = {
            {"albedo.grad",  albedo->gradBlock,  albedo->floatCount},
            {"albedo.state", albedo->stateBlock, albedo->floatCount * 2u},
            {"ssao.grad",    ssao->gradBlock,    ssao->floatCount},
            {"ssao.state",   ssao->stateBlock,   ssao->floatCount * 2u},
        };
        for (const Expect& e : expects) {
            const std::vector<float> block = regArena.readback(ctx.allocator(), e.block);
            if (block.size() != e.floats) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: %s resolved to %zu floats, registry says %zu "
                             "(registry/arena block indices disagree)\n",
                             e.name, block.size(), e.floats);
                regArena.destroy(ctx.allocator());
                return 1;
            }
            for (float v : block) {
                if (v != 0.0f) {
                    std::fprintf(stderr, "[diff_gpu_probe] FAIL: %s not zeroed\n", e.name);
                    regArena.destroy(ctx.allocator());
                    return 1;
                }
            }
        }
        regArena.destroy(ctx.allocator());
        std::printf("[diff_gpu_probe] OK: registry block indices resolve against arena "
                    "(4 blocks, sizes match)\n");
    }

    arena.destroy(ctx.allocator());
    ctx.shutdown();
    std::printf("[diff_gpu_probe] PASS\n");
    return 0;
}
