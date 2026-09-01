// The replay-instantiation probe.
//
// Lifted verbatim out of gpu_probe_context.cpp: same member, same
// signature, same body. A linkage change, not a value change.
#include "gpu_probe_context.hpp"

#include "context/probe_scene.hpp"

#include "diff/wavefront/compute_pipeline.hpp"
#include "diff/wavefront/wavefront_loop.hpp"
#include "diff/wavefront/wavefront_stage.hpp"
#include "render/rt/rt_acceleration_structure.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace ohao::diff {

// Same as gpu_probe_context.cpp had: the shared scene by name, so every
// call site below reads as it did when this code lived there.
using namespace probe_scene;  // NOLINT(google-build-using-namespace)

// ===========================================================================
// Stage 1 Task 1 -- the REPLAY-EQUIVALENCE probe.
// ===========================================================================
//
// The same closed-box fused loop runWavefrontFusedLoopProbe runs, run TWICE
// per bounce count -- once through the FORWARD instantiation of
// shaders/includes/diff/traverse.glsl and once through the REPLAY one -- with
// each writing its own binding-3 vertex trace. See the doc comment in
// gpu_probe_context.hpp for why the replay is a second FULL run from zeroed
// buffers rather than a resumed dispatch, and for why it is handed nothing
// the forward run produced.
//
// WHAT IS DELIBERATELY DUPLICATED FROM runWavefrontFusedLoopProbe, AND WHY IT
// IS NOT A PARAMETER ON IT. That probe's expected values -- the bit-exact
// 0.0625 throughput, the per-bounce PathRng parity, the live counts -- are
// calibrated to exactly one configuration, and its own doc comment records
// the judgement that generalising it would put every one of those calibrated
// checks "one parameter default away from silently changing scene". This
// probe needs a different SHAPE of run (two loops per bounce count, against
// two different scatter SPVs, with two sets of sinks), not different values,
// so it is a sibling for the same reason runWavefrontParityProbe is. The
// SCENE, though, is shared by construction: buildAxisAlignedBoxGeometry and
// the kFusedLoop* constants below are the same objects, not copies of them,
// so the survival induction that makes "every path, every bounce" non-vacuous
// cannot drift between the two probes.
bool GpuProbeContext::runWavefrontReplayProbe(
    WavefrontBuffers& buffers, uint32_t width, uint32_t height, uint32_t maxBounces, float albedo,
    uint32_t iterationSeed, std::vector<std::vector<float>>& outForwardTracePerBounce,
    std::vector<std::vector<float>>& outReplayTracePerBounce) {
    // Byte-identical to runWavefrontFusedLoopProbe's / runWavefrontGenerateProbe's.
    struct GeneratePush {
        float origin[3];
        float pad0;
        float forward[3];
        float pad1;
        float right[3];
        float pad2;
        float up[3];
        float pad3;
        uint32_t width;
        uint32_t height;
        float tanHalfFov;
        uint32_t capacity;
    };
    static_assert(sizeof(GeneratePush) == 80,
                  "GeneratePush must match wf_generate.comp's Push block layout");

    outForwardTracePerBounce.clear();
    outReplayTracePerBounce.clear();

    const uint32_t capacity = buffers.layout().capacity();
    bool ok = capacity > 0 && buffers.stateBuffer() != VK_NULL_HANDLE &&
              buffers.queueBuffer() != VK_NULL_HANDLE && buffers.counterBuffer() != VK_NULL_HANDLE;
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: buffers not built\n");
        return false;
    }
    // The dispatch-shape guard is runWavefrontFusedLoopProbe's, and
    // `width * height == capacity` carries a SECOND meaning here: it is
    // exactly "one path per pixel", i.e. one sample per pixel per dispatch,
    // which is the film-hazard option this subsystem took (spec 4.5; see the
    // long note on diffVertexHook in wf_scatter.comp). Refusing to run
    // without it is half of how that option is enforced; the other half is
    // the pixel-index histogram the consuming check measures.
    if (height != kFusedLoopGenerateLocalY || width == 0u ||
        (width % kFusedLoopGenerateLocalX) != 0u || width * height != capacity ||
        maxBounces == 0u) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontReplayProbe: requires height == %u "
                     "(wf_generate.comp's local_size_y -- this probe's generate dispatch is 1-D), "
                     "width a non-zero multiple of %u, width*height == capacity (%u) -- which is "
                     "also the ONE-SAMPLE-PER-PIXEL condition the film-hazard resolution rests "
                     "on -- and maxBounces > 0; got %ux%u, maxBounces %u\n",
                     kFusedLoopGenerateLocalY, kFusedLoopGenerateLocalX, capacity, width, height,
                     maxBounces);
        return false;
    }

    // --- Scene: the closed box. Same builder, same constants, same survival
    // induction as runWavefrontFusedLoopProbe. ---
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    buildAxisAlignedBoxGeometry(kFusedLoopBoxHalfExtent, positions, indices);

    GpuBuffer vertexBuffer = m_allocator.createBufferFromSpan<float>(
        std::span<const float>(positions),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer indexBuffer = m_allocator.createBufferFromSpan<uint32_t>(
        std::span<const uint32_t>(indices),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ok = vertexBuffer.isValid() && indexBuffer.isValid();
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: failed to create box "
                              "vertex/index buffers\n");
    }

    RTAccelerationStructure accel;
    if (ok && !accel.init(m_device, m_physicalDevice, m_queue, m_queueFamily, m_commandPool,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: "
                              "RTAccelerationStructure::init failed\n");
        ok = false;
    }
    if (ok) {
        BlasHandle blas = INVALID_BLAS;
        runImmediate([&](VkCommandBuffer cmd) {
            blas = accel.createBLASFromPositions(vertexBuffer.buffer,
                                                 static_cast<uint32_t>(positions.size() / 3),
                                                 indexBuffer.buffer,
                                                 static_cast<uint32_t>(indices.size()),
                                                 /*indexByteOffset=*/0, cmd);
        });
        if (blas == INVALID_BLAS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: "
                                  "createBLASFromPositions failed\n");
            ok = false;
        } else {
            accel.clearInstances();
            accel.addInstance(blas, glm::mat4(1.0f));
            runImmediate([&](VkCommandBuffer cmd) { accel.buildTLAS(cmd); });
            if (accel.getTLAS() == VK_NULL_HANDLE) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: buildTLAS "
                                      "produced no TLAS\n");
                ok = false;
            }
        }
    }

    // --- Two INDEPENDENT sets of scatter-side sinks, one per instantiation.
    //
    // Separate allocations, not one set reused, so that nothing the replay
    // stage writes can reach a byte the forward comparison reads -- including
    // through a future hook that does touch the film or the NEE record. The
    // traces in particular are read back into two separate host vectors from
    // two separate device buffers; "read back independently" is what makes
    // the forward run its own oracle rather than a value the replay was
    // handed.
    struct ScatterSinks {
        GpuBuffer trace;  // binding 3, the vertex trace -- the only one read
        GpuBuffer env;    // binding 6
        GpuBuffer nee;    // binding 7
        GpuBuffer film;   // binding 9
    };
    ScatterSinks fwdSinks;
    ScatterSinks repSinks;
    const VkDeviceSize traceBytes =
        static_cast<VkDeviceSize>(capacity) * kDebugDrawFloats * sizeof(float);
    const VkDeviceSize envBytes =
        static_cast<VkDeviceSize>(capacity) * kEnvSampleFloats * sizeof(float);
    const VkDeviceSize neeBytes =
        static_cast<VkDeviceSize>(capacity) * kNeeSampleFloats * sizeof(float);
    const uint32_t filmPixelCount = width * height;
    const VkDeviceSize filmBytes = static_cast<VkDeviceSize>(filmPixelCount) * 3u * sizeof(float);
    ScatterSinks* const sinkSets[2] = {&fwdSinks, &repSinks};
    for (ScatterSinks* s : sinkSets) {
        if (!ok) break;
        s->trace = m_allocator.createBuffer(traceBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                            AllocationUsage::GpuToCpu,
                                            /*persistentlyMapped=*/true);
        s->env = m_allocator.createBuffer(envBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                          AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        s->nee = m_allocator.createBuffer(neeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                          AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        // TRANSFER_DST: the film is read-modify-written by the forward hook,
        // so it is zero-filled at the top of every run's command buffer.
        s->film = m_allocator.createBuffer(
            filmBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        if (!s->trace.isValid() || !s->env.isValid() || !s->nee.isValid() || !s->film.isValid()) {
            std::fprintf(stderr,
                         "[GpuProbeContext] runWavefrontReplayProbe: scatter sink allocation "
                         "failed\n");
            ok = false;
        }
    }

    // --- Stages. generate/prepare_indirect/intersect are shared by both
    // runs -- the whole point is that only the SCATTER stage differs. ---
    WavefrontStage generate;
    WavefrontStage prepareIndirect;
    WavefrontStage intersect;
    WavefrontStage scatterForward;
    WavefrontStage scatterReplay;
    // Index-parallel to sinkSets: variant 0 is the FORWARD instantiation,
    // variant 1 the REPLAY one. The pairing is expressed once, here, so the
    // run loop below cannot bind one variant's stage to the other's sinks.
    WavefrontStage* const scatterStages[2] = {&scatterForward, &scatterReplay};

    const VkDescriptorType kStateQueueCounter[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkDescriptorType kCounterOnly[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkDescriptorType kIntersectBindings[6] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    // Both scatter instantiations declare the SAME bindings, because both
    // include the same traverse.glsl -- that is the structural claim this
    // task rests on, and here it shows up as one binding-type array used
    // twice rather than two that have to be kept in step.
    // Eleven, not ten: binding 10 is the gradient arena (Stage 1 Task 2).
    // See runWavefrontScatterProbe's note at its binding-10 bind for why the
    // probes that have no arena re-bind the film buffer there.
    const VkDescriptorType kScatterBindings[12] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};

    if (ok && !generate.build(m_device, "diff_wf_generate.comp.spv", kStateQueueCounter,
                              sizeof(GeneratePush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: generate build\n");
        ok = false;
    }
    if (ok && !prepareIndirect.build(m_device, "diff_wf_prepare_indirect.comp.spv", kCounterOnly,
                                     sizeof(WavefrontLoop::PrepareIndirectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: prepare_indirect build\n");
        ok = false;
    }
    if (ok && !intersect.build(m_device, "diff_wf_intersect.comp.spv", kIntersectBindings,
                               sizeof(WavefrontLoop::IntersectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: intersect build\n");
        ok = false;
    }
    if (ok && !scatterForward.build(m_device, "diff_wf_scatter.comp.spv", kScatterBindings,
                                    sizeof(WavefrontLoop::ScatterPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: forward scatter build\n");
        ok = false;
    }
    // The REPLAY instantiation. Before shaders/diff/wf_scatter_replay.comp
    // exists this is where the probe fails, and the message says so plainly:
    // the check that consumes this probe is written before the shader is, so
    // "no replay stage yet" must read as a clear failure and not as a
    // mysterious Vulkan error.
    if (ok && !scatterReplay.build(m_device, "diff_wf_scatter_replay.comp.spv", kScatterBindings,
                                   sizeof(WavefrontLoop::ScatterPush))) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontReplayProbe: REPLAY scatter build failed "
                     "(diff_wf_scatter_replay.comp.spv). If that shader does not exist yet, this "
                     "is the expected failure: there is no second instantiation of the traversal "
                     "to compare the forward one against\n");
        ok = false;
    }

    if (ok) {
        const VkBuffer stateQueueCounter[3] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                               buffers.counterBuffer()};
        const VkBuffer counterOnly[1] = {buffers.counterBuffer()};
        const VkBuffer intersectBuffers[5] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                              buffers.counterBuffer(), vertexBuffer.buffer,
                                              indexBuffer.buffer};
        ok = generate.bindBuffers(m_device, stateQueueCounter) &&
             prepareIndirect.bindBuffers(m_device, counterOnly) &&
             intersect.bindBuffers(m_device, intersectBuffers) &&
             intersect.bindAccelerationStructure(m_device, 5, accel.getTLAS());
        for (int i = 0; ok && i < 2; ++i) {
            const ScatterSinks& s = *sinkSets[i];
            const VkBuffer scatterBuffers[8] = {buffers.stateBuffer(),
                                                buffers.queueBuffer(),
                                                buffers.counterBuffer(),
                                                s.trace.buffer,
                                                buffers.envMarginalBuffer(),
                                                buffers.envConditionalBuffer(),
                                                s.env.buffer,
                                                s.nee.buffer};
            // ONE TLAS for both: the shadow rays and the path rays see one
            // scene, which is what keeps the two runs comparable at all.
            ok = scatterStages[i]->bindBuffers(m_device, scatterBuffers) &&
                 scatterStages[i]->bindAccelerationStructure(m_device, 8, accel.getTLAS()) &&
                 scatterStages[i]->bindStorageBuffer(m_device, 9, s.film.buffer) &&
                 // Binding 10, the gradient arena. This probe has none --
                 // it is about the two traversals walking ONE path, not
                 // about gradients -- so each variant re-binds its OWN
                 // film there and gradArenaFloats stays 0. See
                 // runWavefrontScatterProbe's note at the same call.
                 scatterStages[i]->bindStorageBuffer(m_device, 10, s.film.buffer) &&
                 // Binding 11, the emission-texture primal (Stage 1 Task 5):
                 // this probe configures no texture, so the film goes here
                 // too, for binding 10's reason.
                 scatterStages[i]->bindStorageBuffer(m_device, 11, s.film.buffer);
        }
        if (!ok) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: descriptor binding\n");
        }
    }

    if (ok) {
        // Camera at the box centre -- the survival induction's base case.
        GeneratePush genPush{};
        genPush.origin[0] = kFusedLoopCameraX;
        genPush.origin[1] = kFusedLoopCameraY;
        genPush.origin[2] = kFusedLoopCameraZ;
        genPush.forward[2] = 1.0f;
        genPush.right[0] = 1.0f;
        genPush.up[1] = 1.0f;
        genPush.width = width;
        genPush.height = height;
        genPush.tanHalfFov = kFusedLoopTanHalfFov;
        genPush.capacity = capacity;
        generate.setPushConstants(&genPush, sizeof(genPush));
        generate.setGroupCount(WavefrontStage::Fixed{width / kFusedLoopGenerateLocalX});

        WavefrontLoop::Config loopConfig;
        loopConfig.albedo = albedo;
        loopConfig.iterationSeed = iterationSeed;
        loopConfig.filmPixelCount = filmPixelCount;
        // Material left at Config's defaults -- the pure Lambertian
        // configuration whose per-bounce estimator weight is exactly
        // `albedo`, which is what lets the consuming check assert the traced
        // throughput against albedo^bounce EXACTLY and so establish that the
        // records it is comparing are not two buffers of zeros.

        WavefrontLoop loop;
        loop.setConfig(loopConfig);
        loop.setGenerate(generate);
        loop.setPrepareIndirect(prepareIndirect);
        loop.setIntersect(intersect);

        outForwardTracePerBounce.resize(maxBounces);
        outReplayTracePerBounce.resize(maxBounces);

        for (uint32_t bounces = 1; ok && bounces <= maxBounces; ++bounces) {
            for (int variant = 0; ok && variant < 2; ++variant) {
                ScatterSinks& s = *sinkSets[variant];
                // The ONLY difference between the two runs of this loop.
                loop.setScatter(*scatterStages[variant]);

                runImmediate([&](VkCommandBuffer cmd) {
                    // Fresh path state for EVERY run, forward and replay
                    // alike. The replay therefore starts from precisely what
                    // the forward started from -- zeroed buffers and the same
                    // seed -- and from nothing the forward produced.
                    buffers.zero(cmd);

                    // The film is caller-owned, so record() will not zero it
                    // (it zeroes nothing it does not own) and the forward
                    // hook atomicAdds into it. TRANSFER_WRITE ->
                    // SHADER_READ|SHADER_WRITE because the first thing the
                    // shader does to these bytes is a read-modify-write.
                    vkCmdFillBuffer(cmd, s.film.buffer, 0, VK_WHOLE_SIZE, 0u);
                    VkBufferMemoryBarrier filmZero{};
                    filmZero.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    filmZero.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    filmZero.dstAccessMask =
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                    filmZero.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    filmZero.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    filmZero.buffer = s.film.buffer;
                    filmZero.offset = 0;
                    filmZero.size = VK_WHOLE_SIZE;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                                         &filmZero, 0, nullptr);

                    // EVERY caller-owned buffer these dispatches WRITE goes
                    // through extraBarrierBuffers -- the trace at a fixed
                    // pathIndex*kDebugDrawFloats offset every bounce, the env
                    // and NEE sinks likewise, and the film, which is
                    // read-modify-written. Omitting any of them is a real
                    // missing memory dependency that nothing here would
                    // detect (wavefront_loop.hpp's class comment: every
                    // compute-side barrier deleted, zero SYNC- diagnostics).
                    // The env CDF buffers are deliberately absent: no
                    // dispatch writes them.
                    const VkBuffer loopExtras[4] = {s.trace.buffer, s.env.buffer, s.nee.buffer,
                                                    s.film.buffer};
                    loop.record(cmd, buffers, bounces, loopExtras);

                    // This probe reads back exactly ONE buffer through a
                    // mapped pointer -- the trace -- so exactly one buffer is
                    // named in a SHADER_WRITE -> HOST_READ barrier.
                    // vmaInvalidateAllocation handles the CPU cache side
                    // only; it is not a substitute for the GPU-side
                    // availability operation, and vkQueueWaitIdle alone does
                    // not make writes visible in the host domain per the
                    // Vulkan spec.
                    VkBufferMemoryBarrier toHost{};
                    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost.buffer = s.trace.buffer;
                    toHost.offset = 0;
                    toHost.size = VK_WHOLE_SIZE;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0,
                                         nullptr);
                });

                m_allocator.invalidateBuffer(s.trace);
                const auto* mapped = static_cast<const float*>(s.trace.getMappedData());
                if (mapped == nullptr) {
                    std::fprintf(stderr, "[GpuProbeContext] runWavefrontReplayProbe: trace buffer "
                                          "not mapped, cannot read back\n");
                    ok = false;
                    break;
                }
                std::vector<float>& out = (variant == 0)
                                              ? outForwardTracePerBounce[bounces - 1]
                                              : outReplayTracePerBounce[bounces - 1];
                out.assign(mapped,
                           mapped + (static_cast<std::size_t>(capacity) * kDebugDrawFloats));
            }
        }
    }

    // --- Cleanup, reverse order. ---
    scatterReplay.destroy(m_device);
    scatterForward.destroy(m_device);
    intersect.destroy(m_device);
    prepareIndirect.destroy(m_device);
    generate.destroy(m_device);
    for (ScatterSinks* s : sinkSets) {
        if (s->film.isValid()) m_allocator.destroyBuffer(s->film);
        if (s->nee.isValid()) m_allocator.destroyBuffer(s->nee);
        if (s->env.isValid()) m_allocator.destroyBuffer(s->env);
        if (s->trace.isValid()) m_allocator.destroyBuffer(s->trace);
    }
    if (vertexBuffer.isValid()) m_allocator.destroyBuffer(vertexBuffer);
    if (indexBuffer.isValid()) m_allocator.destroyBuffer(indexBuffer);

    return ok;
}

}  // namespace ohao::diff
