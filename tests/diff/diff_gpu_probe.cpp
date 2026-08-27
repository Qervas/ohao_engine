// Standalone GPU probe for the differentiable renderer scaffolding.
// Requires a working Vulkan device. Returns 0 on success.
//
// Checks:
//   1. GradientArena allocates, zeroes, and reads back.
//   2. atomicAdd on a float SSBO accumulates correctly under contention.
//   3. rayQueryEXT visibility matches a closed-form plane intersection.

#include "gpu_probe_context.hpp"

#include "diff/device_caps.hpp"
#include "diff/grad/arena_layout.hpp"
#include "diff/grad/gradient_arena.hpp"

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
    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 64;
    constexpr float kPlaneDistance = 2.0f;
    constexpr float kTanHalfFov = 0.2f;  // narrow, so every ray hits the quad

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
    if (std::fabs(hitsT[centre] - kPlaneDistance) > 1e-3f) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: centre ray t = %f, expected %f\n",
                     hitsT[centre], kPlaneDistance);
        return 1;
    }

    // Off-axis rays are longer by exactly 1/cos(theta), and every ray must hit.
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
            const float dx = ndcX * kTanHalfFov;  // aspect is 1
            const float dy = ndcY * kTanHalfFov;
            const float expected = kPlaneDistance * std::sqrt(1.0f + dx * dx + dy * dy);
            if (std::fabs(hitsT[i] - expected) > 2e-3f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: pixel (%u,%u) t = %f, closed form %f\n",
                             x, y, hitsT[i], expected);
                return 1;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: ray query matches closed-form plane intersection "
                "over %u pixels\n", kW * kH);

    arena.destroy(ctx.allocator());
    ctx.shutdown();
    std::printf("[diff_gpu_probe] PASS\n");
    return 0;
}
