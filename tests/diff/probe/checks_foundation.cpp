// Foundation checks 1-6: the arena, the pipeline and stage lifecycles,
// the ray query, the registry/arena seam, and CPU/GPU RNG parity.
//
// Lifted verbatim out of diff_gpu_probe.cpp, commentary and all.
#include "probe/checks_foundation.hpp"

#include "diff/grad/arena_layout.hpp"
#include "diff/grad/gradient_arena.hpp"
#include "diff/param/param_registry.hpp"
#include "diff/rng/diff_rng.hpp"
#include "diff/wavefront/compute_pipeline.hpp"
#include "diff/wavefront/wavefront_stage.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace ohao::diff::probe {

bool checkArenaAtomicsAndStage(ohao::diff::GpuProbeContext& ctx,
                               const ohao::diff::ArenaLayout& layout,
                               ohao::diff::GradientArena& arena,
                               std::size_t blockA, std::size_t blockB,
                               std::size_t blockC) {
    // 1. zero + readback
    //
    // The guard below asserts the EXPECTED element count, not merely that
    // something came back. `values.empty()` would let a readback that
    // returned ONE zeroed float pass a loop that then verifies one element
    // and calls the block zeroed -- the same weakness check 7 documents and
    // closes in its own case (review finding). The expected counts are the
    // sizes handed to ArenaLayout::add above, paired with their indices here
    // so a future block cannot be added to one list and not the other.
    ctx.runImmediate([&](VkCommandBuffer cmd) { arena.zero(cmd); });
    const std::pair<std::size_t, std::size_t> kZeroChecked[] = {{blockA, 16}, {blockB, 4}};
    for (const auto& [b, expectedCount] : kZeroChecked) {
        const std::vector<float> values = arena.readback(ctx.allocator(), b);
        if (values.size() != expectedCount) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: block %zu readback returned %zu floats, expected "
                         "%zu (the size it was added to the layout with). A short readback would "
                         "otherwise let this check pass having verified only the elements that "
                         "came back\n",
                         b, values.size(), expectedCount);
            return false;
        }
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (values[i] != 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: block %zu element %zu = %f, expected 0\n",
                             b, i, values[i]);
                return false;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: arena zero + readback\n");

    // 2. float atomics under contention. Also exercises
    // ohao::diff::ComputePipeline (Task 1, Stage 0b-2a) directly: build()
    // for diff_atomic_probe.comp.spv, confirm pipeline()/layout()/
    // descriptorSet() are all non-null, bind the arena buffer, then call
    // destroy() twice to confirm the second call is a no-op and that
    // destroy() nulls all three handles.
    //
    // This has its own OK line. It used to have none, on the stated grounds
    // that folding it into the atomics check below "keeps this section at
    // one printed OK line, matching the pre-refactor count" -- which is
    // exactly backwards: two of the assertions here (a second destroy() is
    // a no-op; destroy() nulls pipeline()/layout()/descriptorSet()) are
    // covered by nothing else, so deleting them would leave this probe's
    // stdout byte-identical. A check whose removal is invisible is not a
    // check. Printed OK-line counts are an output of what is verified,
    // never a target to hold constant.
    {
        ohao::diff::ComputePipeline pipelineSanity;
        const VkDescriptorType bindingTypes[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        if (!pipelineSanity.build(ctx.device(), "diff_atomic_probe.comp.spv", bindingTypes,
                                  /*pushConstantSize=*/8)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: ComputePipeline::build\n");
            return false;
        }
        if (pipelineSanity.pipeline() == VK_NULL_HANDLE ||
            pipelineSanity.layout() == VK_NULL_HANDLE ||
            pipelineSanity.descriptorSet() == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: ComputePipeline built a null handle\n");
            return false;
        }
        const VkBuffer buffersToBind[] = {arena.buffer()};
        if (!pipelineSanity.bindBuffers(ctx.device(), buffersToBind)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: ComputePipeline::bindBuffers\n");
            return false;
        }
        pipelineSanity.destroy(ctx.device());
        pipelineSanity.destroy(ctx.device());  // must be a no-op, not a double-free
        if (pipelineSanity.pipeline() != VK_NULL_HANDLE ||
            pipelineSanity.layout() != VK_NULL_HANDLE ||
            pipelineSanity.descriptorSet() != VK_NULL_HANDLE) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: ComputePipeline::destroy left a handle live\n");
            return false;
        }
    }
    std::printf("[diff_gpu_probe] OK: ComputePipeline build + bind + double destroy (second "
                "destroy is a no-op, all handles nulled)\n");

    constexpr uint32_t kInvocations = 4096;
    if (!ctx.runAtomicProbe(arena, /*targetIndex=*/0, kInvocations)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: atomic probe dispatch\n");
        return false;
    }
    const std::vector<float> after = arena.readback(ctx.allocator(), blockA);
    if (after.size() != 16) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: block %zu readback returned %zu floats, expected 16 "
                     "(the size it was added to the layout with). A short readback would "
                     "otherwise leave the out-of-target-index scan below covering fewer elements "
                     "than the block has\n",
                     blockA, after.size());
        return false;
    }
    if (after[0] != static_cast<float>(kInvocations)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: atomicAdd gave %f, expected %u "
                     "(lost updates = non-atomic accumulation)\n",
                     after[0], kInvocations);
        return false;
    }
    // Every element of the block, not just after[1] -- the message says
    // "wrote outside target index" and now asserts it (review finding),
    // matching what check 2b's twin loop already did over its own block.
    for (std::size_t i = 1; i < after.size(); ++i) {
        if (after[i] != 0.0f) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: atomicAdd wrote outside target index (block %zu "
                         "element %zu = %f, expected 0)\n",
                         blockA, i, after[i]);
            return false;
        }
    }
    std::printf("[diff_gpu_probe] OK: atomicAdd accumulated %u contended adds exactly\n",
                kInvocations);

    // 2b. ohao::diff::WavefrontStage (Stage 0b-2a Task 2): drives the same
    // atomic-probe canary as check 2, but through record() -- bind
    // pipeline, bind descriptor set, push constants, vkCmdDispatch -- rather
    // than a hand-rolled sequence. This is a real differential, not just a
    // handle-non-null sanity check: if record() forgot to bind the
    // descriptor set, pushed the wrong constants, or dispatched a Fixed
    // group count of 0, the canary would land on something other than
    // exactly kStageInvocations (most likely 0, since block C was zeroed
    // alongside every other block in check 1 and nothing else writes to
    // it), not silently pass.
    constexpr uint32_t kStageInvocations = 2048;
    {
        ohao::diff::WavefrontStage stage;
        const VkDescriptorType bindingTypes[] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        struct PushConstants {
            uint32_t targetIndex;
            uint32_t invocationCount;
        };
        if (!stage.build(ctx.device(), "diff_atomic_probe.comp.spv", bindingTypes,
                         sizeof(PushConstants))) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: WavefrontStage::build\n");
            return false;
        }
        const VkBuffer buffersToBind[] = {arena.buffer()};
        if (!stage.bindBuffers(ctx.device(), buffersToBind)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: WavefrontStage::bindBuffers\n");
            return false;
        }

        // Absolute float index of block C within the arena's single flat
        // `data[]` array -- computed from the layout rather than hardcoded,
        // since blockC's byte offset depends on blockA/blockB's sizes.
        const ohao::diff::ArenaBlock blockCInfo = layout.block(blockC);
        const uint32_t targetIndex = static_cast<uint32_t>(blockCInfo.offsetBytes / sizeof(float));

        const PushConstants push{targetIndex, kStageInvocations};
        stage.setPushConstants(&push, sizeof(push));
        stage.setGroupCount(
            ohao::diff::WavefrontStage::Fixed{(kStageInvocations + 63u) / 64u});

        ctx.runImmediate([&](VkCommandBuffer cmd) {
            stage.record(cmd);

            // Same host-read barrier dispatchStorageBufferCompute records
            // after its own dispatch -- that function, in
            // gpu_probe_context.cpp, IS the reference sequence for this
            // pattern (compute_pipeline.cpp records no barriers at all; it
            // contains no vkCmdPipelineBarrier call) --
            // WavefrontStage::record() deliberately does not
            // add this itself (see its class comment), so the check adds it
            // directly, exactly as WavefrontLoop will for each stage it
            // sequences in Task 3.
            VkBufferMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = arena.buffer();
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &barrier, 0,
                                 nullptr);
        });
        stage.destroy(ctx.device());

        const std::vector<float> stageResult = arena.readback(ctx.allocator(), blockC);
        if (stageResult.empty()) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: block %zu readback returned no data\n", blockC);
            return false;
        }
        if (stageResult[0] != static_cast<float>(kStageInvocations)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: WavefrontStage::record produced %f, expected "
                         "%u contended adds (stage did not run, or ran the wrong invocation "
                         "count)\n",
                         stageResult[0], kStageInvocations);
            return false;
        }
        for (std::size_t i = 1; i < stageResult.size(); ++i) {
            if (stageResult[i] != 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: WavefrontStage::record wrote outside its "
                             "target index (block %zu element %zu = %f)\n",
                             blockC, i, stageResult[i]);
                return false;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: WavefrontStage::record replays the atomic-probe canary -- "
                "%u contended adds exactly through a Fixed dispatch\n", kStageInvocations);
    return true;
}

bool checkRayQueryVisibility(ohao::diff::GpuProbeContext& ctx) {
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
        return false;
    }
    if (hitsT.size() != static_cast<std::size_t>(kW) * kH) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: hit buffer size %zu, expected %u\n",
                     hitsT.size(), kW * kH);
        return false;
    }

    // The centre pixel looks straight down -Z, so t is exactly the plane distance.
    const std::size_t centre = static_cast<std::size_t>(kH / 2) * kW + (kW / 2);
    if (std::fabs(hitsT[centre] - kPlaneDistance) > 1e-4f) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: centre ray t = %f, expected %f\n",
                     hitsT[centre], kPlaneDistance);
        return false;
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
                return false;
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
                return false;
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
        return false;
    }
    if (halfHits.size() != static_cast<std::size_t>(kW) * kH) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: half-quad hit buffer size %zu, expected %u\n",
                     halfHits.size(), kW * kH);
        return false;
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
                return false;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: half-quad pins camera Y orientation over %u pixels\n",
                kW * kH);
    return true;
}

bool checkRegistryArenaSeam(ohao::diff::GpuProbeContext& ctx) {
    // 5. The seam Stage 1 depends on most: a block index handed out by the registry
    //    must resolve correctly against an arena built from that registry's layout.
    //    Both hold ArenaLayout by value, so this proves the positional indices survive
    //    the copy -- previously true only by inspection.
    ohao::diff::ParamRegistry reg;
    const auto reg1 = reg.registerTexture("albedo", {8, 8, 3}, VK_FORMAT_R32G32B32A32_SFLOAT);
    const auto reg2 = reg.registerScalarBlock("ssao_params", 4);
    if (!reg1.ok || !reg2.ok) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: registry setup: %s %s\n",
                     reg1.error.c_str(), reg2.error.c_str());
        return false;
    }

    ohao::diff::GradientArena regArena;
    if (!regArena.build(ctx.allocator(), reg.layout())) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: arena build from registry layout\n");
        return false;
    }
    ctx.runImmediate([&](VkCommandBuffer cmd) { regArena.zero(cmd); });

    const ohao::diff::DiffParam* albedo = reg.find("albedo");
    const ohao::diff::DiffParam* ssao = reg.find("ssao_params");
    if (albedo == nullptr || ssao == nullptr) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: registered params not found\n");
        regArena.destroy(ctx.allocator());
        return false;
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
            return false;
        }
        for (float v : block) {
            if (v != 0.0f) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: %s not zeroed\n", e.name);
                regArena.destroy(ctx.allocator());
                return false;
            }
        }
    }
    regArena.destroy(ctx.allocator());
    std::printf("[diff_gpu_probe] OK: registry block indices resolve against arena "
                "(4 blocks, sizes match)\n");
    return true;
}

bool checkRngParity(ohao::diff::GpuProbeContext& ctx) {
    // 6. CPU/GPU RNG parity -- the invariant path replay backpropagation rests on.
    //    The backward pass stores no tape; it replays each light path from its seed.
    //    If shaders/includes/diff/rng.glsl and ohao/diff/rng/diff_rng.cpp disagree by
    //    a single bit, the replayed path is a DIFFERENT path and every gradient is
    //    silently wrong -- no crash, no NaN. Until now the two were verified
    //    identical only by reading them side by side.
    constexpr uint32_t kDraws = 64;
    constexpr uint32_t kPixel = 4096;
    constexpr uint32_t kSample = 3;
    constexpr uint32_t kSeed = 12345;

    ohao::diff::ArenaLayout rngLayout;
    const std::size_t drawBlock = rngLayout.add(kDraws);
    ohao::diff::GradientArena rngArena;
    if (!rngArena.build(ctx.allocator(), rngLayout)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: rng arena build\n");
        return false;
    }
    ctx.runImmediate([&](VkCommandBuffer cmd) { rngArena.zero(cmd); });

    std::vector<float> gpuDraws;
    if (!ctx.runRngParityProbe(kPixel, kSample, kSeed, kDraws, rngArena, drawBlock, gpuDraws)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: rng parity dispatch\n");
        rngArena.destroy(ctx.allocator());
        return false;
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
            return false;
        }
    }
    if (cpuRng.drawCount() != kDraws) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: drawCount %u, expected %u\n",
                     cpuRng.drawCount(), kDraws);
        rngArena.destroy(ctx.allocator());
        return false;
    }
    std::printf("[diff_gpu_probe] OK: CPU PathRng drawCount matches expected count (%u)\n",
                kDraws);
    rngArena.destroy(ctx.allocator());
    std::printf("[diff_gpu_probe] OK: CPU and GLSL RNG agree bit-exactly over %u draws\n",
                kDraws);
    return true;
}

}  // namespace ohao::diff::probe
