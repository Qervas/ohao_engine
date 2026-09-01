// The integrator-parity probe.
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
// Stage 0b-2b Task 6 -- the PARITY probe.
// ===========================================================================
//
// A caller-supplied scene driven through the SAME ohao::diff::WavefrontLoop
// the fused-loop probe uses, at one sample per pixel per run, once per seed.
// See the doc comment in gpu_probe_context.hpp for why this is a sibling of
// runWavefrontFusedLoopProbe rather than more parameters on it.
bool GpuProbeContext::runWavefrontParityProbe(
    WavefrontBuffers& buffers, uint32_t width, uint32_t height, uint32_t bounces,
    const WavefrontGenerateCamera& camera, std::span<const float> positions,
    std::span<const uint32_t> indices, float albedo, const WavefrontScatterMaterial& material,
    std::span<const uint32_t> iterationSeeds, std::vector<std::vector<float>>& outFilmPerSeed,
    std::vector<uint32_t>& outLiveCountPerSeed) {
    // Byte-identical to runWavefrontGenerateProbe's / runWavefrontFusedLoop-
    // Probe's GeneratePush (80 bytes, see those functions).
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

    outFilmPerSeed.clear();
    outLiveCountPerSeed.clear();

    const uint32_t capacity = buffers.layout().capacity();
    bool ok = capacity > 0 && buffers.stateBuffer() != VK_NULL_HANDLE &&
              buffers.queueBuffer() != VK_NULL_HANDLE && buffers.counterBuffer() != VK_NULL_HANDLE;
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: buffers not built\n");
        return false;
    }
    // Same dispatch-shape requirement as runWavefrontFusedLoopProbe, and for
    // the same reason: this probe's generate dispatch sets groupCountX only,
    // so one dispatch covers exactly one row of local_size_y = 8 pixels.
    if (height != kFusedLoopGenerateLocalY || width == 0u ||
        (width % kFusedLoopGenerateLocalX) != 0u || width * height != capacity || bounces == 0u) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontParityProbe: requires height == %u, width a "
                     "non-zero multiple of %u, width*height == capacity (%u) and bounces > 0; "
                     "got %ux%u, bounces %u\n",
                     kFusedLoopGenerateLocalY, kFusedLoopGenerateLocalX, capacity, width, height,
                     bounces);
        return false;
    }
    if (positions.size() < 9u || positions.size() % 3u != 0u || indices.size() < 3u ||
        indices.size() % 3u != 0u || iterationSeeds.empty()) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontParityProbe: needs at least one triangle "
                     "(3 floats per vertex, 3 indices per triangle) and at least one seed; got "
                     "%zu floats, %zu indices, %zu seeds\n",
                     positions.size(), indices.size(), iterationSeeds.size());
        return false;
    }

    // --- Scene. ONE triangle soup, bound to the primary trace (as
    // acceleration structure AND as wf_intersect.comp's vertex/index storage
    // buffers, which is how it recovers the geometric normal) and to
    // wf_scatter.comp's shadow rays. Not two.
    GpuBuffer vertexBuffer = m_allocator.createBufferFromSpan<float>(
        positions, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    GpuBuffer indexBuffer = m_allocator.createBufferFromSpan<uint32_t>(
        indices, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ok = vertexBuffer.isValid() && indexBuffer.isValid();
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: failed to create scene "
                              "vertex/index buffers\n");
    }

    RTAccelerationStructure accel;
    if (ok && !accel.init(m_device, m_physicalDevice, m_queue, m_queueFamily, m_commandPool,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: "
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
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: "
                                  "createBLASFromPositions failed\n");
            ok = false;
        }
    }
    if (ok) {
        accel.clearInstances();
        accel.addInstance(blas, glm::mat4(1.0f));
        runImmediate([&](VkCommandBuffer cmd) { accel.buildTLAS(cmd); });
        if (accel.getTLAS() == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: buildTLAS produced "
                                  "no TLAS\n");
            ok = false;
        }
    }

    // --- Sinks. debugDraws (3), envSamples (6) and neeSamples (7) are
    // allocated and bound but NEVER read back here: a descriptor set must
    // cover every binding the shader statically uses, and this probe's
    // subject is the film. They still go through record()'s
    // extraBarrierBuffers below, because every scatter dispatch in the fused
    // run overwrites the same per-path offsets in them and the loop's own
    // barriers name only state/queue/counter -- an unordered write is a
    // hazard whether or not anyone reads the result.
    GpuBuffer debugDrawsBuffer;
    GpuBuffer envSamplesBuffer;
    GpuBuffer neeSamplesBuffer;
    GpuBuffer filmBuffer;
    const uint32_t filmPixelCount = width * height;
    if (ok) {
        debugDrawsBuffer =
            m_allocator.createBuffer(static_cast<VkDeviceSize>(capacity) * kDebugDrawFloats *
                                         sizeof(float),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::GpuOnly);
        envSamplesBuffer =
            m_allocator.createBuffer(static_cast<VkDeviceSize>(capacity) * kEnvSampleFloats *
                                         sizeof(float),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::GpuOnly);
        neeSamplesBuffer = m_allocator.createBuffer(
            static_cast<VkDeviceSize>(capacity) * kNeeSampleFloats * sizeof(float),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::GpuOnly);
        // TRANSFER_DST as well as STORAGE: read-modify-written by atomicAdd,
        // so it is zeroed with vkCmdFillBuffer at the top of every run.
        filmBuffer = m_allocator.createBuffer(
            static_cast<VkDeviceSize>(filmPixelCount) * 3u * sizeof(float),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        if (!debugDrawsBuffer.isValid() || !envSamplesBuffer.isValid() ||
            !neeSamplesBuffer.isValid() || !filmBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: sink buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }

    WavefrontStage generate;
    WavefrontStage prepareIndirect;
    WavefrontStage intersect;
    WavefrontStage scatter;

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
    // Eleven, not ten: binding 10 is the gradient arena (Stage 1 Task 2).
    // See runWavefrontScatterProbe's note at its binding-10 bind for why the
    // probes that have no arena re-bind the film buffer there.
    const VkDescriptorType kScatterBindings[13] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
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
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   // 12: the Stage 2 Task 1 adjoint seed.
                                                   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};

    if (ok && !generate.build(m_device, "diff_wf_generate.comp.spv", kStateQueueCounter,
                              sizeof(GeneratePush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: generate build\n");
        ok = false;
    }
    if (ok && !prepareIndirect.build(m_device, "diff_wf_prepare_indirect.comp.spv", kCounterOnly,
                                     sizeof(WavefrontLoop::PrepareIndirectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: prepare_indirect build\n");
        ok = false;
    }
    if (ok && !intersect.build(m_device, "diff_wf_intersect.comp.spv", kIntersectBindings,
                               sizeof(WavefrontLoop::IntersectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: intersect build\n");
        ok = false;
    }
    if (ok && !scatter.build(m_device, "diff_wf_scatter.comp.spv", kScatterBindings,
                             sizeof(WavefrontLoop::ScatterPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: scatter build\n");
        ok = false;
    }

    if (ok) {
        const VkBuffer stateQueueCounter[3] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                               buffers.counterBuffer()};
        const VkBuffer counterOnly[1] = {buffers.counterBuffer()};
        const VkBuffer intersectBuffers[5] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                              buffers.counterBuffer(), vertexBuffer.buffer,
                                              indexBuffer.buffer};
        const VkBuffer scatterBuffers[8] = {
            buffers.stateBuffer(),       buffers.queueBuffer(),
            buffers.counterBuffer(),     debugDrawsBuffer.buffer,
            buffers.envMarginalBuffer(), buffers.envConditionalBuffer(),
            envSamplesBuffer.buffer,     neeSamplesBuffer.buffer};
        if (!generate.bindBuffers(m_device, stateQueueCounter) ||
            !prepareIndirect.bindBuffers(m_device, counterOnly) ||
            !intersect.bindBuffers(m_device, intersectBuffers) ||
            !intersect.bindAccelerationStructure(m_device, 5, accel.getTLAS()) ||
            !scatter.bindBuffers(m_device, scatterBuffers) ||
            // The SAME TLAS the path rays trace. Deliberately not a second
            // one -- see this function's doc comment.
            !scatter.bindAccelerationStructure(m_device, 8, accel.getTLAS()) ||
            !scatter.bindStorageBuffer(m_device, 9, filmBuffer.buffer) ||
            // Binding 10, the gradient arena: none here, so the film is
            // re-bound and ScatterPush::gradArenaFloats stays 0. See
            // runWavefrontScatterProbe's note at the same call.
            !scatter.bindStorageBuffer(m_device, 10, filmBuffer.buffer) ||
            // BINDING 11, THE EMISSION-TEXTURE PRIMAL (Stage 1 Task 5). The
            // film again, for binding 10's reason exactly: this probe
            // configures no texture (ScatterPush::emissionTexWidth stays 0,
            // which makes the traversal read the uniform `emission` scalar
            // and never touch this buffer), and a stray read here is
            // harmless while a stray WRITE -- which the shader's `readonly`
            // already forbids -- would land somewhere three independent
            // film checks would notice.
            !scatter.bindStorageBuffer(m_device, 11, filmBuffer.buffer) ||
            !scatter.bindStorageBuffer(m_device, 12, filmBuffer.buffer)) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: descriptor binding\n");
            ok = false;
        }
    }

    if (ok) {
        GeneratePush genPush{};
        for (int i = 0; i < 3; ++i) {
            genPush.origin[i] = camera.origin[i];
            genPush.forward[i] = camera.forward[i];
            genPush.right[i] = camera.right[i];
            genPush.up[i] = camera.up[i];
        }
        genPush.width = width;
        genPush.height = height;
        genPush.tanHalfFov = camera.tanHalfFov;
        genPush.capacity = capacity;
        generate.setPushConstants(&genPush, sizeof(genPush));
        generate.setGroupCount(WavefrontStage::Fixed{width / kFusedLoopGenerateLocalX});

        WavefrontLoop loop;
        loop.setGenerate(generate);
        loop.setPrepareIndirect(prepareIndirect);
        loop.setIntersect(intersect);
        loop.setScatter(scatter);

        outFilmPerSeed.resize(iterationSeeds.size());
        outLiveCountPerSeed.resize(iterationSeeds.size());

        const WavefrontLoop::Ring finalRing = WavefrontLoop::finalLiveRing(capacity, bounces);

        for (std::size_t s = 0; ok && s < iterationSeeds.size(); ++s) {
            // Named-field assignment, not a positional aggregate: Config has
            // grown fields BETWEEN albedo and iterationSeed twice on this
            // branch already.
            WavefrontLoop::Config loopConfig;
            loopConfig.albedo = albedo;
            loopConfig.roughness = material.roughness;
            loopConfig.metallic = material.metallic;
            loopConfig.specularWeight = material.specularWeight;
            loopConfig.filmPixelCount = filmPixelCount;
            loopConfig.iterationSeed = iterationSeeds[s];
            loop.setConfig(loopConfig);

            runImmediate([&](VkCommandBuffer cmd) {
                buffers.zero(cmd);

                // The film is caller-owned, so record() neither zeroes it nor
                // knows its size. It is READ-modify-written by atomicAdd, so
                // an un-zeroed film accumulates onto the previous seed's
                // total; dstAccessMask includes SHADER_WRITE because the
                // first thing the shader does to these bytes is that
                // read-modify-write.
                vkCmdFillBuffer(cmd, filmBuffer.buffer, 0, VK_WHOLE_SIZE, 0u);
                VkBufferMemoryBarrier filmZeroBarrier{};
                filmZeroBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                filmZeroBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                filmZeroBarrier.dstAccessMask =
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                filmZeroBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                filmZeroBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                filmZeroBarrier.buffer = filmBuffer.buffer;
                filmZeroBarrier.offset = 0;
                filmZeroBarrier.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                                     &filmZeroBarrier, 0, nullptr);

                // Every caller-owned buffer a scatter dispatch WRITES. The
                // film is the one that costs radiance rather than a stale
                // diagnostic if it is omitted: bounce k+1 both reads and
                // writes the bytes bounce k wrote, and barrier (7) inside
                // WavefrontLoop::recordCompactingStage -- which covers only
                // the buffers named in this span -- is the only thing that
                // makes that read see that write. See wf_scatter.comp's note
                // at the atomicAdd. The three unread sinks are here for the
                // same structural reason, not because anything reads them.
                const VkBuffer loopExtras[4] = {debugDrawsBuffer.buffer, envSamplesBuffer.buffer,
                                                neeSamplesBuffer.buffer, filmBuffer.buffer};
                loop.record(cmd, buffers, bounces, loopExtras);

                // Host-read availability for exactly the two buffers this
                // function reads back: the counter (through
                // buffers.readbackCounter) and the film (through a mapped
                // pointer). vkQueueWaitIdle does not make writes visible in
                // the host domain, and vmaInvalidateAllocation only handles
                // the CPU cache side. Synchronization validation has been
                // MEASURED silent on this subsystem, so a green run is not
                // evidence that this list is complete -- it is complete
                // because it names every buffer read below and nothing else
                // is read.
                VkBufferMemoryBarrier toHost[2]{};
                const VkBuffer hostRead[2] = {buffers.counterBuffer(), filmBuffer.buffer};
                for (int i = 0; i < 2; ++i) {
                    toHost[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    toHost[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    toHost[i].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    toHost[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost[i].buffer = hostRead[i];
                    toHost[i].offset = 0;
                    toHost[i].size = VK_WHOLE_SIZE;
                }
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 2, toHost, 0,
                                     nullptr);
            });

            outLiveCountPerSeed[s] = buffers.readbackCounter(m_allocator, finalRing.countSlot);

            m_allocator.invalidateBuffer(filmBuffer);
            const auto* mappedFilm = static_cast<const float*>(filmBuffer.getMappedData());
            if (mappedFilm == nullptr) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontParityProbe: film buffer not "
                                      "mapped, cannot read back\n");
                ok = false;
                break;
            }
            outFilmPerSeed[s].assign(mappedFilm,
                                     mappedFilm + (static_cast<std::size_t>(filmPixelCount) * 3u));
        }
    }

    scatter.destroy(m_device);
    intersect.destroy(m_device);
    prepareIndirect.destroy(m_device);
    generate.destroy(m_device);
    if (filmBuffer.isValid()) m_allocator.destroyBuffer(filmBuffer);
    if (neeSamplesBuffer.isValid()) m_allocator.destroyBuffer(neeSamplesBuffer);
    if (envSamplesBuffer.isValid()) m_allocator.destroyBuffer(envSamplesBuffer);
    if (debugDrawsBuffer.isValid()) m_allocator.destroyBuffer(debugDrawsBuffer);
    if (vertexBuffer.isValid()) m_allocator.destroyBuffer(vertexBuffer);
    if (indexBuffer.isValid()) m_allocator.destroyBuffer(indexBuffer);

    return ok;
}

}  // namespace ohao::diff
