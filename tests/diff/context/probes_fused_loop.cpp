// The fused multi-bounce loop probe.
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

bool GpuProbeContext::runWavefrontFusedLoopProbe(
    WavefrontBuffers& buffers, uint32_t width, uint32_t height, uint32_t maxBounces, float albedo,
    uint32_t iterationSeed, std::vector<std::vector<float>>& outDrawsPerBounce,
    std::vector<uint32_t>& outLiveCountPerRun, std::vector<uint32_t>& outFinalQueue,
    std::vector<float>* outEnvSamples, std::vector<float>* outNeeSamples,
    bool unoccludedShadowRays, std::vector<std::vector<float>>* outNeeSamplesPerRun,
    std::vector<std::vector<float>>* outFilmPerRun) {
    // Push constants for wf_generate.comp -- byte-identical to
    // runWavefrontGenerateProbe's (80 bytes, see that function's comment).
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

    outDrawsPerBounce.clear();
    outLiveCountPerRun.clear();
    outFinalQueue.clear();
    if (outNeeSamples != nullptr) outNeeSamples->clear();
    if (outNeeSamplesPerRun != nullptr) outNeeSamplesPerRun->clear();
    if (outFilmPerRun != nullptr) outFilmPerRun->clear();

    const uint32_t capacity = buffers.layout().capacity();
    bool ok = capacity > 0 && buffers.stateBuffer() != VK_NULL_HANDLE &&
              buffers.queueBuffer() != VK_NULL_HANDLE && buffers.counterBuffer() != VK_NULL_HANDLE;
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: buffers not built\n");
        return false;
    }
    // requires height == kFusedLoopGenerateLocalY: WavefrontStage::Fixed can
    // dispatch a genuine 3-D group count now (see kFusedLoopGenerateLocalY's
    // comment above), but this probe's own generate dispatch still only sets
    // groupCountX and leaves groupCountY/Z at 1, so one dispatch covers
    // exactly one row of local_size_y=8 pixels. This guard's height == 8
    // requirement is therefore still necessary, not merely historical -- but
    // it is NOT changed here even though the underlying 1-D limitation is
    // gone, because this probe's expected values (throughput, per-bounce
    // PathRng parity, live counts) are all calibrated to a single 512-path
    // run at this resolution; widening the dispatch to cover a genuine
    // 64x48 image is a possible follow-up, not done in this change.
    if (height != kFusedLoopGenerateLocalY || width == 0u ||
        (width % kFusedLoopGenerateLocalX) != 0u || width * height != capacity ||
        maxBounces == 0u) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontFusedLoopProbe: requires height == %u "
                     "(wf_generate.comp's local_size_y -- this probe's generate dispatch is "
                     "1-D, one row of pixels per group), width a non-zero multiple of %u, "
                     "width*height == capacity (%u), and maxBounces > 0; got %ux%u, "
                     "maxBounces %u\n",
                     kFusedLoopGenerateLocalY, kFusedLoopGenerateLocalX, capacity, width, height,
                     maxBounces);
        return false;
    }
    // This scene's "every path survives every bounce" guarantee also needs
    // maxBounces >= 1 (a zero-bounce run has nothing to guarantee), which is
    // exactly the condition already checked and reported above, alongside
    // this probe's other dispatch-shape requirements -- so it is not
    // repeated here. Every other hypothesis of the survival induction (see
    // the scene-constants header above) is a compile-time constant and is
    // enforced by the static_asserts just above this function, which fail
    // the BUILD, unconditionally, rather than only when this probe runs.

    // --- Scene: the closed box derived above. ---
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    buildAxisAlignedBoxGeometry(kFusedLoopBoxHalfExtent, positions, indices);

    // Storage-buffer usage as well as build input: wf_intersect.comp reads
    // the hit triangle's vertices back out of these to compute its geometric
    // normal (bindings 3 and 4).
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
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: failed to create "
                              "box vertex/index buffers\n");
    }

    RTAccelerationStructure accel;
    if (ok && !accel.init(m_device, m_physicalDevice, m_queue, m_queueFamily, m_commandPool,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: "
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
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: "
                                  "createBLASFromPositions failed\n");
            ok = false;
        }
    }
    if (ok) {
        accel.clearInstances();
        accel.addInstance(blas, glm::mat4(1.0f));
        runImmediate([&](VkCommandBuffer cmd) { accel.buildTLAS(cmd); });
        if (accel.getTLAS() == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: buildTLAS produced "
                                  "no TLAS\n");
            ok = false;
        }
    }

    // --- The SHADOW acceleration structure, when the caller asked for the
    // path rays and the shadow rays to see different scenes. See this
    // function's doc comment for why that rig exists at all: it is the only
    // way to have every path survive every bounce (which needs a closed
    // body) AND a nonzero direct-lighting contribution at every bounce
    // (which needs shadow rays that escape). Same "no occluders" expression
    // runWavefrontScatterProbe uses -- one triangle a million units out,
    // three orders of magnitude past the shader's kShadowTMax of 1000 --
    // because an acceleration-structure descriptor cannot be
    // VK_NULL_HANDLE without the nullDescriptor feature this context does
    // not enable.
    //
    // With unoccludedShadowRays false (the default, and what checks 27/28
    // run) NONE of this is built and scatter's binding 8 gets the box, i.e.
    // exactly the previous behaviour.
    static constexpr float kUnreachable = 1.0e6f;
    static const std::array<float, 9> kUnreachableVertices = {
        kUnreachable,        kUnreachable,        kUnreachable,
        kUnreachable + 1.0f, kUnreachable,        kUnreachable,
        kUnreachable,        kUnreachable + 1.0f, kUnreachable};
    static const std::array<uint32_t, 3> kUnreachableIndices = {0, 1, 2};
    GpuBuffer shadowVertexBuffer;
    GpuBuffer shadowIndexBuffer;
    RTAccelerationStructure shadowAccel;
    if (ok && unoccludedShadowRays) {
        shadowVertexBuffer = m_allocator.createBufferFromSpan<float>(
            std::span<const float>(kUnreachableVertices),
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        shadowIndexBuffer = m_allocator.createBufferFromSpan<uint32_t>(
            std::span<const uint32_t>(kUnreachableIndices),
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!shadowVertexBuffer.isValid() || !shadowIndexBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: shadow scene "
                                  "vertex/index buffer allocation failed\n");
            ok = false;
        }
        if (ok && !shadowAccel.init(m_device, m_physicalDevice, m_queue, m_queueFamily,
                                    m_commandPool, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: shadow "
                                  "RTAccelerationStructure::init failed\n");
            ok = false;
        }
        if (ok) {
            BlasHandle shadowBlas = INVALID_BLAS;
            runImmediate([&](VkCommandBuffer cmd) {
                shadowBlas = shadowAccel.createBLASFromPositions(
                    shadowVertexBuffer.buffer,
                    static_cast<uint32_t>(kUnreachableVertices.size() / 3),
                    shadowIndexBuffer.buffer, static_cast<uint32_t>(kUnreachableIndices.size()),
                    /*indexByteOffset=*/0, cmd);
            });
            if (shadowBlas == INVALID_BLAS) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: shadow scene "
                                      "createBLASFromPositions failed\n");
                ok = false;
            } else {
                shadowAccel.clearInstances();
                shadowAccel.addInstance(shadowBlas, glm::mat4(1.0f));
                runImmediate([&](VkCommandBuffer cmd) { shadowAccel.buildTLAS(cmd); });
                if (shadowAccel.getTLAS() == VK_NULL_HANDLE) {
                    std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: shadow "
                                          "scene buildTLAS produced no TLAS\n");
                    ok = false;
                }
            }
        }
    }
    const VkAccelerationStructureKHR scatterTLAS =
        unoccludedShadowRays ? shadowAccel.getTLAS() : accel.getTLAS();

    // --- Buffers this function owns: the live queue ring readback (the queue
    // is only exposed as a raw VkBuffer, so it has to be copied out) and
    // wf_scatter.comp's probe-only DebugDraws sink (allocated here, never
    // shared, so it is mapped directly). ---
    GpuBuffer queueReadback;
    const VkDeviceSize queueBytes = static_cast<VkDeviceSize>(capacity) * sizeof(uint32_t);
    if (ok) {
        queueReadback = m_allocator.createBuffer(queueBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                 AllocationUsage::GpuToCpu,
                                                 /*persistentlyMapped=*/true);
        if (!queueReadback.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: queue readback "
                                  "buffer allocation failed\n");
            ok = false;
        }
    }
    GpuBuffer debugDrawsBuffer;
    const VkDeviceSize debugDrawsBytes =
        static_cast<VkDeviceSize>(capacity) * kDebugDrawFloats * sizeof(float);
    if (ok) {
        debugDrawsBuffer =
            m_allocator.createBuffer(debugDrawsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                     AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        if (!debugDrawsBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: debug draws buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }
    // wf_scatter.comp's environment-sample sink (binding 6). The descriptor
    // set needs it bound whether or not the caller asked to read it back --
    // every scatter dispatch in the fused loop writes it, so it goes into
    // loopExtras below for exactly the reason debugDrawsBuffer does -- and it
    // is read back into outEnvSamples when non-null (see this function's doc
    // comment: this is the ONLY probe that exercises WavefrontLoop::record's
    // OWN fill of ScatterPush's envWidth/envHeight/envIntegral tail, as
    // opposed to runWavefrontScatterProbe's hand-filled push constants, so it
    // is also the only probe that can observe a bug in that fill). The
    // chi-squared check (24-26 in diff_gpu_probe.cpp) still runs against the
    // stage-by-stage scatter probe, which can vary the seed per dispatch;
    // this sink exists for a different purpose (check 27).
    GpuBuffer envSamplesBuffer;
    const VkDeviceSize envSamplesBytes =
        static_cast<VkDeviceSize>(capacity) * kEnvSampleFloats * sizeof(float);
    if (ok) {
        envSamplesBuffer =
            m_allocator.createBuffer(envSamplesBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                     AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        if (!envSamplesBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: env samples buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }
    // wf_scatter.comp's next-event sink (binding 7). Written at a fixed
    // pathIndex*kNeeSampleFloats offset by EVERY scatter dispatch in the
    // fused run, so -- exactly like debugDrawsBuffer and envSamplesBuffer --
    // it belongs in loopExtras below, and only the LAST bounce's write
    // survives in it.
    GpuBuffer neeSamplesBuffer;
    const VkDeviceSize neeSamplesBytes =
        static_cast<VkDeviceSize>(capacity) * kNeeSampleFloats * sizeof(float);
    if (ok) {
        neeSamplesBuffer =
            m_allocator.createBuffer(neeSamplesBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                     AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        if (!neeSamplesBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: NEE samples buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }
    // wf_scatter.comp's FILM (binding 9, Stage 0b-2b Task 5): 3 floats per
    // PIXEL. This probe runs one sample per pixel over width*height pixels
    // and capacity == width*height (checked above), so the film is
    // capacity*3 floats -- but it is sized from the pixel count on purpose,
    // because that is what it is, and a probe that one day runs several
    // samples per pixel must not silently over-allocate instead of failing.
    //
    // TRANSFER_DST as well as STORAGE: it is zeroed with vkCmdFillBuffer at
    // the top of every run's command buffer (see below). Unlike the three
    // probe sinks above, which every dispatch overwrites wholesale, this one
    // is READ-MODIFY-WRITTEN, so a stale prior value is not overwritten --
    // it is added to.
    const uint32_t filmPixelCount = width * height;
    GpuBuffer filmBuffer;
    const VkDeviceSize filmBytes = static_cast<VkDeviceSize>(filmPixelCount) * 3u * sizeof(float);
    if (ok) {
        filmBuffer = m_allocator.createBuffer(
            filmBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        if (!filmBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: film buffer "
                                  "allocation failed\n");
            ok = false;
        }
    }

    // --- The four stages. Built exactly once each, because this probe has
    // no reason to rebuild one. ComputePipeline::build IS safe to call again
    // -- commit e2f88e7 gave it a re-entrancy guard (compute_pipeline.cpp:
    // `if (m_device != VK_NULL_HANDLE) destroy(m_device);`), so a second
    // build() destroys the first build's objects rather than leaking them.
    // Building once here is a statement about this probe, not a constraint
    // to work around. ---
    WavefrontStage generate;
    WavefrontStage prepareIndirect;
    WavefrontStage intersect;
    WavefrontStage scatter;

    const VkDescriptorType kStateQueueCounter[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const VkDescriptorType kCounterOnly[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    // state, queues, counters, vertex positions, triangle indices, TLAS --
    // the acceleration structure last, so bindBuffers can write all five
    // storage buffers as one contiguous prefix.
    const VkDescriptorType kIntersectBindings[6] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    // state, queues, counters, debug draws, env marginal CDF, env
    // conditional CDF, env samples, NEE samples, TLAS --
    // wf_scatter.comp's bindings 0..9. The eight storage buffers 0-7 are one
    // contiguous prefix bindBuffers can write; the acceleration structure at
    // 8 and the film at 9 are bound one at a time after it.
    //
    // Binding 8 is by DEFAULT the SAME TLAS intersect traces against:
    // next-event estimation's shadow rays and the path's own rays see one
    // scene, which is what makes check 28's "inside a closed box every
    // shadow ray is occluded" a statement about the scene the paths are
    // actually in. `unoccludedShadowRays` is the one documented exception --
    // see this function's doc comment for the film check that needs it and
    // why nothing about the estimator is concluded from such a run.
    // ... and the film at 9, after the acceleration structure, so it is
    // bound with bindStorageBuffer rather than through bindBuffers' prefix.
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
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: generate build\n");
        ok = false;
    }
    if (ok && !prepareIndirect.build(m_device, "diff_wf_prepare_indirect.comp.spv", kCounterOnly,
                                     sizeof(WavefrontLoop::PrepareIndirectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: prepare_indirect "
                              "build\n");
        ok = false;
    }
    if (ok && !intersect.build(m_device, "diff_wf_intersect.comp.spv", kIntersectBindings,
                               sizeof(WavefrontLoop::IntersectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: intersect build\n");
        ok = false;
    }
    if (ok && !scatter.build(m_device, "diff_wf_scatter.comp.spv", kScatterBindings,
                             sizeof(WavefrontLoop::ScatterPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: scatter build\n");
        ok = false;
    }

    if (ok) {
        const VkBuffer stateQueueCounter[3] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                               buffers.counterBuffer()};
        const VkBuffer counterOnly[1] = {buffers.counterBuffer()};
        const VkBuffer intersectBuffers[5] = {buffers.stateBuffer(), buffers.queueBuffer(),
                                              buffers.counterBuffer(), vertexBuffer.buffer,
                                              indexBuffer.buffer};
        const VkBuffer scatterBuffers[8] = {buffers.stateBuffer(),       buffers.queueBuffer(),
                                            buffers.counterBuffer(),     debugDrawsBuffer.buffer,
                                            buffers.envMarginalBuffer(), buffers.envConditionalBuffer(),
                                            envSamplesBuffer.buffer,     neeSamplesBuffer.buffer};
        if (!generate.bindBuffers(m_device, stateQueueCounter) ||
            !prepareIndirect.bindBuffers(m_device, counterOnly) ||
            !intersect.bindBuffers(m_device, intersectBuffers) ||
            !intersect.bindAccelerationStructure(m_device, 5, accel.getTLAS()) ||
            !scatter.bindBuffers(m_device, scatterBuffers) ||
            // scatterTLAS is accel's TLAS unless the caller asked for
            // unoccluded shadow rays -- see the shadow-scene block above.
            !scatter.bindAccelerationStructure(m_device, 8, scatterTLAS) ||
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
            std::fprintf(stderr,
                         "[GpuProbeContext] runWavefrontFusedLoopProbe: descriptor binding\n");
            ok = false;
        }
    }

    if (ok) {
        // Camera at the box CENTRE. That is the survival induction's base
        // case -- an origin in the open interior -- and unlike the staircase
        // it replaced, no direction is privileged: whichever way a primary
        // ray goes it leaves through a face, so the camera basis below is
        // free to be the plain identity one. These fields are pushed from
        // the same constants the static_asserts above check, so the camera
        // the build-time guard reasons about is the camera the probe uses.
        GeneratePush genPush{};
        genPush.origin[0] = kFusedLoopCameraX;
        genPush.origin[1] = kFusedLoopCameraY;
        genPush.origin[2] = kFusedLoopCameraZ;
        genPush.forward[0] = 0.0f;
        genPush.forward[1] = 0.0f;
        genPush.forward[2] = 1.0f;
        genPush.right[0] = 1.0f;
        genPush.right[1] = 0.0f;
        genPush.right[2] = 0.0f;
        genPush.up[0] = 0.0f;
        genPush.up[1] = 1.0f;
        genPush.up[2] = 0.0f;
        genPush.width = width;
        genPush.height = height;
        genPush.tanHalfFov = kFusedLoopTanHalfFov;
        genPush.capacity = capacity;
        generate.setPushConstants(&genPush, sizeof(genPush));
        // Fixed dispatch, used here as 1-D: groupsY/groupsZ are left at
        // Fixed's default of 1 (Fixed itself now supports setting them to
        // get a genuine 3-D dispatch -- see kFusedLoopGenerateLocalY's
        // comment above -- this call site just doesn't use that), so this is
        // (width/8, 1, 1) groups x local_size (8,8), covering exactly
        // width x 8 pixels. See the height check above.
        generate.setGroupCount(WavefrontStage::Fixed{width / kFusedLoopGenerateLocalX});

        WavefrontLoop loop;
        // Named rather than positional: Config grew material fields BETWEEN
        // albedo and iterationSeed (Stage 0b-2b Task 2), and a positional
        // aggregate here would have silently re-bound iterationSeed to
        // roughness had the compiler not caught the narrowing. The material
        // is left at Config's defaults -- the pure Lambertian configuration
        // for which the per-bounce estimator weight is exactly `albedo`, so
        // checks 16-18 keep asserting what they always asserted.
        WavefrontLoop::Config loopConfig;
        loopConfig.albedo = albedo;
        loopConfig.iterationSeed = iterationSeed;
        // The film is caller-owned, so its size is Config's to state and not
        // `buffers`' -- see WavefrontLoop::Config::filmPixelCount.
        loopConfig.filmPixelCount = filmPixelCount;
        loop.setConfig(loopConfig);
        loop.setGenerate(generate);
        loop.setPrepareIndirect(prepareIndirect);
        loop.setIntersect(intersect);
        loop.setScatter(scatter);

        outDrawsPerBounce.resize(maxBounces);
        outLiveCountPerRun.resize(maxBounces);
        if (outNeeSamplesPerRun != nullptr) outNeeSamplesPerRun->resize(maxBounces);
        if (outFilmPerRun != nullptr) outFilmPerRun->resize(maxBounces);

        for (uint32_t bounces = 1; ok && bounces <= maxBounces; ++bounces) {
            const WavefrontLoop::Ring finalRing = WavefrontLoop::finalLiveRing(capacity, bounces);

            runImmediate([&](VkCommandBuffer cmd) {
                // Fresh state for every run: zero() ends with its own
                // TRANSFER_WRITE -> SHADER_READ|SHADER_WRITE barrier, so
                // generate's first read of the counter is ordered against it.
                buffers.zero(cmd);

                // The film is NOT part of WavefrontBuffers, so zero() does
                // not touch it and record() will not either -- record()
                // zeroes nothing it does not own. wf_scatter.comp atomicAdds
                // into this buffer, i.e. it READS the previous value, so an
                // un-zeroed film accumulates onto whatever the allocator
                // handed back (and, across the runs of this loop, onto the
                // previous run's total). This fill plus its
                // TRANSFER_WRITE -> SHADER_READ|SHADER_WRITE barrier is the
                // caller-side obligation wf_scatter.comp's film note names.
                // dstAccessMask includes SHADER_WRITE for the usual reason:
                // the first thing the shader does to these bytes is an
                // atomicAdd, which is a read-modify-write.
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

                // THE fused loop -- one command buffer, no vkQueueWaitIdle
                // anywhere inside it. Everything that orders the stages
                // against each other is a barrier recorded by
                // WavefrontLoop::record.
                //
                // debugDrawsBuffer is passed as an extra barrier buffer:
                // wf_scatter.comp writes its (u1, u2, drawCount) triple at a
                // FIXED pathIndex*3 offset on every bounce, so for bounces >= 2
                // successive scatter dispatches in this ONE command buffer
                // overwrite the same bytes. It is caller-owned -- it is not
                // part of WavefrontBuffers -- so the loop's own barriers name
                // only state/queue/counter and nothing would otherwise make
                // bounce k's write available before bounce k+1's. Check 18
                // depends on the LAST bounce's write being the survivor. See
                // wavefront_loop.hpp's "Caller-owned buffers the loop must
                // also order".
                //
                // envSamplesBuffer is here for the identical reason: every
                // scatter dispatch writes the same pathIndex*4 slots.
                // buffers.envMarginalBuffer()/envConditionalBuffer() are NOT
                // -- no dispatch writes them, and extraBarrierBuffers orders
                // buffers the dispatches write.
                // neeSamplesBuffer joins them for the third time over: same
                // fixed per-path offset, same overwrite between bounces of
                // one command buffer, same absence of any barrier that would
                // otherwise order it.
                //
                // filmBuffer is the fourth, and the one where forgetting
                // would cost real radiance rather than a stale diagnostic:
                // wf_scatter.comp atomicAdds into film[pixelIndex*3..+2]
                // every bounce, so bounce k+1 both READS and WRITES the
                // bytes bounce k wrote. Barrier (7) inside
                // WavefrontLoop::recordCompactingStage is what makes that
                // read see that write, and it only covers the buffers named
                // in this span. See wf_scatter.comp's note at the atomicAdd
                // and wavefront_loop.hpp's "Caller-owned buffers the loop
                // must also order".
                const VkBuffer loopExtras[4] = {debugDrawsBuffer.buffer, envSamplesBuffer.buffer,
                                                neeSamplesBuffer.buffer, filmBuffer.buffer};
                loop.record(cmd, buffers, bounces, loopExtras);

                // Everything below is this probe's own readback plumbing,
                // deliberately NOT part of WavefrontLoop::record (only the
                // caller knows what consumes the loop's output).
                //
                // EVERY buffer this function reads back through a mapped
                // pointer has to be named here. envSamplesBuffer was missing
                // from this list before Stage 0b-2b Task 4 even though the
                // readback below invalidates and maps it: vmaInvalidate-
                // Allocation only handles the CPU cache side, it is not a
                // substitute for the GPU-side availability operation, and
                // vkQueueWaitIdle alone does not make writes visible in the
                // host domain per the Vulkan spec. Nothing detected it --
                // synchronization validation has been MEASURED silent on
                // this subsystem's hazards (wavefront_loop.hpp's class
                // comment records disabling every compute-side barrier with
                // zero SYNC- diagnostics) -- so a green run was never
                // evidence either way. The sibling runWavefrontScatterProbe
                // has always listed all of its readback targets; this one
                // now does too, neeSamplesBuffer and (Stage 0b-2b Task 5)
                // filmBuffer included -- the film is read back through a
                // mapped pointer below exactly like the other three sinks,
                // so it needs the same SHADER_WRITE -> HOST_READ availability
                // operation and nothing else provides one.
                VkBufferMemoryBarrier toHost[6]{};
                const VkBuffer hostRead[6] = {buffers.stateBuffer(),   buffers.counterBuffer(),
                                              debugDrawsBuffer.buffer, envSamplesBuffer.buffer,
                                              neeSamplesBuffer.buffer, filmBuffer.buffer};
                for (int i = 0; i < 6; ++i) {
                    toHost[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    toHost[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    toHost[i].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    toHost[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    toHost[i].buffer = hostRead[i];
                    toHost[i].offset = 0;
                    toHost[i].size = VK_WHOLE_SIZE;
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

                const VkBufferMemoryBarrier post[7] = {toHost[0], toHost[1], toHost[2],
                                                       toHost[3], toHost[4], toHost[5],
                                                       queueToTransfer};
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                     0, nullptr, 7, post, 0, nullptr);

                VkBufferCopy region{};
                region.srcOffset = static_cast<VkDeviceSize>(finalRing.queueBase) * sizeof(uint32_t);
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
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &postCopy, 0,
                                     nullptr);
            });

            m_allocator.invalidateBuffer(debugDrawsBuffer);
            const auto* mappedDebug = static_cast<const float*>(debugDrawsBuffer.getMappedData());
            if (mappedDebug == nullptr) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: debug draws "
                                      "buffer not mapped, cannot read back\n");
                ok = false;
                break;
            }
            outDrawsPerBounce[bounces - 1].assign(
                mappedDebug, mappedDebug + (static_cast<std::size_t>(capacity) * kDebugDrawFloats));
            outLiveCountPerRun[bounces - 1] =
                buffers.readbackCounter(m_allocator, finalRing.countSlot);

            m_allocator.invalidateBuffer(queueReadback);
            const auto* mappedQueue = static_cast<const uint32_t*>(queueReadback.getMappedData());
            if (mappedQueue == nullptr) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: queue readback "
                                      "buffer not mapped, cannot read back\n");
                ok = false;
                break;
            }
            outFinalQueue.assign(mappedQueue, mappedQueue + capacity);

            // --- Per-run sinks (Stage 0b-2b Task 5). Read INSIDE the loop,
            // unlike outEnvSamples/outNeeSamples below, because their whole
            // value is that each run exposes a DIFFERENT bounce: run b+1
            // leaves bounce b's NEE record in binding 7 (the same argument
            // outDrawsPerBounce rests on), and leaves the film holding the
            // total over exactly b+1 bounces because it is re-zeroed at the
            // top of every run's command buffer.
            if (outNeeSamplesPerRun != nullptr) {
                m_allocator.invalidateBuffer(neeSamplesBuffer);
                const auto* mappedNeeRun =
                    static_cast<const float*>(neeSamplesBuffer.getMappedData());
                if (mappedNeeRun == nullptr) {
                    std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: NEE "
                                          "samples buffer not mapped, cannot read back\n");
                    ok = false;
                    break;
                }
                (*outNeeSamplesPerRun)[bounces - 1].assign(
                    mappedNeeRun,
                    mappedNeeRun + (static_cast<std::size_t>(capacity) * kNeeSampleFloats));
            }
            if (outFilmPerRun != nullptr) {
                m_allocator.invalidateBuffer(filmBuffer);
                const auto* mappedFilm = static_cast<const float*>(filmBuffer.getMappedData());
                if (mappedFilm == nullptr) {
                    std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: film "
                                          "buffer not mapped, cannot read back\n");
                    ok = false;
                    break;
                }
                (*outFilmPerRun)[bounces - 1].assign(
                    mappedFilm, mappedFilm + (static_cast<std::size_t>(filmPixelCount) * 3u));
            }
        }

        // outEnvSamples, when requested: binding 6 after the FINAL run,
        // capacity*4 floats (dirX, dirY, dirZ, pdf per path index), written
        // by ScatterPush fields THIS function never touches -- they are
        // filled by ohao::diff::WavefrontLoop::record from `buffers`, not by
        // this probe. See this function's doc comment.
        if (ok && outEnvSamples != nullptr) {
            m_allocator.invalidateBuffer(envSamplesBuffer);
            const auto* mappedEnv = static_cast<const float*>(envSamplesBuffer.getMappedData());
            if (mappedEnv == nullptr) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: env samples "
                                      "buffer not mapped, cannot read back\n");
                ok = false;
            } else {
                outEnvSamples->assign(mappedEnv,
                                      mappedEnv + (static_cast<std::size_t>(capacity) * kEnvSampleFloats));
            }
        }

        // outNeeSamples, when requested: binding 7 after the FINAL run. The
        // scene is the closed box, so every shadow ray traced into it is
        // occluded and every contribution here must be exactly zero.
        if (ok && outNeeSamples != nullptr) {
            m_allocator.invalidateBuffer(neeSamplesBuffer);
            const auto* mappedNee = static_cast<const float*>(neeSamplesBuffer.getMappedData());
            if (mappedNee == nullptr) {
                std::fprintf(stderr, "[GpuProbeContext] runWavefrontFusedLoopProbe: NEE samples "
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
    scatter.destroy(m_device);
    intersect.destroy(m_device);
    prepareIndirect.destroy(m_device);
    generate.destroy(m_device);
    if (filmBuffer.isValid()) m_allocator.destroyBuffer(filmBuffer);
    if (shadowIndexBuffer.isValid()) m_allocator.destroyBuffer(shadowIndexBuffer);
    if (shadowVertexBuffer.isValid()) m_allocator.destroyBuffer(shadowVertexBuffer);
    if (neeSamplesBuffer.isValid()) m_allocator.destroyBuffer(neeSamplesBuffer);
    if (envSamplesBuffer.isValid()) m_allocator.destroyBuffer(envSamplesBuffer);
    if (debugDrawsBuffer.isValid()) m_allocator.destroyBuffer(debugDrawsBuffer);
    if (queueReadback.isValid()) m_allocator.destroyBuffer(queueReadback);
    if (vertexBuffer.isValid()) m_allocator.destroyBuffer(vertexBuffer);
    if (indexBuffer.isValid()) m_allocator.destroyBuffer(indexBuffer);

    return ok;
}

}  // namespace ohao::diff
