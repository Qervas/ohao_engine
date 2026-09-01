// The scatter stage probe.
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

// --- Scene builders shared by more than one probe -------------------------

bool GpuProbeContext::runWavefrontScatterProbe(WavefrontBuffers& buffers, uint32_t srcQueueBase,
                                               uint32_t srcCountSlot, uint32_t dstQueueBase,
                                               uint32_t dstCountSlot, float albedo,
                                               uint32_t iterationSeed,
                                               std::vector<uint32_t>& outQueueDst,
                                               std::vector<float>& outDebugDraws,
                                               const WavefrontScatterMaterial& material,
                                               std::vector<float>* outEnvSamples,
                                               const WavefrontShadowScene& shadowScene,
                                               std::vector<float>* outNeeSamples) {
    outQueueDst.clear();
    outDebugDraws.clear();
    if (outEnvSamples != nullptr) outEnvSamples->clear();
    if (outNeeSamples != nullptr) outNeeSamples->clear();

    const uint32_t capacity = buffers.layout().capacity();
    bool ok = capacity > 0 && buffers.stateBuffer() != VK_NULL_HANDLE &&
              buffers.queueBuffer() != VK_NULL_HANDLE && buffers.counterBuffer() != VK_NULL_HANDLE;
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: buffers not built\n");
        return false;
    }

    // --- prepare_indirect (counter only) and scatter (state, queue,
    // counter, debug draws), via the wavefront execution library rather
    // than a hand-rolled shader-module -> layout -> pipeline -> pool -> set
    // sequence for each. Reuses WavefrontLoop::PrepareIndirectPush/
    // ScatterPush -- byte-identical to this function's former private
    // PrepPush/ScatterPush -- rather than redeclaring them. ---
    // --- Occluders for the next-event estimator's shadow rays. An empty
    // WavefrontShadowScene becomes ONE triangle a million units out: the
    // acceleration-structure descriptor cannot be VK_NULL_HANDLE (this
    // context does not enable nullDescriptor), and the shader's shadow rays
    // stop at kShadowTMax = 1000, so geometry at 1e6 is reachable by nothing
    // and "no occluders" is expressed as data rather than as a null handle.
    // The triangle has real area, so the BLAS build has nothing to reject. ---
    static constexpr float kUnreachable = 1.0e6f;
    static const std::array<float, 9> kEmptySceneVertices = {
        kUnreachable,        kUnreachable,        kUnreachable,
        kUnreachable + 1.0f, kUnreachable,        kUnreachable,
        kUnreachable,        kUnreachable + 1.0f, kUnreachable};
    static const std::array<uint32_t, 3> kEmptySceneIndices = {0, 1, 2};
    const std::span<const float> shadowPositions =
        shadowScene.empty() ? std::span<const float>(kEmptySceneVertices) : shadowScene.positions;
    const std::span<const uint32_t> shadowIndices =
        shadowScene.empty() ? std::span<const uint32_t>(kEmptySceneIndices) : shadowScene.indices;

    GpuBuffer shadowVertexBuffer = m_allocator.createBufferFromSpan<float>(
        shadowPositions, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer shadowIndexBuffer = m_allocator.createBufferFromSpan<uint32_t>(
        shadowIndices, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    RTAccelerationStructure shadowAccel;
    if (!shadowVertexBuffer.isValid() || !shadowIndexBuffer.isValid()) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: shadow scene "
                              "vertex/index buffer allocation failed\n");
        ok = false;
    }
    if (ok && !shadowAccel.init(m_device, m_physicalDevice, m_queue, m_queueFamily, m_commandPool,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: "
                              "RTAccelerationStructure::init failed\n");
        ok = false;
    }
    if (ok) {
        BlasHandle blas = INVALID_BLAS;
        runImmediate([&](VkCommandBuffer cmd) {
            blas = shadowAccel.createBLASFromPositions(
                shadowVertexBuffer.buffer, static_cast<uint32_t>(shadowPositions.size() / 3),
                shadowIndexBuffer.buffer, static_cast<uint32_t>(shadowIndices.size()),
                /*indexByteOffset=*/0, cmd);
        });
        if (blas == INVALID_BLAS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: shadow scene "
                                  "createBLASFromPositions failed\n");
            ok = false;
        } else {
            shadowAccel.clearInstances();
            shadowAccel.addInstance(blas, glm::mat4(1.0f));
            runImmediate([&](VkCommandBuffer cmd) { shadowAccel.buildTLAS(cmd); });
            if (shadowAccel.getTLAS() == VK_NULL_HANDLE) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: shadow scene "
                                      "buildTLAS produced no TLAS\n");
                ok = false;
            }
        }
    }
    if (!ok) {
        if (shadowIndexBuffer.isValid()) m_allocator.destroyBuffer(shadowIndexBuffer);
        if (shadowVertexBuffer.isValid()) m_allocator.destroyBuffer(shadowVertexBuffer);
        return false;
    }

    WavefrontStage prepareIndirect;
    WavefrontStage scatter;
    const VkDescriptorType counterOnly[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    // state, queues, counters, debug draws, env marginal CDF, env
    // conditional CDF, env samples, NEE samples, TLAS -- wf_scatter.comp's
    // bindings 0..8 in order. The two env CDF buffers are read-only to the
    // shader but are ordinary storage buffers as far as the descriptor set
    // is concerned; the acceleration structure is last so bindBuffers can
    // write all eight storage buffers as one contiguous prefix.
    // ... and binding 9, the film (Stage 0b-2b Task 5). The acceleration
    // structure at 8 sits BETWEEN two storage buffers now, so the storage
    // buffers are no longer one contiguous prefix -- bindBuffers writes
    // 0..N-1 in order, so the film cannot go through it and is bound
    // separately by bindStorageBuffer below.
    // ... and binding 10, the gradient arena (Stage 1 Task 2). See the note
    // at the bindStorageBuffer call below for why THIS probe binds the film
    // buffer there rather than allocating a placeholder.
    const VkDescriptorType scatterBindingTypes[12] = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};

    if (!prepareIndirect.build(m_device, "diff_wf_prepare_indirect.comp.spv", counterOnly,
                               sizeof(WavefrontLoop::PrepareIndirectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: prepare_indirect "
                              "build\n");
        m_allocator.destroyBuffer(shadowIndexBuffer);
        m_allocator.destroyBuffer(shadowVertexBuffer);
        return false;
    }
    if (!scatter.build(m_device, "diff_wf_scatter.comp.spv", scatterBindingTypes,
                       sizeof(WavefrontLoop::ScatterPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: scatter build\n");
        prepareIndirect.destroy(m_device);
        m_allocator.destroyBuffer(shadowIndexBuffer);
        m_allocator.destroyBuffer(shadowVertexBuffer);
        return false;
    }

    // --- Readback buffers, owned by this function: dst queue ring (via
    // vkCmdCopyBuffer, same reasoning as runWavefrontIntersectProbe -- the
    // queue buffer is only exposed as a raw VkBuffer) and DebugDraws (host-
    // visible directly, since this buffer is allocated by this function and
    // never shared, so it can just be GpuToCpu-mapped like
    // runVisibilityProbe's hit buffer -- no copy needed). ---
    GpuBuffer queueReadback;
    const VkDeviceSize queueBytes = static_cast<VkDeviceSize>(capacity) * sizeof(uint32_t);
    if (ok) {
        queueReadback = m_allocator.createBuffer(queueBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                 AllocationUsage::GpuToCpu,
                                                 /*persistentlyMapped=*/true);
        if (!queueReadback.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: queue readback "
                                  "buffer allocation failed\n");
            ok = false;
        }
    }

    GpuBuffer debugDrawsBuffer;
    const VkDeviceSize debugDrawsBytes =
        static_cast<VkDeviceSize>(capacity) * kDebugDrawFloats * sizeof(float);
    if (ok) {
        debugDrawsBuffer = m_allocator.createBuffer(debugDrawsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                    AllocationUsage::GpuToCpu,
                                                    /*persistentlyMapped=*/true);
        if (!debugDrawsBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: debug draws buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }

    // wf_scatter.comp's environment-sample sink (binding 6): 4 floats per
    // path index. Allocated unconditionally -- the descriptor set needs
    // something valid bound there whether or not the caller asked to read it
    // back.
    GpuBuffer envSamplesBuffer;
    const VkDeviceSize envSamplesBytes =
        static_cast<VkDeviceSize>(capacity) * kEnvSampleFloats * sizeof(float);
    if (ok) {
        envSamplesBuffer = m_allocator.createBuffer(
            envSamplesBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::GpuToCpu,
            /*persistentlyMapped=*/true);
        if (!envSamplesBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: env samples buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }

    // wf_scatter.comp's next-event sink (binding 7): kNeeSampleFloats floats
    // per path index. Allocated unconditionally for the same reason
    // envSamplesBuffer is -- the descriptor set is not optional.
    GpuBuffer neeSamplesBuffer;
    const VkDeviceSize neeSamplesBytes =
        static_cast<VkDeviceSize>(capacity) * kNeeSampleFloats * sizeof(float);
    if (ok) {
        neeSamplesBuffer = m_allocator.createBuffer(
            neeSamplesBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::GpuToCpu,
            /*persistentlyMapped=*/true);
        if (!neeSamplesBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: NEE samples buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }

    // wf_scatter.comp's FILM (binding 9): 3 floats per PIXEL index. This
    // probe runs one sample per pixel, so pixel count == capacity here.
    // Allocated and bound but never read back -- see this function's doc
    // comment: the ordering it would exercise is a device idle wait's, not a
    // barrier's, so a film check here would be measuring nothing. It is
    // zeroed in the command buffer below all the same, because a film
    // accumulating onto whatever the allocator handed back is a bug class
    // worth not having even in an unread buffer.
    GpuBuffer filmBuffer;
    const VkDeviceSize filmBytes = static_cast<VkDeviceSize>(capacity) * 3u * sizeof(float);
    if (ok) {
        filmBuffer = m_allocator.createBuffer(
            filmBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        if (!filmBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: film buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }

    if (ok) {
        const VkBuffer counterOnlyBuf[1] = {buffers.counterBuffer()};
        const VkBuffer scatterBuffers[8] = {buffers.stateBuffer(),         buffers.queueBuffer(),
                                            buffers.counterBuffer(),       debugDrawsBuffer.buffer,
                                            buffers.envMarginalBuffer(),   buffers.envConditionalBuffer(),
                                            envSamplesBuffer.buffer,       neeSamplesBuffer.buffer};
        if (!prepareIndirect.bindBuffers(m_device, counterOnlyBuf) ||
            !scatter.bindBuffers(m_device, scatterBuffers) ||
            !scatter.bindAccelerationStructure(m_device, 8, shadowAccel.getTLAS()) ||
            // Binding 9 sits after the acceleration structure, so it cannot
            // go through bindBuffers' 0-based prefix.
            !scatter.bindStorageBuffer(m_device, 9, filmBuffer.buffer) ||
            // BINDING 10, THE GRADIENT ARENA. A descriptor set must cover
            // every binding the shader statically declares, and this probe
            // has no arena: it runs the FORWARD instantiation, whose hook is
            // the film write, and it leaves ScatterPush::gradArenaFloats at
            // 0, which disables every gradient write in the traversal.
            //
            // The FILM buffer is re-bound here rather than a placeholder
            // being allocated, and that choice is deliberate rather than
            // lazy: if a gradient write ever DID reach a probe that set
            // gradArenaFloats to 0, it would land in the film -- where
            // checks 32, 33 and 34 compare the film against independent
            // oracles and would fail loudly. A private placeholder buffer
            // would absorb the same stray write in silence. The same
            // reasoning and the same re-bind appear in the fused-loop,
            // replay and parity probes.
            !scatter.bindStorageBuffer(m_device, 10, filmBuffer.buffer) ||
            // BINDING 11, THE EMISSION-TEXTURE PRIMAL (Stage 1 Task 5). The
            // film again, for binding 10's reason exactly: this probe
            // configures no texture (ScatterPush::emissionTexWidth stays 0,
            // which makes the traversal read the uniform `emission` scalar
            // and never touch this buffer), and a stray read here is
            // harmless while a stray WRITE -- which the shader's `readonly`
            // already forbids -- would land somewhere three independent
            // film checks would notice.
            !scatter.bindStorageBuffer(m_device, 11, filmBuffer.buffer)) {
            std::fprintf(stderr,
                         "[GpuProbeContext] runWavefrontScatterProbe: descriptor binding\n");
            ok = false;
        }
    }

    if (ok) {
        const WavefrontLoop::ScatterPush scatterPush{capacity,
                                                     srcQueueBase,
                                                     srcCountSlot,
                                                     dstQueueBase,
                                                     dstCountSlot,
                                                     albedo,
                                                     iterationSeed,
                                                     material.roughness,
                                                     material.metallic,
                                                     material.specularWeight,
                                                     buffers.envWidth(),
                                                     buffers.envHeight(),
                                                     buffers.envIntegral(),
                                                     /*filmPixelCount=*/capacity};
        scatter.setPushConstants(&scatterPush, sizeof(scatterPush));
        const VkDeviceSize dstSlotOffset =
            static_cast<VkDeviceSize>(dstCountSlot) * sizeof(uint32_t);

        runImmediate([&](VkCommandBuffer cmd) {
            // --- Clear dstCountSlot to 0 before anything reads or writes it.
            // Unlike wf_intersect's checks (always a fresh, zeroed
            // WavefrontBuffers, ring0 -> ring1 exactly once), a multi-bounce
            // caller ping-pongs the SAME two physical rings across many
            // scatter calls, so this slot generally holds a stale prior
            // count -- reusing it as the atomicAdd base would silently
            // corrupt every compaction offset after the first entry. ---
            vkCmdFillBuffer(cmd, buffers.counterBuffer(), dstSlotOffset, sizeof(uint32_t), 0u);
            // The film, for the same reason: wf_scatter.comp atomicAdds into
            // it, so it must be 0 going in or the accumulation starts from
            // whatever the allocator handed back.
            vkCmdFillBuffer(cmd, filmBuffer.buffer, 0, VK_WHOLE_SIZE, 0u);

            VkBufferMemoryBarrier fillBarrier[2]{};
            const VkBuffer filled[2] = {buffers.counterBuffer(), filmBuffer.buffer};
            for (int i = 0; i < 2; ++i) {
                fillBarrier[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                fillBarrier[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                fillBarrier[i].dstAccessMask =
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                fillBarrier[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                fillBarrier[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                fillBarrier[i].buffer = filled[i];
                fillBarrier[i].offset = 0;
                fillBarrier[i].size = VK_WHOLE_SIZE;
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 2,
                                 fillBarrier, 0, nullptr);

            // --- prepare_indirect: counter[srcCountSlot] ->
            // counter[argsSlot..+2] -> the COMPUTE_SHADER -> DRAW_INDIRECT /
            // INDIRECT_COMMAND_READ barrier that
            // docs/superpowers/specs/2026-08-27-differentiable-renderer-design.md
            // section 3.1, "What actually guards those hand-written
            // barriers", documents as
            // load-bearing (wf_prepare_indirect's write of the dispatch-args
            // triple must be visible to vkCmdDispatchIndirect's read before
            // that read happens -- INDIRECT_COMMAND_READ, not HOST_READ or
            // SHADER_READ; this is the one barrier in this subsystem
            // anything here is proven to detect the absence of) -> scatter,
            // dispatched indirectly from the triple just made visible,
            // re-queuing into (dstQueueBase, dstCountSlot). Shared with
            // WavefrontLoop::recordCompactingStage and
            // runWavefrontIntersectProbe -- see recordIndirectSizedDispatch's
            // doc comment in wavefront_loop.hpp for the full account. ---
            recordIndirectSizedDispatch(cmd, buffers.counterBuffer(), srcCountSlot, prepareIndirect,
                                        scatter);

            // scatter's writes (state origin/dir/throughput/bounce, queue
            // dst ring, counter dstCountSlot, DebugDraws) must become
            // visible to what reads them next: this function's own host
            // readback of state/counter through WavefrontBuffers'
            // persistently-mapped pointers and of DebugDraws through this
            // function's own mapped buffer (HOST_READ), and the
            // vkCmdCopyBuffer below that pulls the dst queue ring out
            // (TRANSFER_READ).
            VkBufferMemoryBarrier postScatter[5]{};
            VkBuffer writtenBuffers[5] = {buffers.stateBuffer(), buffers.counterBuffer(),
                                          debugDrawsBuffer.buffer, envSamplesBuffer.buffer,
                                          neeSamplesBuffer.buffer};
            for (int i = 0; i < 5; ++i) {
                postScatter[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                postScatter[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                postScatter[i].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                postScatter[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                postScatter[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                postScatter[i].buffer = writtenBuffers[i];
                postScatter[i].offset = 0;
                postScatter[i].size = VK_WHOLE_SIZE;
            }
            VkBufferMemoryBarrier queueToTransfer{};
            queueToTransfer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            queueToTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            queueToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            queueToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            queueToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            queueToTransfer.buffer = buffers.queueBuffer();
            queueToTransfer.offset = 0;
            queueToTransfer.size = VK_WHOLE_SIZE;

            VkBufferMemoryBarrier postDispatch[6] = {postScatter[0], postScatter[1], postScatter[2],
                                                     postScatter[3], postScatter[4],
                                                     queueToTransfer};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 6, postDispatch, 0, nullptr);

            // Copy the dst queue ring (elements [dstQueueBase,
            // dstQueueBase+capacity)) into this function's own host-visible
            // buffer.
            VkBufferCopy region{};
            region.srcOffset = static_cast<VkDeviceSize>(dstQueueBase) * sizeof(uint32_t);
            region.dstOffset = 0;
            region.size = queueBytes;
            vkCmdCopyBuffer(cmd, buffers.queueBuffer(), queueReadback.buffer, 1, &region);

            VkBufferMemoryBarrier postCopy{};
            postCopy.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            postCopy.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            postCopy.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            postCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            postCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            postCopy.buffer = queueReadback.buffer;
            postCopy.offset = 0;
            postCopy.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                 0, nullptr, 1, &postCopy, 0, nullptr);
        });

        m_allocator.invalidateBuffer(queueReadback);
        const auto* mappedQueue = static_cast<const uint32_t*>(queueReadback.getMappedData());
        if (mappedQueue == nullptr) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: queue readback "
                                  "buffer not mapped, cannot read back\n");
            ok = false;
        } else {
            outQueueDst.assign(mappedQueue, mappedQueue + capacity);
        }

        m_allocator.invalidateBuffer(debugDrawsBuffer);
        const auto* mappedDebug = static_cast<const float*>(debugDrawsBuffer.getMappedData());
        if (mappedDebug == nullptr) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: debug draws buffer "
                                  "not mapped, cannot read back\n");
            ok = false;
        } else {
            outDebugDraws.assign(mappedDebug,
                                 mappedDebug + (static_cast<std::size_t>(capacity) *
                                                kDebugDrawFloats));
        }

        if (outEnvSamples != nullptr) {
            m_allocator.invalidateBuffer(envSamplesBuffer);
            const auto* mappedEnv = static_cast<const float*>(envSamplesBuffer.getMappedData());
            if (mappedEnv == nullptr) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: env samples "
                                      "buffer not mapped, cannot read back\n");
                ok = false;
            } else {
                outEnvSamples->assign(mappedEnv,
                                      mappedEnv + (static_cast<std::size_t>(capacity) * kEnvSampleFloats));
            }
        }

        if (outNeeSamples != nullptr) {
            m_allocator.invalidateBuffer(neeSamplesBuffer);
            const auto* mappedNee = static_cast<const float*>(neeSamplesBuffer.getMappedData());
            if (mappedNee == nullptr) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontScatterProbe: NEE samples "
                                      "buffer not mapped, cannot read back\n");
                ok = false;
            } else {
                outNeeSamples->assign(
                    mappedNee,
                    mappedNee + (static_cast<std::size_t>(capacity) * kNeeSampleFloats));
            }
        }
    }

    // --- Cleanup, reverse order. ---
    if (filmBuffer.isValid()) m_allocator.destroyBuffer(filmBuffer);
    if (neeSamplesBuffer.isValid()) m_allocator.destroyBuffer(neeSamplesBuffer);
    if (envSamplesBuffer.isValid()) m_allocator.destroyBuffer(envSamplesBuffer);
    if (debugDrawsBuffer.isValid()) m_allocator.destroyBuffer(debugDrawsBuffer);
    if (queueReadback.isValid()) m_allocator.destroyBuffer(queueReadback);
    scatter.destroy(m_device);
    prepareIndirect.destroy(m_device);
    if (shadowIndexBuffer.isValid()) m_allocator.destroyBuffer(shadowIndexBuffer);
    if (shadowVertexBuffer.isValid()) m_allocator.destroyBuffer(shadowVertexBuffer);

    return ok;
}

}  // namespace ohao::diff
