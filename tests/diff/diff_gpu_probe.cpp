// Standalone GPU probe for the differentiable renderer scaffolding.
// Requires a working Vulkan device. Returns 0 on success.
//
// Checks:
//   1. GradientArena allocates, zeroes, and reads back.
//   2. atomicAdd on a float SSBO accumulates correctly under contention;
//      also exercises ohao::diff::ComputePipeline's build/bind/destroy
//      lifecycle directly (Stage 0b-2a Task 1), then replays the same
//      dispatch through ohao::diff::WavefrontStage::record() (Stage 0b-2a
//      Task 2) into an independent arena block, proving record() actually
//      drives bind/push/dispatch rather than silently no-op'ing.
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
//   14. wf_scatter.comp's constant-albedo throughput decay is exact after 4
//       bounces (p=0.5 -> 0.0625, compared bit-exact, no tolerance) --
//       proves the Throughput and Bounce fields survive 4 separate dispatch
//       boundaries intact. (Origin/Dir are written every bounce but not
//       exercised by any assertion here -- intersect runs once, not per
//       bounce, so positions after bounce 0 are geometrically meaningless
//       scaffolding; Stage 0b-2 running intersect per bounce is what would
//       make them worth checking.)
//   15. wf_scatter.comp's per-bounce RNG draws (values AND drawCount) match
//       ohao::diff::PathRng's replay exactly, for every one of those 4
//       dispatches -- extends check 6's single-stream parity guarantee
//       across dispatch boundaries, which is what path replay in Stage 1
//       depends on.
//   16-18. The SAME two analytic properties as checks 14-15, re-proved with
//       the whole bounce loop fused into ONE command buffer through
//       ohao::diff::WavefrontLoop -- no vkQueueWaitIdle between stages, so
//       ordering is the barriers' job rather than a full-device idle wait's.
//       16 also asserts every path survives every bounce and the live ring
//       holds each path index exactly once, which is what keeps 17 from
//       passing vacuously over an empty survivor set.
//   19. wf_intersect.comp's stored geometric normal equals the analytic
//       surface normal of the face it hit, for every path, against a closed
//       axis-aligned box whose face planes and outward winding are known
//       host-side. The oracle is that geometry plus the closed-form camera
//       ray -- nothing the shader computed.

#include "gpu_probe_context.hpp"

#include "diff/device_caps.hpp"
#include "diff/grad/arena_layout.hpp"
#include "diff/grad/gradient_arena.hpp"
#include "diff/rng/diff_rng.hpp"
#include "diff/param/param_registry.hpp"
#include "diff/wavefront/compute_pipeline.hpp"
#include "diff/wavefront/wavefront_buffers.hpp"
#include "diff/wavefront/wavefront_loop.hpp"
#include "diff/wavefront/wavefront_stage.hpp"

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
    // Reserved for the WavefrontStage check below (Stage 0b-2a Task 2) so it
    // has its own independent target index rather than reusing blockA's,
    // which check 2 has already mutated by the time this runs -- reusing it
    // would make this check's result depend on execution order.
    const std::size_t blockC = layout.add(16);

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
            return 1;
        }
        if (pipelineSanity.pipeline() == VK_NULL_HANDLE ||
            pipelineSanity.layout() == VK_NULL_HANDLE ||
            pipelineSanity.descriptorSet() == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: ComputePipeline built a null handle\n");
            return 1;
        }
        const VkBuffer buffersToBind[] = {arena.buffer()};
        if (!pipelineSanity.bindBuffers(ctx.device(), buffersToBind)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: ComputePipeline::bindBuffers\n");
            return 1;
        }
        pipelineSanity.destroy(ctx.device());
        pipelineSanity.destroy(ctx.device());  // must be a no-op, not a double-free
        if (pipelineSanity.pipeline() != VK_NULL_HANDLE ||
            pipelineSanity.layout() != VK_NULL_HANDLE ||
            pipelineSanity.descriptorSet() != VK_NULL_HANDLE) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: ComputePipeline::destroy left a handle live\n");
            return 1;
        }
    }
    std::printf("[diff_gpu_probe] OK: ComputePipeline build + bind + double destroy (second "
                "destroy is a no-op, all handles nulled)\n");

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
            return 1;
        }
        const VkBuffer buffersToBind[] = {arena.buffer()};
        if (!stage.bindBuffers(ctx.device(), buffersToBind)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: WavefrontStage::bindBuffers\n");
            return 1;
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
            return 1;
        }
        if (stageResult[0] != static_cast<float>(kStageInvocations)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: WavefrontStage::record produced %f, expected "
                         "%u contended adds (stage did not run, or ran the wrong invocation "
                         "count)\n",
                         stageResult[0], kStageInvocations);
            return 1;
        }
        for (std::size_t i = 1; i < stageResult.size(); ++i) {
            if (stageResult[i] != 0.0f) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: WavefrontStage::record wrote outside its "
                             "target index (block %zu element %zu = %f)\n",
                             blockC, i, stageResult[i]);
                return 1;
            }
        }
    }
    std::printf("[diff_gpu_probe] OK: WavefrontStage::record replays the atomic-probe canary -- "
                "%u contended adds exactly through a Fixed dispatch\n", kStageInvocations);

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
        std::printf("[diff_gpu_probe] OK: CPU PathRng drawCount matches expected count (%u)\n",
                    kDraws);
        rngArena.destroy(ctx.allocator());
        std::printf("[diff_gpu_probe] OK: CPU and GLSL RNG agree bit-exactly over %u draws\n",
                    kDraws);
    }

    // 7. Wavefront path-state/queue/counter buffers: build, zero, and confirm
    //    every one of the PathStateFields (all kFieldCount of them, whatever
    //    that count is today) and both counter slots come back zeroed at
    //    exactly the expected element count. This is the
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

        // Deliberately NOT all kFieldCount fields: wf_generate.comp does not
        // write HitT (that is wf_intersect.comp's output, checked
        // separately in check 12), so this list -- and the count printed
        // below, which is derived from its size rather than kFieldCount --
        // covers exactly what this stage actually produces.
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
        std::printf("[diff_gpu_probe] OK: all %zu wf_generate-written PathStateFields round-trip "
                    "across %u paths (origin, dir, throughput=1, radiance=0, pixelIndex, "
                    "sampleIndex=0, bounce=0, alive=1)\n",
                    allFields.size(), kCapacity);

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
            {ohao::diff::PathStateField::HitT, "HitT", false, 1016.0f, 0u},
            {ohao::diff::PathStateField::NormalX, "NormalX", false, 1017.0f, 0u},
            {ohao::diff::PathStateField::NormalY, "NormalY", false, 1018.0f, 0u},
            {ohao::diff::PathStateField::NormalZ, "NormalZ", false, 1019.0f, 0u},
        };
        // This array must cover every PathStateField or the "all %u
        // PathStateFields" claim below is false -- exactly the coverage gap
        // a fix round caught here once already (HitT was added to the enum
        // in Task 5 but not to this array or to wf_layout_probe.comp until a
        // review found the mismatch). A build-time check so a future field
        // addition fails loudly here instead of silently narrowing what
        // "all" means.
        static_assert(sizeof(expects) / sizeof(expects[0]) ==
                          ohao::diff::PathStateLayout::kFieldCount,
                      "expects[] must have exactly kFieldCount entries -- add the new field's "
                      "row here AND its psSet* write in wf_layout_probe.comp");

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

        // The canary must equal exactly the number of invocations the
        // indirect dispatch launched. kCapacity (3072) is an exact multiple
        // of wf_intersect.comp's local_size_x=64, so
        // groupCountX = kCapacity/64 with no rounding tail, and every
        // invocation runs (queue ring 0 holds all kCapacity paths). A wrong
        // kCanarySlot or a wrong push field would still leave check 13's
        // "canary == 0 on an empty queue" true, so that check alone cannot
        // catch this; asserting a specific *nonzero* expected value here is
        // what turns the canary into a real differential rather than a check
        // that only ever needs to prove absence.
        const std::uint32_t canary =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kCanarySlot);
        if (canary != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: canary = %u, expected exactly %u invocations "
                         "(wrong kCanarySlot or push field would read 0 here and only be caught "
                         "by this nonzero assertion, not check 13's empty-queue one)\n",
                         canary, kCapacity);
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
        // The tail beyond what compaction actually wrote, [kExpectedSurvivors,
        // kCapacity), must still read as wf.zero()'s initial fill (0) --
        // never inspected until now. This is a "nothing extra was written"
        // sanity check, not a live exercise of the dstSlot < capacity guard
        // itself: at kCapacity=3072 with kExpectedSurvivors bounding dstSlot
        // well under capacity, that guard is provably unreachable in this
        // configuration. It still catches an off-by-one that spills the
        // compacted prefix past kExpectedSurvivors, a class the sorted-prefix
        // check above cannot see since that check only ever looks at
        // [0, kExpectedSurvivors).
        for (uint32_t i = kExpectedSurvivors; i < kCapacity; ++i) {
            if (queue1[i] != 0u) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: queue ring 1 tail[%u] = %u, expected 0 "
                             "(a write landed past the compacted prefix -- possible unclamped "
                             "dstSlot)\n",
                             i, queue1[i]);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }

        std::printf("[diff_gpu_probe] OK: wf_intersect compacted exactly %u survivors into queue "
                    "ring 1 (indices 0..%u, no duplicates, no dead paths, tail untouched)\n",
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

    // 14-15. Wavefront scatter stage (shaders/diff/wf_scatter.comp), run for
    // 4 real bounces: generate -> intersect once (full quad, quadMinY=-1, so
    // every ray hits and seeds every path into scatter) -> scatter x4,
    // ping-ponging the SAME two physical queue rings (each scatter call
    // zeroes its own destination counter slot first -- see
    // GpuProbeContext::runWavefrontScatterProbe's doc comment for why that
    // is load-bearing once a ring/slot pair is reused).
    //
    // Check 14 is the analytic throughput check: with albedo p=0.5 and every
    // ray surviving every bounce, throughput after 4 bounces must be exactly
    // p^4 = 0.0625, compared with ==, not a tolerance. This proves Throughput
    // and Bounce survive 4 dispatch boundaries -- it says nothing about
    // Origin/Dir, which this loop's single intersect call leaves stale after
    // bounce 0 (see the file header's note on check 14 for why).
    //
    // Check 15 is the RNG-parity check: for one chosen path, the exact
    // (u1, u2, drawCount) wf_scatter.comp computed at each of the 4 real,
    // separate dispatches must match ohao::diff::PathRng::forPath(...)
    // replayed the same number of draws on the CPU -- proving the GPU
    // reconstructs the RNG from (pixelIndex, sampleIndex, bounce) correctly
    // across a dispatch boundary, not just within a single dispatch (which
    // check 6 already covers).
    {
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 48;
        constexpr uint32_t kCapacity = kW * kH;  // 3072
        constexpr float kPlaneDistance = 2.0f;
        constexpr float kTanHalfFov = 0.2f;
        constexpr float kAlbedo = 0.5f;
        constexpr uint32_t kIterationSeed = 20260828u;
        constexpr uint32_t kBounces = 4;
        constexpr uint32_t kDrawsPerBounce = 2;  // must match wf_scatter.comp's kDrawsPerBounce
        constexpr uint32_t kChosenPath = 1234;   // arbitrary, < kCapacity
        static_assert(kChosenPath < kCapacity, "kChosenPath must be a valid path index");

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_scatter buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        ohao::diff::WavefrontGenerateCamera camera;
        camera.tanHalfFov = kTanHalfFov;
        std::vector<uint32_t> queue0;
        if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_scatter setup: wf_generate dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        // Full quad (quadMinY=-1.0): every ray hits, so every one of
        // kCapacity paths survives into scatter ring1/slot(kNextCountSlot).
        std::vector<uint32_t> queue1;
        if (!ctx.runWavefrontIntersectProbe(wf, kPlaneDistance, /*quadMinY=*/-1.0f, queue1)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_scatter setup: wf_intersect dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }
        const std::uint32_t seededCount =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kNextCountSlot);
        if (seededCount != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: wf_scatter setup: %u of %u rays hit the full "
                         "quad, expected all of them (quadMinY=-1 should guarantee every ray "
                         "hits)\n",
                         seededCount, kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }

        // Ping-pong: scatter's first source is the ring/slot wf_intersect
        // just filled (ring1/kNextCountSlot); its destination is the other
        // ring (ring0/kCurrentCountSlot), which currently holds a stale
        // pre-intersect count and gets zeroed by runWavefrontScatterProbe.
        uint32_t srcQueueBase = kCapacity;  // ring 1
        uint32_t srcCountSlot = ohao::diff::WavefrontBuffers::kNextCountSlot;
        uint32_t dstQueueBase = 0;  // ring 0
        uint32_t dstCountSlot = ohao::diff::WavefrontBuffers::kCurrentCountSlot;

        std::vector<std::vector<float>> drawsPerBounce(kBounces);
        bool scatterOk = true;
        for (uint32_t b = 0; b < kBounces && scatterOk; ++b) {
            std::vector<uint32_t> outQueue;
            std::vector<float> outDraws;
            if (!ctx.runWavefrontScatterProbe(wf, srcQueueBase, srcCountSlot, dstQueueBase,
                                              dstCountSlot, kAlbedo, kIterationSeed, outQueue,
                                              outDraws)) {
                std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_scatter dispatch at bounce %u\n", b);
                scatterOk = false;
                break;
            }
            const std::uint32_t survCount = wf.readbackCounter(ctx.allocator(), dstCountSlot);
            if (survCount != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: wf_scatter bounce %u re-queued %u paths, "
                             "expected all %u (every invocation must re-queue -- nothing "
                             "terminates in scatter yet)\n",
                             b, survCount, kCapacity);
                scatterOk = false;
                break;
            }
            // Inspect the re-queued ring itself, mirroring check 12's
            // sorted-prefix inspection of wf_intersect's compaction output:
            // survCount alone is a single scalar and cannot distinguish
            // "every path re-queued exactly once" from "some path's slot
            // got overwritten while another was dropped, but the atomicAdd
            // count still landed on kCapacity by coincidence." This is
            // precisely the ring GpuProbeContext::runWavefrontScatterProbe
            // was already paying to copy back as outQueue -- it was going
            // unread before this check existed, which is exactly the blind
            // spot that let wf_scatter.comp ship without wf_intersect.comp's
            // dstSlot < capacity guard (Important 1): a missing bounds guard
            // changes what lands in this ring without necessarily changing
            // the counter atomicAdd returns.
            if (outQueue.size() != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: wf_scatter bounce %u re-queued ring readback "
                             "size %zu, expected %u\n",
                             b, outQueue.size(), kCapacity);
                scatterOk = false;
                break;
            }
            std::vector<uint32_t> sortedQueue = outQueue;
            std::sort(sortedQueue.begin(), sortedQueue.end());
            for (uint32_t i = 0; i < kCapacity; ++i) {
                if (sortedQueue[i] != i) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: wf_scatter bounce %u re-queued ring "
                                 "sorted[%u] = %u, expected %u (duplicate or dead path index -- "
                                 "every survivor must appear exactly once)\n",
                                 b, i, sortedQueue[i], i);
                    scatterOk = false;
                    break;
                }
            }
            if (!scatterOk) break;
            if (outDraws.size() != static_cast<std::size_t>(kCapacity) * 3u) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: wf_scatter bounce %u debug-draws readback "
                             "size %zu, expected %zu\n",
                             b, outDraws.size(), static_cast<std::size_t>(kCapacity) * 3u);
                scatterOk = false;
                break;
            }
            drawsPerBounce[b] = std::move(outDraws);
            std::swap(srcQueueBase, dstQueueBase);
            std::swap(srcCountSlot, dstCountSlot);
        }
        if (!scatterOk) {
            wf.destroy(ctx.allocator());
            return 1;
        }
        std::printf("[diff_gpu_probe] OK: wf_scatter re-queued ring contains each of %u path "
                    "indices exactly once at every one of %u bounces (no duplicates, no dead "
                    "paths)\n",
                    kCapacity, kBounces);

        // 14. Throughput decay -- exact, no tolerance. Hardcoded to the
        // literal 0.0625f rather than computed as kAlbedo^kBounces here: if
        // this were derived from kAlbedo, perturbing kAlbedo alone (as
        // task-6-report.md's required proof does) would perturb BOTH sides
        // of the comparison identically and the check could never fail --
        // exactly the kind of tautological check the brief warns about. The
        // expected value must come from an independent source (arithmetic
        // done by hand: 0.5*0.5*0.5*0.5 = 0.0625), not from the GLSL/CPU
        // path currently under test.
        constexpr float expectedThroughput = 0.0625f;

        const std::vector<float> throughputR =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputR);
        const std::vector<float> throughputG =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputG);
        const std::vector<float> throughputB =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputB);
        if (throughputR.size() != kCapacity || throughputG.size() != kCapacity ||
            throughputB.size() != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: wf_scatter throughput readback size "
                                  "mismatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }
        for (uint32_t i = 0; i < kCapacity; ++i) {
            // Bit-exact on purpose -- p=0.5 keeps every intermediate product
            // exactly representable in float32, so an epsilon here would
            // defeat the entire point of the check.
            if (throughputR[i] != expectedThroughput || throughputG[i] != expectedThroughput ||
                throughputB[i] != expectedThroughput) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u throughput = (%.9g,%.9g,%.9g) after "
                             "%u bounces, expected exactly (%.9g,%.9g,%.9g) -- Throughput did not "
                             "survive every dispatch boundary intact\n",
                             i, static_cast<double>(throughputR[i]), static_cast<double>(throughputG[i]),
                             static_cast<double>(throughputB[i]), kBounces,
                             static_cast<double>(expectedThroughput),
                             static_cast<double>(expectedThroughput),
                             static_cast<double>(expectedThroughput));
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: wf_scatter throughput decay after %u bounces is exactly "
                    "%.9g (p=%.9g) for all %u paths\n",
                    kBounces, static_cast<double>(expectedThroughput), static_cast<double>(kAlbedo),
                    kCapacity);

        // 15. Per-bounce RNG parity for one chosen path.
        ohao::diff::PathRng cpuRng =
            ohao::diff::PathRng::forPath(kChosenPath, /*sampleIndex=*/0u, kIterationSeed);
        for (uint32_t b = 0; b < kBounces; ++b) {
            const float cpuU1 = cpuRng.next1D();
            const float cpuU2 = cpuRng.next1D();
            const std::uint32_t cpuDrawCount = cpuRng.drawCount();

            const std::vector<float>& gpuDraws = drawsPerBounce[b];
            const float gpuU1 = gpuDraws[static_cast<std::size_t>(kChosenPath) * 3u + 0u];
            const float gpuU2 = gpuDraws[static_cast<std::size_t>(kChosenPath) * 3u + 1u];
            std::uint32_t gpuDrawCount = 0;
            std::memcpy(&gpuDrawCount, &gpuDraws[static_cast<std::size_t>(kChosenPath) * 3u + 2u],
                       sizeof(gpuDrawCount));

            if (cpuU1 != gpuU1 || cpuU2 != gpuU2) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u RNG diverges at bounce %u: CPU "
                             "(%.9g,%.9g), GPU (%.9g,%.9g) -- wf_scatter.comp's reconstruction "
                             "from (pixelIndex, sampleIndex, bounce) does not match "
                             "ohao::diff::PathRng replayed the same number of draws\n",
                             kChosenPath, b, static_cast<double>(cpuU1), static_cast<double>(cpuU2),
                             static_cast<double>(gpuU1), static_cast<double>(gpuU2));
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (cpuDrawCount != gpuDrawCount) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u drawCount diverges at bounce %u: CPU "
                             "%u, GPU %u (expected %u -- (bounce+1)*%u)\n",
                             kChosenPath, b, cpuDrawCount, gpuDrawCount, (b + 1) * kDrawsPerBounce,
                             kDrawsPerBounce);
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (cpuDrawCount != (b + 1) * kDrawsPerBounce) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: path %u drawCount at bounce %u = %u, expected "
                             "%u\n",
                             kChosenPath, b, cpuDrawCount, (b + 1) * kDrawsPerBounce);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: wf_scatter per-bounce RNG draws (values and drawCount) "
                    "match ohao::diff::PathRng exactly across %u dispatch boundaries for path %u\n",
                    kBounces, kChosenPath);

        wf.destroy(ctx.allocator());
    }

    // 16-18. THE FUSED BOUNCE LOOP (ohao::diff::WavefrontLoop, Stage 0b-2a
    // Task 3). Everything above this point runs one wavefront stage per
    // command-buffer submission, with a vkQueueWaitIdle between every stage.
    // That idle wait is a full device barrier: it silently satisfies every
    // ordering requirement the stages have on each other, which is why
    // checks 12-15 can pass with barriers that would be wrong in a real
    // integrator. This block removes it. generate, and then
    // prepare_indirect/intersect/prepare_indirect/scatter once per bounce,
    // are recorded into ONE command buffer, and ordering becomes the
    // barriers' job for the first time (see wavefront_loop.hpp).
    //
    // The assertions are deliberately the SAME two analytic properties
    // checks 14 and 15 already prove stage-by-stage -- throughput exactly
    // albedo^bounces, and per-bounce RNG draws bit-identical to
    // ohao::diff::PathRng -- not new, weaker ones. The whole point is to
    // re-run a property that is already known to hold under the idle wait,
    // against the same shaders, with only the synchronization changed.
    //
    // 16. Every path survives every bounce, and the live ring holds each
    //     path index exactly once -- the compaction offsets are not
    //     displaced. This is the check that catches a stale (unzeroed)
    //     destination counter slot: an atomicAdd based on a non-zero
    //     starting value hands out offsets past the end of the ring, whose
    //     writes wf_scatter.comp's `dstSlot < capacity` guard then drops,
    //     leaving holes.
    // 17. Throughput after 4 fused bounces is exactly 0.0625.
    // 18. Per-bounce RNG draws match ohao::diff::PathRng exactly.
    {
        // height MUST be 8 because every expected value below is
        // calibrated to exactly 512 paths at 64x8 -- the 0.0625 throughput,
        // the per-bounce PathRng parity for kChosenPath, the live counts.
        // It is NOT a dispatch limitation: WavefrontStage::Fixed carries
        // groupsY/groupsZ and can dispatch a genuine 3-D grid, so a stage
        // recorded through WavefrontLoop can cover any resolution just as
        // checks 8-10's hand-recorded 2-D generate dispatch does.
        // runWavefrontFusedLoopProbe leaves groupsY/groupsZ at 1 and
        // dispatches (width/8, 1, 1) x local_size (8,8), covering exactly 8
        // pixel rows. Changing the resolution means recomputing the
        // expectations here first.
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 8;
        constexpr uint32_t kCapacity = kW * kH;  // 512
        constexpr float kAlbedo = 0.5f;
        constexpr uint32_t kIterationSeed = 20260828u;
        constexpr uint32_t kBounces = 4;
        constexpr uint32_t kDrawsPerBounce = 2;  // must match wf_scatter.comp's kDrawsPerBounce
        constexpr uint32_t kChosenPath = 333;    // arbitrary, < kCapacity
        static_assert(kChosenPath < kCapacity, "kChosenPath must be a valid path index");

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: fused loop buffers build\n");
            return 1;
        }

        std::vector<std::vector<float>> fusedDraws;
        std::vector<uint32_t> fusedLiveCounts;
        std::vector<uint32_t> fusedFinalQueue;
        if (!ctx.runWavefrontFusedLoopProbe(wf, kW, kH, kBounces, kAlbedo, kIterationSeed,
                                            fusedDraws, fusedLiveCounts, fusedFinalQueue)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: fused loop dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        // 16. Survivors and compaction integrity.
        //
        // A live count of exactly kCapacity at every bounce depth is not a
        // given: it is what the closed-box scene (see
        // runWavefrontFusedLoopProbe's doc comment) is built to guarantee,
        // and asserting it is what stops the throughput check below from
        // passing vacuously over an empty survivor set.
        if (fusedLiveCounts.size() != kBounces) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: fused loop returned %zu live counts, expected "
                         "%u\n",
                         fusedLiveCounts.size(), kBounces);
            wf.destroy(ctx.allocator());
            return 1;
        }
        for (uint32_t b = 0; b < kBounces; ++b) {
            if (fusedLiveCounts[b] != kCapacity) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop of %u bounces left %u live paths, "
                             "expected all %u -- survival here is conditional on wf_intersect.comp "
                             "writing the CORRECT stored normal (see the survival derivation's "
                             "\"Normal\" step in gpu_probe_context.cpp), so a wrong or missing "
                             "normal sending a path's scattered direction out through the wrong "
                             "face is the most likely cause; also consider a path that escaped the "
                             "closed box scene some other way, or a compaction counter slot not "
                             "zeroed before its atomicAdd\n",
                             b + 1u, fusedLiveCounts[b], kCapacity);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        // Same sorted-prefix inspection check 12/14 apply to the
        // stage-by-stage rings: the scalar count alone cannot distinguish
        // "every path re-queued exactly once" from "one slot overwritten and
        // another dropped, with the atomicAdd total landing on kCapacity
        // anyway."
        if (fusedFinalQueue.size() != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: fused loop final ring readback size %zu, expected "
                         "%u\n",
                         fusedFinalQueue.size(), kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }
        std::vector<uint32_t> sortedFused = fusedFinalQueue;
        std::sort(sortedFused.begin(), sortedFused.end());
        for (uint32_t i = 0; i < kCapacity; ++i) {
            if (sortedFused[i] != i) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop final ring sorted[%u] = %u, "
                             "expected %u (duplicate or missing path index -- compaction offsets "
                             "are displaced)\n",
                             i, sortedFused[i], i);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: fused loop kept all %u paths alive through %u bounces "
                    "and its final ring holds each path index exactly once\n",
                    kCapacity, kBounces);

        // 17. Throughput decay -- exact, no tolerance. Hardcoded to the
        // literal 0.0625f for the same reason check 14 hardcodes it:
        // deriving it from kAlbedo would perturb both sides of the
        // comparison identically and the check could never fail.
        // 0.5*0.5*0.5*0.5 = 0.0625, arithmetic done by hand.
        constexpr float expectedFusedThroughput = 0.0625f;

        const std::vector<float> fusedR =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputR);
        const std::vector<float> fusedG =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputG);
        const std::vector<float> fusedB =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::ThroughputB);
        if (fusedR.size() != kCapacity || fusedG.size() != kCapacity ||
            fusedB.size() != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: fused loop throughput readback size "
                                  "mismatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }
        for (uint32_t i = 0; i < kCapacity; ++i) {
            // Bit-exact on purpose -- p=0.5 keeps every intermediate product
            // exactly representable in float32.
            if (fusedR[i] != expectedFusedThroughput || fusedG[i] != expectedFusedThroughput ||
                fusedB[i] != expectedFusedThroughput) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop path %u throughput = "
                             "(%.9g,%.9g,%.9g) after %u bounces, expected exactly "
                             "(%.9g,%.9g,%.9g) -- a bounce ran the wrong number of times, or "
                             "Throughput did not survive a dispatch boundary inside the fused "
                             "command buffer\n",
                             i, static_cast<double>(fusedR[i]), static_cast<double>(fusedG[i]),
                             static_cast<double>(fusedB[i]), kBounces,
                             static_cast<double>(expectedFusedThroughput),
                             static_cast<double>(expectedFusedThroughput),
                             static_cast<double>(expectedFusedThroughput));
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: fused loop throughput decay after %u bounces is exactly "
                    "%.9g (p=%.9g) for all %u paths\n",
                    kBounces, static_cast<double>(expectedFusedThroughput),
                    static_cast<double>(kAlbedo), kCapacity);

        // 18. Per-bounce RNG parity, identical in form to check 15.
        // fusedDraws[b] is what bounce b's scatter dispatch wrote; see
        // runWavefrontFusedLoopProbe's doc comment for how each bounce's
        // draws are observed despite wf_scatter.comp writing them at a fixed
        // per-path offset.
        if (fusedDraws.size() != kBounces) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: fused loop returned %zu draw sets, expected %u\n",
                         fusedDraws.size(), kBounces);
            wf.destroy(ctx.allocator());
            return 1;
        }
        ohao::diff::PathRng fusedCpuRng =
            ohao::diff::PathRng::forPath(kChosenPath, /*sampleIndex=*/0u, kIterationSeed);
        for (uint32_t b = 0; b < kBounces; ++b) {
            const float cpuU1 = fusedCpuRng.next1D();
            const float cpuU2 = fusedCpuRng.next1D();
            const std::uint32_t cpuDrawCount = fusedCpuRng.drawCount();

            const std::vector<float>& gpuDraws = fusedDraws[b];
            if (gpuDraws.size() != static_cast<std::size_t>(kCapacity) * 3u) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop bounce %u debug-draws size %zu, "
                             "expected %zu\n",
                             b, gpuDraws.size(), static_cast<std::size_t>(kCapacity) * 3u);
                wf.destroy(ctx.allocator());
                return 1;
            }
            const float gpuU1 = gpuDraws[static_cast<std::size_t>(kChosenPath) * 3u + 0u];
            const float gpuU2 = gpuDraws[static_cast<std::size_t>(kChosenPath) * 3u + 1u];
            std::uint32_t gpuDrawCount = 0;
            std::memcpy(&gpuDrawCount, &gpuDraws[static_cast<std::size_t>(kChosenPath) * 3u + 2u],
                       sizeof(gpuDrawCount));

            if (cpuU1 != gpuU1 || cpuU2 != gpuU2) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop path %u RNG diverges at bounce %u: "
                             "CPU (%.9g,%.9g), GPU (%.9g,%.9g) -- the fused loop replayed a "
                             "different random stream than ohao::diff::PathRng\n",
                             kChosenPath, b, static_cast<double>(cpuU1), static_cast<double>(cpuU2),
                             static_cast<double>(gpuU1), static_cast<double>(gpuU2));
                wf.destroy(ctx.allocator());
                return 1;
            }
            if (cpuDrawCount != gpuDrawCount || cpuDrawCount != (b + 1) * kDrawsPerBounce) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: fused loop path %u drawCount at bounce %u: "
                             "CPU %u, GPU %u, expected %u ((bounce+1)*%u) -- the Bounce field the "
                             "fast-forward reads did not advance exactly once per fused bounce\n",
                             kChosenPath, b, cpuDrawCount, gpuDrawCount,
                             (b + 1) * kDrawsPerBounce, kDrawsPerBounce);
                wf.destroy(ctx.allocator());
                return 1;
            }
        }
        std::printf("[diff_gpu_probe] OK: fused loop per-bounce RNG draws (values and drawCount) "
                    "match ohao::diff::PathRng exactly across %u fused bounces for path %u\n",
                    kBounces, kChosenPath);

        wf.destroy(ctx.allocator());
    }

    // 19. GEOMETRIC NORMALS (Stage 0b-2b Task 1). wf_intersect.comp must
    // write the hit's real, forward-facing geometric normal into path state
    // (PathStateField::NormalX/Y/Z), and this asserts it against an oracle
    // that is pure analytic geometry computed here on the host -- the box's
    // face planes and the camera's closed-form ray -- with nothing the
    // shader computed anywhere in it. (The Dir field the GPU wrote is
    // deliberately NOT read: it is check 8's subject, not this check's
    // oracle.)
    //
    // WHY A BOX AND NOT THE QUAD every other intersect check uses: the
    // single quad at z = -planeDistance, seen from a camera at the origin
    // looking down -Z, has exactly one forward-facing normal, (0,0,1) --
    // which is precisely the value wf_scatter.comp used to hardcode. A
    // check built on that scene cannot tell a real normal from the constant.
    // A closed box entered from its centre reaches five of its six faces, so
    // five distinct analytic normals are asserted, and the box is wound
    // OUTWARD (see buildAxisAlignedBoxGeometry) so that every one of those
    // hits also has to go through the flip-to-oppose-the-ray step.
    //
    // GEOMETRY OF THE ORACLE. The camera sits at the box centre, so for a
    // unit direction d the exit distance through the face on axis k is
    // t_k = E / |d_k|; the face actually hit is the argmin over k, i.e. the
    // argmax of |d_k| (E is the same on all three axes). Its inward normal
    // -- which is the forward-facing one, since the ray leaves the box
    // through that face -- is -sign(d_k) * e_k.
    //
    // TIE-FREEDOM IS BY CONSTRUCTION, NOT BY LUCK. That argmax is only
    // well-defined if no two |d_k| are equal. With kW even and kH ODD:
    //   |d_x| ~ |2x + 1 - kW| * tanHalfFov / kH  (odd numerator)
    //   |d_y| ~ |kH - 2y - 1| * tanHalfFov / kH  (even numerator)
    // so |d_x| == |d_y| would need an odd integer to equal an even one, and
    // |d_x| == |d_z| (== 1 before normalisation) would need
    // |2x + 1 - kW| == kH / tanHalfFov == 24.5, not an integer. The closest
    // approach of any pair is therefore >= 0.5 * tanHalfFov / kH in
    // pre-normalisation units, and the loop below asserts a hard margin on
    // the normalised directions anyway, so a future change to kW/kH/FOV that
    // reintroduced a tie fails loudly here instead of silently comparing
    // against whichever face the GPU happened to pick.
    //
    // TOLERANCES. The two off-axis components are required to be BIT-EXACTLY
    // zero: the box's edge vectors are exactly axis-aligned, so
    // cross(v1 - v0, v2 - v0) is exactly zero on those two axes, and
    // multiplying an exact zero by any finite normalisation factor (or
    // negating it) stays zero. Only the remaining component passes through
    // normalize(), whose inversesqrt() GLSL permits to be up to 2 ULP off
    // (~2.4e-7 relative), so it is compared to +/-1 with a 1e-6 bound --
    // roughly 4x that spec limit, and six orders of magnitude tighter than
    // the distance to any other face's normal. HitT gets a relative bound of
    // 1e-4, loose enough for the ray-triangle solve and far tighter than the
    // gap between adjacent faces' distances.
    //
    // LIMITATION: this check CANNOT distinguish a face from its opposite.
    // The stored normal's sign comes entirely from wf_intersect.comp's flip
    // against the ray direction, not from which primitive was actually hit:
    // for any triangle whose cross product lies on axis k, the stored
    // normal is -sign(d_k) * e_k regardless of which triangle's index was
    // actually looked up. buildAxisAlignedBoxGeometry emits each face's two
    // triangles adjacently, axis by axis (tris 0,1 = +X; 2,3 = -X; 4,5 =
    // +Y; 6,7 = -Y; 8,9 = +Z; 10,11 = -Z), so a primitive-index bug of the
    // form `primitive ^ 1` (the OTHER triangle of the SAME face) or
    // `primitive ^ 2` (the OPPOSITE face, same axis) reads back a triangle
    // whose cross product still lies on the same axis with the same sign,
    // so it still normalize()s to the exact expected normal -- and HitT
    // (from rayQueryGetIntersectionTEXT, which never goes through the index
    // lookup at all) is unaffected by the bug in the first place. Both
    // assertions above would pass with the wrong triangle read. The
    // residual bug class this leaves uncaught is narrow -- an off-by-a-
    // different-amount indexing bug such as `primitive + 1` still fails
    // outright on 3 of the 6 faces, and a stride or scale error in the
    // index lookup fails everywhere -- but it is real, and this check does
    // not close it. It is not weakened to pretend otherwise; this is simply
    // what it does not cover.
    {
        // kH is ODD and kW EVEN on purpose -- see the tie-freedom note above.
        constexpr uint32_t kW = 64;
        constexpr uint32_t kH = 49;
        constexpr uint32_t kCapacity = kW * kH;  // 3136
        static_assert(kW % 2 == 0 && kH % 2 == 1,
                      "the argmax oracle's tie-freedom argument needs kW even and kH odd");
        constexpr float kAspect = static_cast<float>(kW) / static_cast<float>(kH);
        // Wide on purpose: at the default 0.2 every ray would hit the -Z
        // face and the check would degenerate to the one normal the old
        // hardcoded constant already had.
        constexpr float kTanHalfFov = 2.0f;
        constexpr float kBoxHalfExtent = 4.0f;
        constexpr float kNormalTolerance = 1e-6f;
        constexpr float kHitTRelTolerance = 1e-4f;
        // Minimum separation required between the largest and second-largest
        // |d_k| for the argmin face to be unambiguous. The construction above
        // guarantees >= 0.5 * kTanHalfFov / kH / |d| ~ 0.006; this is an
        // order of magnitude below that and ~4 orders above float noise.
        constexpr float kFaceMargin = 1e-3f;

        ohao::diff::WavefrontBuffers wf;
        if (!wf.build(ctx.allocator(), kCapacity)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: normal probe buffers build\n");
            return 1;
        }
        ctx.runImmediate([&](VkCommandBuffer cmd) { wf.zero(cmd); });

        // Camera at the box CENTRE, so every ray hits a face and the exit
        // distance is E / |d_k| with no origin offset term.
        ohao::diff::WavefrontGenerateCamera camera;
        camera.tanHalfFov = kTanHalfFov;
        std::vector<uint32_t> queue0;
        if (!ctx.runWavefrontGenerateProbe(wf, kW, kH, camera, queue0)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: normal probe wf_generate dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        std::vector<uint32_t> boxQueue1;
        if (!ctx.runWavefrontBoxIntersectProbe(wf, kBoxHalfExtent, boxQueue1)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: normal probe wf_intersect dispatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        // A ray from strictly inside a closed convex body always leaves it
        // through a face, so nothing may miss. If this trips, the readbacks
        // below would be comparing against normals for hits that never
        // happened.
        const std::uint32_t boxSurvivors =
            wf.readbackCounter(ctx.allocator(), ohao::diff::WavefrontBuffers::kNextCountSlot);
        if (boxSurvivors != kCapacity) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: normal probe: %u of %u paths hit the closed box, "
                         "expected all of them (a ray from the interior of a convex body cannot "
                         "miss it)\n",
                         boxSurvivors, kCapacity);
            wf.destroy(ctx.allocator());
            return 1;
        }

        const std::vector<float> nx =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::NormalX);
        const std::vector<float> ny =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::NormalY);
        const std::vector<float> nz =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::NormalZ);
        const std::vector<float> hitT =
            wf.readbackField(ctx.allocator(), ohao::diff::PathStateField::HitT);
        if (nx.size() != kCapacity || ny.size() != kCapacity || nz.size() != kCapacity ||
            hitT.size() != kCapacity) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: normal probe field readback size "
                                  "mismatch\n");
            wf.destroy(ctx.allocator());
            return 1;
        }

        // faceHits[2*k + (sign < 0)] -- six buckets, one per box face.
        uint32_t faceHits[6] = {0, 0, 0, 0, 0, 0};
        float maxNormalError = 0.0f;
        float maxHitTRelError = 0.0f;
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const uint32_t i = y * kW + x;

                // --- Analytic ray, host-side. Identical construction to
                // check 8's (and to camera_ray.glsl's), recomputed here
                // rather than read out of the Dir field so that nothing the
                // shader produced enters the oracle. ---
                const float ndcX = 2.0f * (static_cast<float>(x) + 0.5f) / kW - 1.0f;
                const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / kH;
                float d[3] = {ndcX * kAspect * kTanHalfFov, ndcY * kTanHalfFov, -1.0f};
                const float invLen =
                    1.0f / std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                d[0] *= invLen;
                d[1] *= invLen;
                d[2] *= invLen;

                // --- Analytic face: argmax |d_k|, with the tie margin
                // enforced rather than assumed. ---
                uint32_t axis = 0;
                for (uint32_t k = 1; k < 3u; ++k) {
                    if (std::fabs(d[k]) > std::fabs(d[axis])) axis = k;
                }
                float secondAbs = 0.0f;
                for (uint32_t k = 0; k < 3u; ++k) {
                    if (k != axis) secondAbs = std::max(secondAbs, std::fabs(d[k]));
                }
                if (std::fabs(d[axis]) - secondAbs < kFaceMargin) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: normal probe pixel (%u,%u): the box face "
                                 "this ray exits through is ambiguous -- |d| = (%.9g,%.9g,%.9g), "
                                 "largest exceeds runner-up by only %.9g < %.9g. The oracle's "
                                 "argmax is not well defined, so kW/kH/tanHalfFov must be chosen "
                                 "to keep every pair of |d_k| apart (see this check's "
                                 "tie-freedom note)\n",
                                 x, y, static_cast<double>(std::fabs(d[0])),
                                 static_cast<double>(std::fabs(d[1])),
                                 static_cast<double>(std::fabs(d[2])),
                                 static_cast<double>(std::fabs(d[axis]) - secondAbs),
                                 static_cast<double>(kFaceMargin));
                    wf.destroy(ctx.allocator());
                    return 1;
                }

                // Inward (== forward-facing) normal of the exit face, and
                // the exit distance, both straight from the box's algebra.
                float expected[3] = {0.0f, 0.0f, 0.0f};
                expected[axis] = (d[axis] > 0.0f) ? -1.0f : 1.0f;
                const float expectedT = kBoxHalfExtent / std::fabs(d[axis]);
                faceHits[2u * axis + ((d[axis] > 0.0f) ? 0u : 1u)] += 1u;

                const float actual[3] = {nx[i], ny[i], nz[i]};
                for (uint32_t k = 0; k < 3u; ++k) {
                    const float err = std::fabs(actual[k] - expected[k]);
                    maxNormalError = std::max(maxNormalError, err);
                    // Off-axis components: bit-exact zero (see this check's
                    // tolerance note). On-axis: within kNormalTolerance.
                    const bool bad = (k == axis) ? (err > kNormalTolerance)
                                                 : (actual[k] != 0.0f);
                    if (bad) {
                        std::fprintf(stderr,
                                     "[diff_gpu_probe] FAIL: normal probe pixel (%u,%u) path %u: "
                                     "stored normal = (%.9g,%.9g,%.9g), analytic box-face normal "
                                     "= (%.9g,%.9g,%.9g) (face: axis %u at %+.1f). Component %u "
                                     "differs by %.9g -- wf_intersect.comp is not writing the "
                                     "real geometric normal of the hit\n",
                                     x, y, i, static_cast<double>(actual[0]),
                                     static_cast<double>(actual[1]),
                                     static_cast<double>(actual[2]),
                                     static_cast<double>(expected[0]),
                                     static_cast<double>(expected[1]),
                                     static_cast<double>(expected[2]), axis,
                                     static_cast<double>(-expected[axis] * kBoxHalfExtent), k,
                                     static_cast<double>(err));
                        wf.destroy(ctx.allocator());
                        return 1;
                    }
                }

                const float relT = std::fabs(hitT[i] - expectedT) / expectedT;
                maxHitTRelError = std::max(maxHitTRelError, relT);
                if (relT > kHitTRelTolerance) {
                    std::fprintf(stderr,
                                 "[diff_gpu_probe] FAIL: normal probe pixel (%u,%u) hit distance "
                                 "%.9g, analytic box exit distance %.9g (rel err %.9g) -- the hit "
                                 "is not on the face the oracle's normal belongs to\n",
                                 x, y, static_cast<double>(hitT[i]),
                                 static_cast<double>(expectedT), static_cast<double>(relT));
                    wf.destroy(ctx.allocator());
                    return 1;
                }
            }
        }

        // Non-degeneracy: this scene reaches exactly FIVE of the box's six
        // faces, not "at least three" -- the sixth, +Z, is unreachable BY
        // CONSTRUCTION, not by bad luck on this run. The camera used above
        // is WavefrontGenerateCamera's default (only tanHalfFov is
        // overridden), whose forward is (0,0,-1); camera_ray.glsl's
        // dir = normalize(forward + right*(...) + up*(...)) only ever picks
        // up a z-component from `forward` (right and up are both z == 0
        // here), so every primary ray has d_z < 0 and none can exit through
        // +Z. faceHits[4] is +Z's bucket (2*axis + (d[axis]>0 ? 0 : 1) with
        // axis == 2, sign > 0 -- see the faceHits indexing comment above the
        // loop), so it must be exactly 0; the other five buckets (+X -X +Y
        // -Y -Z: faceHits[0,1,2,3,5]) must each be nonzero. Asserting each
        // face individually, rather than a floor on the count reached, is
        // what makes a future FOV or resolution change that collapsed
        // coverage to three faces fail here instead of passing silently --
        // and it also catches the oracle's own ray model being wrong: if
        // +Z ever came out nonzero, a sign got flipped somewhere and this
        // check's normals could no longer be trusted either.
        //
        // (-Z alone always totals exactly 600 at this kW/kH/tanHalfFov: the
        // argmax-of-|d_k| condition works out to |2x+1-kW| < kH/tanHalfFov,
        // i.e. |2x+1-64| < 24.5, giving x in [20,43] (24 columns), and
        // |kH-2y-1| < kH/tanHalfFov, i.e. |48-2y| < 24.5, giving y in
        // [12,36] (25 rows); 24*25 == 600. Not asserted as an exact count
        // here -- that arithmetic belongs in a comment, not baked into a
        // brittle assertion -- but it is why -Z's count in the OK: line
        // below is always 600.)
        static constexpr uint32_t kUnreachableFaceZPlus = 4u;
        static constexpr const char* kFaceNames[6] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
        bool faceCoverageOk = true;
        for (uint32_t f = 0; f < 6u; ++f) {
            const bool shouldBeReached = (f != kUnreachableFaceZPlus);
            const bool reached = faceHits[f] != 0u;
            if (reached != shouldBeReached) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: normal probe face %s (bucket %u) got %u hits, "
                             "expected %s -- +Z is unreachable by construction (camera forward is "
                             "(0,0,-1), so d_z < 0 for every primary ray) and the other five faces "
                             "must each be reached at least once (+X %u, -X %u, +Y %u, -Y %u, "
                             "+Z %u, -Z %u)\n",
                             kFaceNames[f], f, faceHits[f], shouldBeReached ? "nonzero" : "exactly 0",
                             faceHits[0], faceHits[1], faceHits[2], faceHits[3], faceHits[4],
                             faceHits[5]);
                faceCoverageOk = false;
            }
        }
        if (!faceCoverageOk) {
            wf.destroy(ctx.allocator());
            return 1;
        }
        uint32_t facesReached = 0;
        for (uint32_t f = 0; f < 6u; ++f) {
            if (faceHits[f] != 0u) ++facesReached;
        }

        std::printf("[diff_gpu_probe] OK: wf_intersect geometric normals match the analytic "
                    "box-face normals for all %u paths across %u distinct faces "
                    "(+X %u, -X %u, +Y %u, -Y %u, +Z %u, -Z %u; max |normal err| = %g, "
                    "max relative HitT err = %g)\n",
                    kCapacity, facesReached, faceHits[0], faceHits[1], faceHits[2], faceHits[3],
                    faceHits[4], faceHits[5], static_cast<double>(maxNormalError),
                    static_cast<double>(maxHitTRelError));

        wf.destroy(ctx.allocator());
    }

    arena.destroy(ctx.allocator());
    ctx.shutdown();
    std::printf("[diff_gpu_probe] PASS\n");
    return 0;
}
