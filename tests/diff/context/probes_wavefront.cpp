// The per-stage wavefront probes: generate, the state-layout mapping, and the three intersect entry points.
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

bool GpuProbeContext::runWavefrontGenerateProbe(WavefrontBuffers& buffers, uint32_t width,
                                                uint32_t height,
                                                const WavefrontGenerateCamera& camera,
                                                std::vector<uint32_t>& outQueue0) {
    // Push constants: must byte-match shaders/diff/wf_generate.comp's Push
    // block exactly (80 bytes -- same four vec3+pad quads as
    // visibility_probe.comp's Push, with a trailing width/height/tanHalfFov/
    // capacity quad in place of visibility_probe's width/height/tanHalfFov/pad).
    struct PushConstants {
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
    static_assert(sizeof(PushConstants) == 80,
                 "PushConstants must match wf_generate.comp's Push block layout");

    outQueue0.clear();

    const uint32_t capacity = buffers.layout().capacity();
    bool ok = capacity > 0 && buffers.stateBuffer() != VK_NULL_HANDLE &&
              buffers.queueBuffer() != VK_NULL_HANDLE && buffers.counterBuffer() != VK_NULL_HANDLE;
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGenerateProbe: buffers not built\n");
        return false;
    }

    // --- state (0), queue (1), counter (2), via the wavefront execution
    // library (ComputePipeline/WavefrontStage) rather than a hand-rolled
    // shader-module -> layout -> pipeline -> pool -> set sequence. ---
    WavefrontStage generate;
    const VkDescriptorType bindingTypes[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    ok = generate.build(m_device, "diff_wf_generate.comp.spv", bindingTypes, sizeof(PushConstants));
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGenerateProbe: generate build\n");
        return false;
    }
    const VkBuffer stateQueueCounter[3] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                           buffers.counterBuffer()};
    ok = generate.bindBuffers(m_device, stateQueueCounter);
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGenerateProbe: bindBuffers failed\n");
        generate.destroy(m_device);
        return false;
    }

    // --- Readback buffer for queue 0 (host-visible, this function's own) ---
    GpuBuffer queueReadback;
    const VkDeviceSize queue0Bytes = static_cast<VkDeviceSize>(capacity) * sizeof(uint32_t);
    if (ok) {
        queueReadback = m_allocator.createBuffer(queue0Bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                 AllocationUsage::GpuToCpu,
                                                 /*persistentlyMapped=*/true);
        if (!queueReadback.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGenerateProbe: queue readback "
                                  "buffer allocation failed\n");
            ok = false;
        }
    }

    if (ok) {
        PushConstants push{};
        push.origin[0] = camera.origin[0]; push.origin[1] = camera.origin[1]; push.origin[2] = camera.origin[2];
        push.forward[0] = camera.forward[0]; push.forward[1] = camera.forward[1]; push.forward[2] = camera.forward[2];
        push.right[0] = camera.right[0]; push.right[1] = camera.right[1]; push.right[2] = camera.right[2];
        push.up[0] = camera.up[0]; push.up[1] = camera.up[1]; push.up[2] = camera.up[2];
        push.width = width;
        push.height = height;
        push.tanHalfFov = camera.tanHalfFov;
        push.capacity = capacity;
        generate.setPushConstants(&push, sizeof(push));

        // Genuinely 2-D: wf_generate.comp is local_size(8,8), and this probe
        // covers a width x height pixel grid, not just local_size_y rows of
        // it -- so the 3-D-widened WavefrontStage::Fixed is used here (see
        // wavefront_stage.hpp), unlike the fused-loop probe's 1-D
        // Fixed{width/8} (which is restricted to height == 8).
        const uint32_t groupsX = (width + 7) / 8;
        const uint32_t groupsY = (height + 7) / 8;
        generate.setGroupCount(WavefrontStage::Fixed{groupsX, groupsY, 1});

        runImmediate([&](VkCommandBuffer cmd) {
            generate.record(cmd);

            // The dispatch wrote (shader-write) the state and queue buffers
            // and read-modify-wrote (atomicAdd) the counter buffer. Two
            // different consumers follow: the caller reads state/counter
            // back through WavefrontBuffers' own persistently-mapped host
            // pointers (needs HOST_READ), and this function copies queue 0
            // out via vkCmdCopyBuffer (needs TRANSFER_READ) because
            // WavefrontBuffers exposes only a raw VkBuffer for it. One
            // barrier naming both destination stages/accesses covers both.
            VkBufferMemoryBarrier postDispatch[3]{};
            VkBuffer writtenBuffers[3] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                          buffers.counterBuffer()};
            for (int i = 0; i < 3; ++i) {
                postDispatch[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                postDispatch[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                postDispatch[i].dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
                postDispatch[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                postDispatch[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                postDispatch[i].buffer = writtenBuffers[i];
                postDispatch[i].offset = 0;
                postDispatch[i].size = VK_WHOLE_SIZE;
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 3, postDispatch, 0, nullptr);

            // Copy queue 0 (the first `capacity` uints of the queue buffer)
            // into this function's own host-visible buffer.
            VkBufferCopy region{};
            region.srcOffset = 0;
            region.dstOffset = 0;
            region.size = queue0Bytes;
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
        const auto* mapped = static_cast<const uint32_t*>(queueReadback.getMappedData());
        if (mapped == nullptr) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGenerateProbe: queue readback "
                                  "buffer not mapped, cannot read back\n");
            ok = false;
        } else {
            outQueue0.assign(mapped, mapped + capacity);
        }
    }

    // --- Cleanup ---
    if (queueReadback.isValid()) m_allocator.destroyBuffer(queueReadback);
    generate.destroy(m_device);

    return ok;
}

bool GpuProbeContext::runWavefrontLayoutProbe(WavefrontBuffers& buffers) {
    struct PushConstants {
        uint32_t capacity;
    };
    const PushConstants push{buffers.layout().capacity()};
    // Single storage buffer (state, binding 0) + push constants + one
    // invocation: exactly what dispatchStorageBufferCompute already does for
    // runAtomicProbe/runRngParityProbe.
    return dispatchStorageBufferCompute("diff_wf_layout_probe.comp.spv", buffers.stateBuffer(),
                                        &push, sizeof(push), /*groupCountX=*/1u);
}

bool GpuProbeContext::runWavefrontIntersectProbe(WavefrontBuffers& buffers, float planeDistance,
                                                 float quadMinY, std::vector<uint32_t>& outQueue1) {
    // --- Quad geometry: x in [-1,1], y in [quadMinY,1] at z = -planeDistance,
    // the same shape runVisibilityProbe builds. ---
    const float d = planeDistance;
    const std::array<float, 12> positions = {
        -1.0f, quadMinY, -d,
         1.0f, quadMinY, -d,
         1.0f,  1.0f,    -d,
        -1.0f,  1.0f,    -d,
    };
    const std::array<uint32_t, 6> indices = {0, 1, 2, 0, 2, 3};
    return runWavefrontIntersectOnGeometry(buffers, std::span<const float>(positions),
                                           std::span<const uint32_t>(indices), outQueue1);
}

bool GpuProbeContext::runWavefrontBoxIntersectProbe(WavefrontBuffers& buffers, float halfExtent,
                                                    std::vector<uint32_t>& outQueue1) {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    buildAxisAlignedBoxGeometry(halfExtent, positions, indices);
    return runWavefrontIntersectOnGeometry(buffers, std::span<const float>(positions),
                                           std::span<const uint32_t>(indices), outQueue1);
}

bool GpuProbeContext::runWavefrontIntersectOnGeometry(WavefrontBuffers& buffers,
                                                      std::span<const float> positions,
                                                      std::span<const uint32_t> indices,
                                                      std::vector<uint32_t>& outQueue1) {
    outQueue1.clear();

    const uint32_t capacity = buffers.layout().capacity();
    bool ok = capacity > 0 && buffers.stateBuffer() != VK_NULL_HANDLE &&
              buffers.queueBuffer() != VK_NULL_HANDLE && buffers.counterBuffer() != VK_NULL_HANDLE;
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: buffers not built\n");
        return false;
    }

    // The vertex and index buffers are ALSO storage buffers, not merely
    // acceleration-structure build input: wf_intersect.comp reads the hit
    // triangle's three vertices back out of them (bindings 3 and 4) to
    // compute the hit's geometric normal. There is no other way to recover
    // it -- a ray query reports a primitive index and barycentrics, never a
    // normal.
    GpuBuffer vertexBuffer = m_allocator.createBufferFromSpan<float>(
        positions,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer indexBuffer = m_allocator.createBufferFromSpan<uint32_t>(
        indices,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    ok = vertexBuffer.isValid() && indexBuffer.isValid();
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: failed to create quad "
                              "vertex/index buffers\n");
    }

    RTAccelerationStructure accel;
    if (ok && !accel.init(m_device, m_physicalDevice, m_queue, m_queueFamily, m_commandPool,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: "
                              "RTAccelerationStructure::init failed\n");
        ok = false;
    }

    BlasHandle blas = INVALID_BLAS;
    if (ok) {
        runImmediate([&](VkCommandBuffer cmd) {
            blas = accel.createBLASFromPositions(vertexBuffer.buffer,
                                                 static_cast<uint32_t>(positions.size() / 3),
                                                 indexBuffer.buffer,
                                                 static_cast<uint32_t>(indices.size()),
                                                 /*indexByteOffset=*/0, cmd);
        });
        if (blas == INVALID_BLAS) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: "
                                  "createBLASFromPositions failed\n");
            ok = false;
        }
    }

    if (ok) {
        accel.clearInstances();
        accel.addInstance(blas, glm::mat4(1.0f));
        runImmediate([&](VkCommandBuffer cmd) { accel.buildTLAS(cmd); });
        if (accel.getTLAS() == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: buildTLAS produced "
                                  "no TLAS\n");
            ok = false;
        }
    }

    // --- prepare_indirect (counter only) and intersect (state, queue,
    // counter, AS), via the wavefront execution library rather than a
    // hand-rolled shader-module -> layout -> pipeline -> pool -> set
    // sequence for each. Reuses WavefrontLoop::PrepareIndirectPush/
    // IntersectPush -- byte-identical to this function's former private
    // PrepPush/IntersectPush -- rather than redeclaring them. ---
    WavefrontStage prepareIndirect;
    WavefrontStage intersect;
    const VkDescriptorType counterOnly[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    // state, queues, counters, vertex positions, triangle indices, TLAS.
    // The acceleration structure is LAST so that bindBuffers -- which writes
    // a contiguous prefix of storage-buffer bindings starting at 0 -- can
    // cover all five buffers in one call.
    const VkDescriptorType intersectBindingTypes[6] = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};

    if (ok && !prepareIndirect.build(m_device, "diff_wf_prepare_indirect.comp.spv", counterOnly,
                                     sizeof(WavefrontLoop::PrepareIndirectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: prepare_indirect "
                              "build\n");
        ok = false;
    }
    if (ok && !intersect.build(m_device, "diff_wf_intersect.comp.spv", intersectBindingTypes,
                               sizeof(WavefrontLoop::IntersectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: intersect build\n");
        ok = false;
    }

    if (ok) {
        const VkBuffer counterOnlyBuf[1] = {buffers.counterBuffer()};
        const VkBuffer intersectBuffers[5] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                              buffers.counterBuffer(), vertexBuffer.buffer,
                                              indexBuffer.buffer};
        if (!prepareIndirect.bindBuffers(m_device, counterOnlyBuf) ||
            !intersect.bindBuffers(m_device, intersectBuffers) ||
            !intersect.bindAccelerationStructure(m_device, 5, accel.getTLAS())) {
            std::fprintf(stderr,
                         "[GpuProbeContext] runWavefrontIntersectProbe: descriptor binding\n");
            ok = false;
        }
    }

    // --- Readback buffer for queue ring 1 (host-visible, this function's own) ---
    GpuBuffer queueReadback;
    const VkDeviceSize queue1Bytes = static_cast<VkDeviceSize>(capacity) * sizeof(uint32_t);
    if (ok) {
        queueReadback = m_allocator.createBuffer(queue1Bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                 AllocationUsage::GpuToCpu,
                                                 /*persistentlyMapped=*/true);
        if (!queueReadback.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: queue readback "
                                  "buffer allocation failed\n");
            ok = false;
        }
    }

    if (ok) {
        const WavefrontLoop::IntersectPush intersectPush{capacity,
                                                          /*srcQueueBase=*/0u,
                                                          WavefrontBuffers::kCurrentCountSlot,
                                                          /*dstQueueBase=*/capacity,
                                                          WavefrontBuffers::kNextCountSlot,
                                                          WavefrontBuffers::kCanarySlot};
        intersect.setPushConstants(&intersectPush, sizeof(intersectPush));

        runImmediate([&](VkCommandBuffer cmd) {
            // --- prepare_indirect: counter[kCurrentCountSlot] ->
            // counter[argsSlot..+2] -> the COMPUTE_SHADER -> DRAW_INDIRECT /
            // INDIRECT_COMMAND_READ barrier ordering that write before
            // vkCmdDispatchIndirect reads it (the one barrier in this
            // subsystem anything here is proven to detect the absence of --
            // the measurement is recorded in
            // docs/superpowers/specs/2026-08-27-differentiable-renderer-design.md
            // section 3.1, "What actually guards those hand-written
            // barriers") -> intersect, dispatched indirectly from
            // the triple just made visible. Shared with
            // WavefrontLoop::recordCompactingStage and
            // runWavefrontScatterProbe -- see recordIndirectSizedDispatch's
            // doc comment in wavefront_loop.hpp for the full account of why
            // each piece is required, including the kIndirectArgsSlot
            // disjointness invariant the lone INDIRECT_COMMAND_READ
            // dstAccessMask depends on. ---
            recordIndirectSizedDispatch(cmd, buffers.counterBuffer(),
                                        WavefrontBuffers::kCurrentCountSlot, prepareIndirect,
                                        intersect);

            // intersect's writes (state Alive/HitT, queue ring 1, counter
            // slots next-count/canary) must become visible to what reads
            // them next: this function's own host readback of state/counter
            // through WavefrontBuffers' persistently-mapped pointers
            // (HOST_READ), and the vkCmdCopyBuffer below that pulls queue
            // ring 1 out into a buffer this function owns (TRANSFER_READ).
            VkBufferMemoryBarrier postIntersect[2]{};
            VkBuffer writtenBuffers[2] = {buffers.stateBuffer(), buffers.counterBuffer()};
            for (int i = 0; i < 2; ++i) {
                postIntersect[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                postIntersect[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                postIntersect[i].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                postIntersect[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                postIntersect[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                postIntersect[i].buffer = writtenBuffers[i];
                postIntersect[i].offset = 0;
                postIntersect[i].size = VK_WHOLE_SIZE;
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

            VkBufferMemoryBarrier postDispatch[3] = {postIntersect[0], postIntersect[1],
                                                     queueToTransfer};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 3, postDispatch, 0, nullptr);

            // Copy queue ring 1 (elements [capacity, 2*capacity)) into this
            // function's own host-visible buffer.
            VkBufferCopy region{};
            region.srcOffset = static_cast<VkDeviceSize>(capacity) * sizeof(uint32_t);
            region.dstOffset = 0;
            region.size = queue1Bytes;
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
        const auto* mapped = static_cast<const uint32_t*>(queueReadback.getMappedData());
        if (mapped == nullptr) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontIntersectProbe: queue readback "
                                  "buffer not mapped, cannot read back\n");
            ok = false;
        } else {
            outQueue1.assign(mapped, mapped + capacity);
        }
    }

    // --- Cleanup, reverse order. ---
    if (queueReadback.isValid()) m_allocator.destroyBuffer(queueReadback);
    intersect.destroy(m_device);
    prepareIndirect.destroy(m_device);
    if (vertexBuffer.isValid()) m_allocator.destroyBuffer(vertexBuffer);
    if (indexBuffer.isValid()) m_allocator.destroyBuffer(indexBuffer);

    return ok;
}

}  // namespace ohao::diff
