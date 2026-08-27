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
//   ...
//   8. wf_generate.comp's generated rays reproduce check 3's closed form.
//   9. Every PathStateField wf_generate.comp writes round-trips exactly.
//   10. wf_generate.comp's queue/counter population has no atomicAdd races.
//   11. path_state_layout.hpp and path_state.glsl agree field-by-field.
//   12. wf_intersect.comp compacts survivors of a known-fraction scene into
//       queue ring 1 exactly (no duplicates, no dead paths), across an
//       indirectly-sized dispatch prepared by wf_prepare_indirect.comp.
//   13. An indirect dispatch sized from a live-count of 0 launches zero
//       invocations -- dead paths are genuinely free.

#include "gpu_probe_context.hpp"

#include "diff/device_caps.hpp"
#include "diff/grad/arena_layout.hpp"
#include "diff/grad/gradient_arena.hpp"
#include "diff/rng/diff_rng.hpp"
#include "diff/param/param_registry.hpp"
#include "diff/wavefront/wavefront_buffers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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


    // 6. CPU/GPU RNG parity -- the invariant path replay backpropagation rests on.
    //    The backward pass stores no tape; it replays each light path from its seed.
    //    If shaders/includes/diff/rng.glsl and ohao/diff/rng/diff_rng.cpp disagree by
    //    a single bit, the replayed path is a DIFFERENT path and every gradient is
    //    silently wrong -- no crash, no NaN. Until now the two were verified
    //    identical only by reading them side by side.
    {
        constexpr uint32_t kDraws = 64;
        constexpr uint32_t kPixel = 4096;
        constexpr uint32_t kSample = 3;
        constexpr uint32_t kSeed = 12345;

        ohao::diff::ArenaLayout rngLayout;
        const std::size_t drawBlock = rngLayout.add(kDraws);
        ohao::diff::GradientArena rngArena;
        if (!rngArena.build(ctx.allocator(), rngLayout)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: rng arena build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { rngArena.zero(cmd); });

        std::vector<float> gpuDraws;
        if (!ctx.runRngParityProbe(kPixel, kSample, kSeed, kDraws, rngArena, drawBlock, gpuDraws)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: rng parity dispatch\n");
            rngArena.destroy(ctx.allocator());
            return 1;
        }

        ohao::diff::PathRng cpuRng = ohao::diff::PathRng::forPath(kPixel, kSample, kSeed);
        for (uint32_t i = 0; i < kDraws; ++i) {
            const float cpu = cpuRng.next1D();
            const float gpu = gpuDraws[i];
            // Bit-exact on purpose: an epsilon here would defeat the entire check.
            if (cpu != gpu) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: RNG diverges at draw %u: CPU %.9g, GPU %.9g\n"
                             "  shaders/includes/diff/rng.glsl and ohao/diff/rng/diff_rng.cpp must\n"
                             "  produce identical sequences -- path replay depends on it\n",
                             i, static_cast<double>(cpu), static_cast<double>(gpu));
                rngArena.destroy(ctx.allocator());
                return 1;
            }
        }
        if (cpuRng.drawCount() != kDraws) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: drawCount %u, expected %u\n",
                         cpuRng.drawCount(), kDraws);
            rngArena.destroy(ctx.allocator());
            return 1;
        }
        rngArena.destroy(ctx.allocator());
        std::printf("[diff_gpu_probe] OK: CPU and GLSL RNG agree bit-exactly over %u draws\n",
                    kDraws);
    }

    // 7. Wavefront path-state/queue/counter buffers: build, zero, and confirm
    //    every one of the 16 PathStateFields and both counter slots come
    //    back zeroed at exactly the expected element count. This is the
    //    substrate every bounce dispatch in the wavefront integrator reads
    //    and writes through -- state cannot live in registers across a
    //    dispatch boundary.
    {
        constexpr std::uint32_t kCapacity = 4096;
        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wavefront buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        for (std::uint32_t i = 0; i < ohao::diff::PathStateLayout::kFieldCount; ++i) {
            const auto field = static_cast<ohao::diff::PathStateField>(i);
            const std::vector<float> values = wf.readbackField(ctx.allocator(), field);
            // Explicit size check (not just empty()) guards against a vacuous
            // pass: a readback regression that silently truncated to a
            // smaller-but-nonempty vector would otherwise loop over fewer
            // elements than the field actually has and pass having verified
            // less than it claims.
            if (values.size() != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: field %u readback returned %zu elements, "
                             "expected %u\n",
                             i, values.size(), kCapacity);
                wf.destroy(ctx.allocator());
                return 1;
            }
            for (std::size_t e = 0; e < values.size(); ++e) {
                if (values[e] != 0.0f) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: field %u element %zu = %f, expected 0\n",
                                 i, e, values[e]);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }
        }

        for (std::uint32_t slot : {ohao::diff::WavefrontBuffers::kCurrentCountSlot,
                                    ohao::diff::WavefrontBuffers::kNextCountSlot}) {
            const std::uint32_t count = wf.readbackCounter(ctx.allocator(), slot);
            if (count != 0) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: counter slot %u = %u, expected 0\n",
                             slot, count);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }

        wf.destroy(ctx.allocator());
        std::printf("[diff_gpu_probe] OK: wavefront buffers zero across %u fields x %u paths "
                    "and both counter slots\n",
                    ohao::diff::PathStateLayout::kFieldCount, kCapacity);
    }

    // 8-10. Wavefront generate stage (shaders/diff/wf_generate.comp): one
    // dispatch, three checks against its output.
    //   8.  Closed form: generated directions reproduce the same hit
    //       distances check 3 validated analytically -- wf_generate.comp
    //       builds rays through camera_ray.glsl, the same include
    //       visibility_probe.comp uses, so this is what catches the two
    //       drifting apart.
    //   9.  Field round-trip: every PathStateField the shader wrote reads
    //       back on the CPU as what was written -- proves path_state.glsl
    //       and path_state_layout.hpp agree on the arena's byte layout, the
    //       same failure mode rng.glsl had (compiled, never executed).
    //   10. Queue population: the counter equals the pixel count exactly and
    //       queue 0 contains each path index exactly once -- an atomicAdd
    //       race would show up as duplicates or a short count.
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 48;
        constexpr uint32_t kCapacity = kW * kH;
        constexpr float kPlaneDistance = 2.0f;
        constexpr float kTanHalfFov = 0.2f;
        constexpr float kAspect = static_cast<float>(kW) / static_cast<float>(kH);

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_generate buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        // Default camera: origin (0,0,0), forward (0,0,-1), right (1,0,0),
        // up (0,1,0) -- the same convention checks 3/4's runVisibilityProbe
        // calls use.
        ohao::diff::WavefrontGenerateCamera camera;
        camera.tanHalfFov = kTanHalfFov;

        std::vector<uint32_t> queue0;
        if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_generate dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        const std::vector<float> originX = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::OriginX);
        const std::vector<float> originY = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::OriginY);
        const std::vector<float> originZ = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::OriginZ);
        const std::vector<float> dirX = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::DirX);
        const std::vector<float> dirY = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::DirY);
        const std::vector<float> dirZ = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::DirZ);
        const std::vector<float> throughputR = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputR);
        const std::vector<float> throughputG = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputG);
        const std::vector<float> throughputB = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputB);
        const std::vector<float> radianceR = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::RadianceR);
        const std::vector<float> radianceG = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::RadianceG);
        const std::vector<float> radianceB = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::RadianceB);
        const std::vector<float> pixelIndexField = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::PixelIndex);
        const std::vector<float> sampleIndexField = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::SampleIndex);
        const std::vector<float> bounceField = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::Bounce);
        const std::vector<float> aliveField = wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::Alive);

        const std::vector<const std::vector<float>*> allFields = {
            &originX, &originY, &originZ, &dirX, &dirY, &dirZ,
            &throughputR, &throughputG, &throughputB,
            &radianceR, &radianceG, &radianceB,
            &pixelIndexField, &sampleIndexField, &bounceField, &aliveField,
        };
        for (const auto* f : allFields) {
            if (f->size() != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: wf_generate field readback size %zu, "
                             "expected %u\n",
                             f->size(), kCapacity);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }

        // 8. Closed form.
        float maxAbsError = 0.0f;
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const uint32_t i = y * kW + x;
                const float ndcX = 2.0f * (static_cast<float>(x) + 0.5f) / kW - 1.0f;
                const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / kH;
                const float dx = ndcX * kAspect * kTanHalfFov;
                const float dy = ndcY * kTanHalfFov;
                const float expectedT = kPlaneDistance * std::sqrt(1.0f + dx * dx + dy * dy);

                // origin=(0,0,0), plane at z=-planeDistance: t = -planeDistance / dir.z.
                // dir.z < 0 is guaranteed at this FOV, same as check 3.
                const float dz = dirZ[i];
                if (dz >= 0.0f) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: pixel (%u,%u) generated dir.z = %f, "
                                 "expected < 0\n",
                                 x, y, dz);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
                const float actualT = -kPlaneDistance / dz;
                const float err = std::fabs(actualT - expectedT);
                maxAbsError = std::max(maxAbsError, err);
                if (err > 1e-4f) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: pixel (%u,%u) generated-dir t = %f, "
                                 "closed form %f (|err| = %g)\n",
                                 x, y, actualT, expectedT, err);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }
        }
        std::printf("[diff_gpu_probe] OK: wf_generate directions reproduce closed-form plane "
                    "intersection over %u pixels (max |err| = %g)\n",
                    kCapacity, maxAbsError);

        // 9. Field round-trip.
        for (uint32_t i = 0; i < kCapacity; ++i) {
            if (originX[i] != camera.origin[0] || originY[i] != camera.origin[1] ||
                originZ[i] != camera.origin[2]) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u origin = (%f,%f,%f), expected "
                             "(%f,%f,%f)\n",
                             i, originX[i], originY[i], originZ[i], camera.origin[0],
                             camera.origin[1], camera.origin[2]);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (throughputR[i] != 1.0f || throughputG[i] != 1.0f || throughputB[i] != 1.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u throughput = (%f,%f,%f), expected "
                             "(1,1,1)\n",
                             i, throughputR[i], throughputG[i], throughputB[i]);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (radianceR[i] != 0.0f || radianceG[i] != 0.0f || radianceB[i] != 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u radiance = (%f,%f,%f), expected "
                             "(0,0,0)\n",
                             i, radianceR[i], radianceG[i], radianceB[i]);
                wf.destroy(ctx.allocator());
                return 1;
            }

            uint32_t pixelIndexBits = 0, sampleIndexBits = 0, bounceBits = 0, aliveBits = 0;
            std::memcpy(&pixelIndexBits, &pixelIndexField[i], sizeof(pixelIndexBits));
            std::memcpy(&sampleIndexBits, &sampleIndexField[i], sizeof(sampleIndexBits));
            std::memcpy(&bounceBits, &bounceField[i], sizeof(bounceBits));
            std::memcpy(&aliveBits, &aliveField[i], sizeof(aliveBits));

            if (pixelIndexBits != i) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: path %u pixelIndex = %u, expected %u\n",
                             i, pixelIndexBits, i);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (sampleIndexBits != 0u) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: path %u sampleIndex = %u, expected 0\n",
                             i, sampleIndexBits);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (bounceBits != 0u) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: path %u bounce = %u, expected 0\n", i,
                             bounceBits);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (aliveBits != 1u) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: path %u alive = %u, expected 1\n", i,
                             aliveBits);
                wf.destroy(ctx.allocator());
                return 1;
            }

            // Dir's exact components were already validated via the
            // closed-form distance check above; here just confirm it
            // round-tripped as a finite, unit-length vector -- catches e.g.
            // an offset error that silently aliased into the wrong field's
            // block without disturbing the z-only distance check.
            const float len2 = dirX[i] * dirX[i] + dirY[i] * dirY[i] + dirZ[i] * dirZ[i];
            if (std::fabs(len2 - 1.0f) > 1e-3f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u dir not unit length: |dir|^2 = %f\n",
                             i, len2);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: all %u PathStateFields round-trip across %u paths "
                    "(origin, dir, throughput=1, radiance=0, pixelIndex, sampleIndex=0, bounce=0, "
                    "alive=1)\n",
                    ohao::diff::PathStateLayout::kFieldCount, kCapacity);

        // 10. Queue population.
        const std::uint32_t counter =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kCurrentCountSlot);
        if (counter != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: queue 0 counter = %u, expected %u\n",
                         counter, kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }
        if (queue0.size() != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: queue 0 readback size %zu, expected %u\n",
                         queue0.size(), kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }
        std::vector<uint32_t> sorted = queue0;
        std::sort(sorted.begin(), sorted.end());
        for (uint32_t i = 0; i < kCapacity; ++i) {
            if (sorted[i] != i) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: queue 0 sorted[%u] = %u, expected %u "
                             "(atomicAdd race: duplicate or missing path index)\n",
                             i, sorted[i], i);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: queue 0 counter = %u and contains each of %u path "
                    "indices exactly once\n",
                    counter, kCapacity);

        wf.destroy(ctx.allocator());
    }

    // 11. Layout-mapping probe (shaders/diff/wf_layout_probe.comp): proves
    // PathStateField enum order and path_state.glsl's PS_* constants agree,
    // field by field, in a way check 9 cannot. Check 9's values are
    // genuinely degenerate (origin (0,0,0), throughput (1,1,1), radiance
    // (0,0,0), sampleIndex and bounce both 0), so a transposition *within*
    // {OriginX,Y,Z}, within {ThroughputR,G,B}, within {RadianceR,G,B}, or
    // between SampleIndex and Bounce would round-trip there undetected.
    // Here every field gets a distinct value (1000+fieldIndex for floats,
    // 7000+fieldIndex for ints), so any permutation of the mapping mismatches
    // somewhere. Capacity 1000 is deliberately not a multiple of 64, so the
    // alignUp(capacity, 64) rounding path -- proven correct on paper for
    // capacity=1000 during review -- is exercised by actual execution here,
    // not just by the other checks' capacities (4096, all multiples of 64).
    {
        constexpr std::uint32_t kCapacity = 1000;

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: layout probe buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        if (!ctx.runWavefrontLayoutProbe(wf)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: layout probe dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        struct FieldExpect {
            ohao::diff::PathStateField field;
            const char* name;
            bool isInt;
            float expectedFloat;
            uint32_t expectedBits;
        };
        const FieldExpect expects[] = {
            {ohao::diff::PathStateField::OriginX, "OriginX", false, 1000.0f, 0u},
            {ohao::diff::PathStateField::OriginY, "OriginY", false, 1001.0f, 0u},
            {ohao::diff::PathStateField::OriginZ, "OriginZ", false, 1002.0f, 0u},
            {ohao::diff::PathStateField::DirX, "DirX", false, 1003.0f, 0u},
            {ohao::diff::PathStateField::DirY, "DirY", false, 1004.0f, 0u},
            {ohao::diff::PathStateField::DirZ, "DirZ", false, 1005.0f, 0u},
            {ohao::diff::PathStateField::ThroughputR, "ThroughputR", false, 1006.0f, 0u},
            {ohao::diff::PathStateField::ThroughputG, "ThroughputG", false, 1007.0f, 0u},
            {ohao::diff::PathStateField::ThroughputB, "ThroughputB", false, 1008.0f, 0u},
            {ohao::diff::PathStateField::RadianceR, "RadianceR", false, 1009.0f, 0u},
            {ohao::diff::PathStateField::RadianceG, "RadianceG", false, 1010.0f, 0u},
            {ohao::diff::PathStateField::RadianceB, "RadianceB", false, 1011.0f, 0u},
            {ohao::diff::PathStateField::PixelIndex, "PixelIndex", true, 0.0f, 7012u},
            {ohao::diff::PathStateField::SampleIndex, "SampleIndex", true, 0.0f, 7013u},
            {ohao::diff::PathStateField::Bounce, "Bounce", true, 0.0f, 7014u},
            {ohao::diff::PathStateField::Alive, "Alive", true, 0.0f, 7015u},
        };

        for (const FieldExpect& e : expects) {
            const std::vector<float> values = wf.readbackField(ctx.allocator(), e.field);
            if (values.size() != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: layout probe field %s readback size %zu, "
                             "expected %u\n",
                             e.name, values.size(), kCapacity);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (e.isInt) {
                uint32_t bits = 0;
                std::memcpy(&bits, &values[0], sizeof(bits));
                if (bits != e.expectedBits) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: layout probe field %s = %u, expected %u "
                                 "(field->offset mapping disagrees between "
                                 "path_state_layout.hpp and path_state.glsl)\n",
                                 e.name, bits, e.expectedBits);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            } else {
                if (values[0] != e.expectedFloat) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: layout probe field %s = %f, expected %f "
                                 "(field->offset mapping disagrees between "
                                 "path_state_layout.hpp and path_state.glsl)\n",
                                 e.name, values[0], e.expectedFloat);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }
        }

        wf.destroy(ctx.allocator());
        std::printf("[diff_gpu_probe] OK: layout probe -- all %u PathStateFields hold their "
                    "distinct expected value at capacity=%u (non-multiple-of-64)\n",
                    ohao::diff::PathStateLayout::kFieldCount, kCapacity);
    }

    // 12. Wavefront intersect stage (shaders/diff/wf_intersect.comp) with
    // compaction, driven through an indirect dispatch prepared by
    // shaders/diff/wf_prepare_indirect.comp. This is the check that catches
    // an atomicAdd compaction race: it works because, for this specific
    // scene, the surviving set is knowable analytically rather than just
    // measured.
    //
    // Reuses check 4's half-quad (quadMinY=0.0): only rays with ndcY > 0
    // hit, i.e. pixel rows y < kH/2. Because pixelIndex = y*kW+x (row-major,
    // same as wf_generate.comp), that set is exactly the contiguous range
    // [0, kW*kH/2) = [0, 1536) -- so queue ring 1, sorted, must equal
    // 0..1535 with nothing else, and counter slot kNextCountSlot must read
    // exactly 1536.
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 48;
        constexpr uint32_t kCapacity = kW * kH;  // 3072
        constexpr uint32_t kExpectedSurvivors = kCapacity / 2;  // 1536
        constexpr float kPlaneDistance = 2.0f;
        constexpr float kTanHalfFov = 0.2f;
        constexpr float kAspect = static_cast<float>(kW) / static_cast<float>(kH);

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_intersect buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        // Populate queue ring 0 / counter slot 0 first, as its own fully
        // queue-idle-separated submission (see runWavefrontGenerateProbe) --
        // this is not the barrier under test, so it stays maximally safe.
        ohao::diff::WavefrontGenerateCamera camera;
        camera.tanHalfFov = kTanHalfFov;
        std::vector<uint32_t> queue0;
        if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_intersect setup: wf_generate dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        // wf_prepare_indirect.comp + the indirectly-dispatched wf_intersect.comp,
        // all on one command buffer with the SHADER_WRITE -> INDIRECT_COMMAND_READ
        // barrier between them -- see gpu_probe_context.cpp's
        // runWavefrontIntersectProbe for exactly which barriers this records.
        std::vector<uint32_t> queue1;
        if (!ctx.runWavefrontIntersectProbe(wf, kPlaneDistance, /*quadMinY=*/0.0f, queue1)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_intersect dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        const std::uint32_t nextCount =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kNextCountSlot);
        if (nextCount != kExpectedSurvivors) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: queue ring 1 counter = %u, expected exactly %u "
                         "(compaction race: lost or duplicated an atomicAdd offset)\n",
                         nextCount, kExpectedSurvivors);
            wf.destroy(ctx.allocator());
            return 1;
        }
        if (queue1.size() != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: queue ring 1 readback size %zu, expected %u\n",
                         queue1.size(), kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }

        // The written prefix, sorted, must be exactly [0, kExpectedSurvivors)
        // with no duplicates and nothing extra.
        std::vector<uint32_t> survivors(queue1.begin(), queue1.begin() + kExpectedSurvivors);
        std::sort(survivors.begin(), survivors.end());
        for (uint32_t i = 0; i < kExpectedSurvivors; ++i) {
            if (survivors[i] != i) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: queue ring 1 sorted[%u] = %u, expected %u "
                             "(compaction race: duplicate or missing path index)\n",
                             i, survivors[i], i);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: wf_intersect compacted exactly %u survivors into queue "
                    "ring 1 (indices 0..%u, no duplicates, no dead paths)\n",
                    kExpectedSurvivors, kExpectedSurvivors - 1);

        // Bonus correctness beyond the brief's minimum: every survivor's
        // Alive flag stayed 1 and HitT matches check 8's closed form; every
        // dead path's Alive flag was cleared and HitT reads -1 (miss).
        const std::vector<float> aliveField =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::Alive);
        const std::vector<float> hitTField =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::HitT);
        if (aliveField.size() != kCapacity || hitTField.size() != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: wf_intersect Alive/HitT readback size mismatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }
        float maxAbsError = 0.0f;
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const uint32_t i = y * kW + x;
                uint32_t aliveBits = 0;
                std::memcpy(&aliveBits, &aliveField[i], sizeof(aliveBits));
                const bool expectAlive = i < kExpectedSurvivors;
                if (static_cast<bool>(aliveBits) != expectAlive) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: path %u Alive = %u, expected %u\n",
                                 i, aliveBits, expectAlive ? 1u : 0u);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
                if (expectAlive) {
                    const float ndcX = 2.0f * (static_cast<float>(x) + 0.5f) / kW - 1.0f;
                    const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / kH;
                    const float dx = ndcX * kAspect * kTanHalfFov;
                    const float dy = ndcY * kTanHalfFov;
                    const float expectedT = kPlaneDistance * std::sqrt(1.0f + dx * dx + dy * dy);
                    const float err = std::fabs(hitTField[i] - expectedT);
                    maxAbsError = std::max(maxAbsError, err);
                    if (err > 1e-4f) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: path %u HitT = %f, closed form %f "
                                     "(|err| = %g)\n",
                                     i, hitTField[i], expectedT, err);
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                } else if (hitTField[i] != -1.0f) {
                    std::fprintf(stderr, "[diff_gpu_probe] FAIL: path %u (dead) HitT = %f, expected -1\n",
                                 i, hitTField[i]);
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }
        }
        std::printf("[diff_gpu_probe] OK: wf_intersect Alive/HitT match compaction membership and "
                    "closed-form plane intersection (max |err| = %g)\n", maxAbsError);

        wf.destroy(ctx.allocator());
    }

    // 13. Indirect dispatch of an empty queue costs nothing -- the property
    // that makes a dead path genuinely free rather than just skipped-but-
    // still-paid-for. Counter slot kCurrentCountSlot is left at 0 (no
    // wf_generate call), so wf_prepare_indirect.comp computes
    // groupCountX = (0+63)/64 = 0, and vkCmdDispatchIndirect with
    // groupCountX == 0 launches zero workgroups -- wf_intersect.comp's very
    // first instruction, an unconditional atomicAdd on a canary counter,
    // never executes. A stage that merely early-returned per-invocation
    // (rather than never launching) would still show this canary at 0, so
    // the real claim under test is the dispatch cost, not just the output --
    // this check is a necessary but not sufficient witness of that; it is
    // the best a black-box GPU probe can observe without a timing query.
    {
        constexpr uint32_t kCapacity = 64;

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: empty-queue buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });
        // Deliberately no wf_generate call: counter slot kCurrentCountSlot
        // stays at 0 from wf.zero(), which is exactly the input under test.

        std::vector<uint32_t> queue1;
        if (!ctx.runWavefrontIntersectProbe(wf, /*planeDistance=*/2.0f, /*quadMinY=*/0.0f, queue1)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: empty-queue wf_intersect dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        const std::uint32_t canary =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kCanarySlot);
        if (canary != 0) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: canary = %u, expected 0 -- an indirect dispatch "
                         "sized from a live-count of 0 ran %u invocation(s)\n",
                         canary, canary);
            wf.destroy(ctx.allocator());
            return 1;
        }
        const std::uint32_t nextCount =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kNextCountSlot);
        if (nextCount != 0) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: next-queue counter = %u, expected 0\n",
                         nextCount);
            wf.destroy(ctx.allocator());
            return 1;
        }
        std::printf("[diff_gpu_probe] OK: indirect dispatch from a live-count of 0 ran zero "
                    "invocations (canary = 0, next-queue counter = 0)\n");

        wf.destroy(ctx.allocator());
    }

    arena.destroy(ctx.allocator());
    ctx.shutdown();
    std::printf("[diff_gpu_probe] PASS\n");
    return 0;
}
