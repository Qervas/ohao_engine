// The gradient probe.
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
// Stage 1 Task 2 -- the GRADIENT probe.
// ===========================================================================
//
// One evaluation point of (film, dJ/d(albedo)) on a caller-supplied scene,
// produced by two fused runs of ohao::diff::WavefrontLoop at one seed: the
// FORWARD instantiation for the film and the REPLAY one for the arena. See
// the doc comment in gpu_probe_context.hpp for why the two halves are kept on
// separate buffers and why this refuses to run outside the pure Lambertian
// configuration.
//
// WHY THIS IS NOT runWavefrontParityProbe WITH MORE PARAMETERS. That probe
// runs ONE stage (the forward one) many times, once per seed, because its
// subject is a Monte Carlo mean over seeds. This one runs TWO stages once,
// because its subject is a derivative at a single common-random-number
// realisation -- a second seed would be a second measurement, not a better
// one. Generalising the parity probe would have put its calibrated
// non-vacuity gates one parameter default away from a different question.
bool GpuProbeContext::runWavefrontGradientProbe(
    WavefrontBuffers& buffers, uint32_t width, uint32_t height, uint32_t bounces,
    const WavefrontGenerateCamera& camera, std::span<const float> positions,
    std::span<const uint32_t> indices, float albedo, const WavefrontScatterMaterial& material,
    uint32_t iterationSeed, GradientArena& arena, uint32_t gradArenaFloats,
    uint32_t gradAlbedoOffset, std::vector<float>& outFilm,
    const WavefrontGradientOptions& options) {
    // Byte-identical to runWavefrontGenerateProbe's (80 bytes).
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

    outFilm.clear();

    const uint32_t capacity = buffers.layout().capacity();
    bool ok = capacity > 0 && buffers.stateBuffer() != VK_NULL_HANDLE &&
              buffers.queueBuffer() != VK_NULL_HANDLE && buffers.counterBuffer() != VK_NULL_HANDLE;
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: buffers not built\n");
        return false;
    }
    if (height != kFusedLoopGenerateLocalY || width == 0u ||
        (width % kFusedLoopGenerateLocalX) != 0u || width * height != capacity || bounces == 0u) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontGradientProbe: requires height == %u, width a "
                     "non-zero multiple of %u, width*height == capacity (%u) -- which is also the "
                     "ONE-SAMPLE-PER-PIXEL condition the film-hazard resolution rests on -- and "
                     "bounces > 0; got %ux%u, bounces %u\n",
                     kFusedLoopGenerateLocalY, kFusedLoopGenerateLocalX, capacity, width, height,
                     bounces);
        return false;
    }
    if (positions.size() < 9u || positions.size() % 3u != 0u || indices.size() < 3u ||
        indices.size() % 3u != 0u) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontGradientProbe: needs at least one triangle "
                     "(3 floats per vertex, 3 indices per triangle); got %zu floats, %zu indices\n",
                     positions.size(), indices.size());
        return false;
    }
    // THE PARAMETER-SET REFUSAL, and it comes FIRST because every refusal
    // below it dispatches on `diffParam` BY NAME. So does
    // wf_scatter_replay.comp's `diffVertexHook`, and so does traverse.glsl's
    // forward-tangent gate. None of the three has a catch-all -- that is
    // deliberate, and bsdf_adjoint.glsl's allow-list note is where the
    // argument for it lives -- which means a value outside the DIFF_PARAM_*
    // set is not a variant of some other parameter's run. It matches no branch
    // anywhere.
    //
    // The shader scatters a quiet NaN in that case and check 42's finiteness
    // precondition catches it, so the hole is not silent. But that catch is
    // downstream of a whole render, it only fires for a run whose contribution
    // reaches the arena at all (`mayScatter` false writes nothing), and what
    // it reports is "the gradient is not finite" rather than the name of the
    // cause. Refusing here says the cause, before any Vulkan work happens.
    constexpr std::uint32_t kMaxKnownDiffParam = 4u;  // DIFF_PARAM_EMISSION_TEXTURE
    if (options.diffParam > kMaxKnownDiffParam) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontGradientProbe: refuses to run with diffParam "
                     "%u. shaders/includes/diff/bsdf_adjoint.glsl defines exactly five, and they "
                     "are contiguous: 0 DIFF_PARAM_BASECOLOR, 1 DIFF_PARAM_ROUGHNESS, 2 "
                     "DIFF_PARAM_METALLIC, 3 DIFF_PARAM_EMISSION, 4 DIFF_PARAM_EMISSION_TEXTURE. "
                     "A sixth parameter needs a branch in wf_scatter_replay.comp's diffVertexHook "
                     "AND its own preconditions here -- raising this bound alone would buy it "
                     "nothing but a NaN gradient from the shader's fallthrough sentinel\n",
                     options.diffParam);
        return false;
    }
    // THE MATERIAL REFUSAL. See the doc comment: bsdf_adjoint.glsl's
    // derivative is exact only at metallic == 0 (where F0 and the lobe
    // probability q stop depending on the base colour) and its throughput
    // term is exact only at specularWeight == 0 (where bsdf.glsl's fast path
    // makes the per-bounce weight EXACTLY `albedo`). Outside that, a green
    // gradient check would be measuring a derivative of something other than
    // what it thinks -- and at metallic > 0 the finite difference would not
    // even be comparing two runs of the same path.
    if (options.diffParam == 0u &&
        (material.metallic != 0.0f || material.specularWeight != 0.0f)) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontGradientProbe: refuses to run with metallic "
                     "%.9g / specularWeight %.9g. shaders/includes/diff/bsdf_adjoint.glsl is the "
                     "PURE LAMBERTIAN derivative only: at metallic > 0 the base colour enters both "
                     "F0 and the lobe-selection probability, so a +/-h perturbation of the albedo "
                     "MOVES THE SAMPLED DIRECTION and the common-random-number comparison is "
                     "between two different paths; at specularWeight > 0 the per-bounce weight "
                     "stops being exactly `albedo` and the throughput term's closed form fails. "
                     "Task 3 replaces those bodies FOR ROUGHNESS AND METALLIC (diffParam 1 and 2, "
                     "which carry their own preconditions below); for the base colour this is "
                     "still a refusal, not a tolerance\n",
                     static_cast<double>(material.metallic),
                     static_cast<double>(material.specularWeight));
        return false;
    }
    // --- THE TASK 3 PRECONDITIONS. Not a relaxation of the one above: a
    // different parameter needs different things to be true, and each of these
    // is a condition without which the measurement would be vacuous or wrong
    // rather than merely awkward.
    if (options.diffParam == 1u || options.diffParam == 2u) {
        if (material.metallic <= 0.0f && material.specularWeight <= 0.0f) {
            std::fprintf(stderr,
                         "[GpuProbeContext] runWavefrontGradientProbe: refuses to differentiate "
                         "roughness or metallic at metallic %.9g / specularWeight %.9g. The "
                         "lobe-selection probability q = clamp(mix(specScale * maxF * (1-0.9r), "
                         "1, metallic), 0, 1) is then identically 0, diffBsdfEval returns before "
                         "it evaluates D, G or F at all, and BOTH gradients are exactly zero -- "
                         "which a finite difference would confirm, vacuously\n",
                         static_cast<double>(material.metallic),
                         static_cast<double>(material.specularWeight));
            return false;
        }
        if (!(material.roughness > 0.01f)) {
            std::fprintf(stderr,
                         "[GpuProbeContext] runWavefrontGradientProbe: refuses to differentiate "
                         "at roughness %.9g. pbr_unpack.glsl floors roughness at 0.01, so at or "
                         "below the floor d(unpacked)/d(pushed) is 0 on one side and 1 on the "
                         "other: the analytic derivative reports 0 and a central difference "
                         "reports half the unfloored slope. A run must sit strictly above it, "
                         "with room for +/-h\n",
                         static_cast<double>(material.roughness));
            return false;
        }
    }
    if (options.diffParam == 2u && !(material.metallic > 0.0f && material.metallic < 1.0f)) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontGradientProbe: refuses to differentiate "
                     "metallic at metallic %.9g. unpackHitPbr clamps it to [0,1], so at either "
                     "endpoint the derivative is ONE-SIDED -- the adjoint reports 0 there and a "
                     "central difference reports half the interior slope. A metallic gradient run "
                     "must sit strictly inside, with room for +/-h\n",
                     static_cast<double>(material.metallic));
        return false;
    }
    if (arena.buffer() == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: gradient arena not "
                              "built\n");
        return false;
    }

    // --- Scene. ONE triangle soup, bound to the primary trace (acceleration
    // structure plus wf_intersect.comp's vertex/index storage buffers) and to
    // the traversal's shadow rays. Not two.
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
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: failed to create scene "
                              "vertex/index buffers\n");
    }

    RTAccelerationStructure accel;
    if (ok && !accel.init(m_device, m_physicalDevice, m_queue, m_queueFamily, m_commandPool,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: "
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
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: "
                                  "createBLASFromPositions failed\n");
            ok = false;
        }
    }
    if (ok) {
        accel.clearInstances();
        accel.addInstance(blas, glm::mat4(1.0f));
        runImmediate([&](VkCommandBuffer cmd) { accel.buildTLAS(cmd); });
        if (accel.getTLAS() == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: buildTLAS produced "
                                  "no TLAS\n");
            ok = false;
        }
    }

    // --- Sinks. Two INDEPENDENT sets, one per instantiation, for
    // runWavefrontReplayProbe's reason: nothing the replay run writes may
    // reach a byte the forward run's film is read out of. debugDraws (3),
    // envSamples (6) and neeSamples (7) are allocated and bound but never
    // read -- a descriptor set must cover every binding the shader statically
    // uses -- and still go through extraBarrierBuffers, because every scatter
    // dispatch overwrites the same per-path offsets in them.
    struct ScatterSinks {
        GpuBuffer trace;
        GpuBuffer env;
        GpuBuffer nee;
        GpuBuffer film;
    };
    ScatterSinks fwdSinks;
    ScatterSinks repSinks;
    ScatterSinks* const sinkSets[2] = {&fwdSinks, &repSinks};
    const uint32_t filmPixelCount = width * height;
    const VkDeviceSize filmBytes = static_cast<VkDeviceSize>(filmPixelCount) * 3u * sizeof(float);
    for (ScatterSinks* s : sinkSets) {
        if (!ok) break;
        // HOST-READABLE, unlike the other two sinks: Stage 1 Task 3's
        // frozen-direction measurement reads the FORWARD run's vertex trace
        // back and compares two renders' ray origins, directions and hit
        // distances bit for bit. That is how the detached instrument's
        // defining claim is MEASURED rather than argued.
        s->trace = m_allocator.createBuffer(
            static_cast<VkDeviceSize>(capacity) * kDebugDrawFloats * sizeof(float),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::GpuToCpu,
            /*persistentlyMapped=*/true);
        s->env = m_allocator.createBuffer(
            static_cast<VkDeviceSize>(capacity) * kEnvSampleFloats * sizeof(float),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::GpuOnly);
        s->nee = m_allocator.createBuffer(
            static_cast<VkDeviceSize>(capacity) * kNeeSampleFloats * sizeof(float),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::GpuOnly);
        s->film = m_allocator.createBuffer(
            filmBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            AllocationUsage::GpuToCpu, /*persistentlyMapped=*/true);
        if (!s->trace.isValid() || !s->env.isValid() || !s->nee.isValid() || !s->film.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: scatter sink "
                                  "allocation failed\n");
            ok = false;
        }
    }

    // --- THE EMISSION TEXTURE'S PRIMAL (Stage 1 Task 5), binding 11.
    //
    // Uploaded FRESH on every call, from `options.emissionTexture`, because
    // that is exactly what a per-texel finite difference needs: the caller
    // perturbs one element of its own vector and calls again, and the two
    // renders differ in that one float and in nothing else. Read-only to
    // both instantiations, so it is NOT passed to record()'s
    // extraBarrierBuffers -- that parameter is for buffers the dispatches
    // WRITE.
    //
    // With no texture requested (the default) a ONE-FLOAT placeholder is
    // allocated rather than the film being re-bound, unlike the other probes
    // in this file: this one has a real arena and a film that check 45's
    // finite difference reads, and a private buffer keeps both out of reach
    // of a shader bug at this binding entirely. `emissionTexWidth` is pushed
    // as 0 in that case, which disables every read of it in the traversal.
    const bool hasEmissionTexture =
        !options.emissionTexture.empty() && options.emissionTexWidth > 0u &&
        options.emissionTexHeight > 0u && options.emissionTexChannels > 0u;
    if (ok && hasEmissionTexture) {
        const std::size_t expected = static_cast<std::size_t>(options.emissionTexWidth) *
                                     options.emissionTexHeight * options.emissionTexChannels;
        if (options.emissionTexture.size() != expected) {
            std::fprintf(stderr,
                         "[GpuProbeContext] runWavefrontGradientProbe: emission texture holds %zu "
                         "floats but its shape (%ux%ux%u) says %zu. The shader's bounds guard "
                         "covers the ARENA side, not this one -- a short array here is a read "
                         "past the end of the allocation\n",
                         options.emissionTexture.size(), options.emissionTexWidth,
                         options.emissionTexHeight, options.emissionTexChannels, expected);
            ok = false;
        }
    }
    // STAGE 2 TASK 1: the adjoint seed. Its LENGTH is checked here rather
    // than trusted, because the shader is told a float count and cannot
    // verify the buffer actually holds that many -- the same gap
    // `filmPixelCount` and `gradArenaFloats` carry, and the same answer.
    const std::size_t kExpectedSeedFloats = static_cast<std::size_t>(width) * height * 3u;
    const bool hasAdjointSeed = !options.adjointSeed.empty();
    if (ok && hasAdjointSeed && options.adjointSeed.size() != kExpectedSeedFloats) {
        std::fprintf(stderr,
                     "[GpuProbeContext] runWavefrontGradientProbe: adjointSeed holds %zu floats, "
                     "but a %ux%u film needs exactly %zu (three per pixel, in the film's own "
                     "pixelIndex*3 + c order). Binding a buffer shorter than the count pushed "
                     "would read past its end; binding a longer one means the caller and the "
                     "shader disagree about which pixel is which\n",
                     options.adjointSeed.size(), width, height, kExpectedSeedFloats);
        ok = false;
    }
    const std::vector<float> kAdjointSeedPlaceholder{0.0f};
    GpuBuffer adjointSeedBuffer;
    if (ok) {
        adjointSeedBuffer = m_allocator.createBufferFromSpan<float>(
            hasAdjointSeed ? std::span<const float>(options.adjointSeed)
                           : std::span<const float>(kAdjointSeedPlaceholder),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!adjointSeedBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: adjoint seed "
                                  "buffer allocation failed\n");
            ok = false;
        }
    }

    const std::vector<float> kEmissionTexPlaceholder{0.0f};
    GpuBuffer emissionTexBuffer;
    if (ok) {
        emissionTexBuffer = m_allocator.createBufferFromSpan<float>(
            hasEmissionTexture ? std::span<const float>(options.emissionTexture)
                               : std::span<const float>(kEmissionTexPlaceholder),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!emissionTexBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: emission texture "
                                  "buffer allocation failed\n");
            ok = false;
        }
    }

    WavefrontStage generate;
    WavefrontStage prepareIndirect;
    WavefrontStage intersect;
    WavefrontStage scatterForward;
    WavefrontStage scatterReplay;
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
    // Both instantiations declare the same eleven bindings, because both
    // include the same traverse.glsl. Binding 10 is the gradient arena and is
    // bound to the REAL arena for both -- unlike every other probe here,
    // which has none and re-binds its film there.
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
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: generate build\n");
        ok = false;
    }
    if (ok && !prepareIndirect.build(m_device, "diff_wf_prepare_indirect.comp.spv", kCounterOnly,
                                     sizeof(WavefrontLoop::PrepareIndirectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: prepare_indirect "
                              "build\n");
        ok = false;
    }
    if (ok && !intersect.build(m_device, "diff_wf_intersect.comp.spv", kIntersectBindings,
                               sizeof(WavefrontLoop::IntersectPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: intersect build\n");
        ok = false;
    }
    if (ok && !scatterForward.build(m_device, "diff_wf_scatter.comp.spv", kScatterBindings,
                                    sizeof(WavefrontLoop::ScatterPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: forward scatter "
                              "build\n");
        ok = false;
    }
    if (ok && !scatterReplay.build(m_device, "diff_wf_scatter_replay.comp.spv", kScatterBindings,
                                   sizeof(WavefrontLoop::ScatterPush))) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: REPLAY scatter build "
                              "failed (diff_wf_scatter_replay.comp.spv)\n");
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
            ok = scatterStages[i]->bindBuffers(m_device, scatterBuffers) &&
                 scatterStages[i]->bindAccelerationStructure(m_device, 8, accel.getTLAS()) &&
                 scatterStages[i]->bindStorageBuffer(m_device, 9, s.film.buffer) &&
                 // The REAL gradient arena, bound to BOTH instantiations. The
                 // forward one never writes it (its hook is the film write)
                 // and is pushed gradArenaFloats = 0 below besides.
                 scatterStages[i]->bindStorageBuffer(m_device, 10, arena.buffer()) &&
                 // BINDING 11, the emission-texture primal (Stage 1 Task 5).
                 // The SAME buffer for both instantiations, deliberately: it
                 // is read-only, and the forward read and the replay scatter
                 // must be looking at ONE array for the gradient to be the
                 // derivative of the film that was actually rendered.
                 scatterStages[i]->bindStorageBuffer(m_device, 11, emissionTexBuffer.buffer) &&
                 scatterStages[i]->bindStorageBuffer(m_device, 12, adjointSeedBuffer.buffer);
        }
        if (!ok) {
            std::fprintf(stderr,
                         "[GpuProbeContext] runWavefrontGradientProbe: descriptor binding\n");
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

        for (int variant = 0; ok && variant < 2; ++variant) {
            const bool isReplay = (variant == 1);
            ScatterSinks& s = *sinkSets[variant];
            loop.setScatter(*scatterStages[variant]);

            WavefrontLoop::Config loopConfig;
            loopConfig.albedo = albedo;
            loopConfig.roughness = material.roughness;
            loopConfig.metallic = material.metallic;
            loopConfig.specularWeight = material.specularWeight;
            loopConfig.filmPixelCount = filmPixelCount;
            // ONLY the replay run is given an arena. The forward run's
            // gradArenaFloats stays 0, which disables every gradient write in
            // its traversal -- so "the forward pass wrote no gradient" is
            // enforced by a push constant as well as by its hook being the
            // film write.
            loopConfig.gradArenaFloats = isReplay ? gradArenaFloats : 0u;
            loopConfig.gradAlbedoOffset = isReplay ? gradAlbedoOffset : 0u;
            // Both runs are pushed the SAME diffParam and the SAME sampling
            // material. They must be: the two instantiations walk one path
            // only while every push-constant field that steers the traversal
            // agrees, and both of these steer it -- diffParam gates the
            // tangent update in path state, and the sampling material gates
            // every direction.
            loopConfig.diffParam = options.diffParam;
            // STAGE 2 TASK 1. Pushed to the REPLAY run only -- unlike the
            // emission, which is a property of the scene, dL/dpixel is a
            // property of the OBJECTIVE and the forward hook has no use for
            // it: its job is to write the film, and the film does not depend
            // on what will later be differentiated. Pushing it to both would
            // be harmless today (the forward hook never calls
            // diffAdjointSeed) and would be a standing invitation to make the
            // film depend on the loss, which spec 4.6 forbids in that exact
            // direction.
            loopConfig.adjointSeedFloats =
                (isReplay && hasAdjointSeed)
                    ? static_cast<std::uint32_t>(options.adjointSeed.size())
                    : 0u;
            // Stage 1 Task 4. Pushed to BOTH runs, unconditionally, like
            // `albedo` above and unlike the arena offset: the emission is a
            // property of the SCENE this loop renders (what the FORWARD
            // hook adds to the film), not a property of which run this is,
            // so both the forward film and the replay run's material context
            // must agree on it.
            loopConfig.emission = options.emission;
            // Stage 1 Task 5, pushed to BOTH runs for `emission`'s reason:
            // the emitted radiance is a property of the SCENE this loop
            // renders. A zero width/height (the default) leaves the
            // traversal reading the scalar above, exactly as before.
            if (hasEmissionTexture) {
                loopConfig.emissionTexWidth = options.emissionTexWidth;
                loopConfig.emissionTexHeight = options.emissionTexHeight;
                loopConfig.emissionTexChannels = options.emissionTexChannels;
                loopConfig.emissionUvScaleU = options.emissionUvScaleU;
                loopConfig.emissionUvScaleV = options.emissionUvScaleV;
                loopConfig.emissionUvBiasU = options.emissionUvBiasU;
                loopConfig.emissionUvBiasV = options.emissionUvBiasV;
            }
            if (options.freezeSampling) {
                loopConfig.samplingAlbedo = options.samplingAlbedo;
                loopConfig.samplingRoughness = options.samplingMaterial.roughness;
                loopConfig.samplingMetallic = options.samplingMaterial.metallic;
                loopConfig.samplingSpecularWeight = options.samplingMaterial.specularWeight;
            }
            loopConfig.iterationSeed = iterationSeed;
            loop.setConfig(loopConfig);

            runImmediate([&](VkCommandBuffer cmd) {
                buffers.zero(cmd);

                // The film is caller-owned and read-modify-written, so it is
                // zeroed here with its own TRANSFER_WRITE ->
                // SHADER_READ|SHADER_WRITE barrier.
                vkCmdFillBuffer(cmd, s.film.buffer, 0, VK_WHOLE_SIZE, 0u);
                VkBufferMemoryBarrier filmZero{};
                filmZero.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                filmZero.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                filmZero.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                filmZero.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                filmZero.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                filmZero.buffer = s.film.buffer;
                filmZero.offset = 0;
                filmZero.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                                     &filmZero, 0, nullptr);

                // The arena is zeroed on BOTH runs, in the same command
                // buffer as the loop that follows -- GradientArena::zero
                // records the fill AND its TRANSFER_WRITE ->
                // SHADER_READ|SHADER_WRITE barrier, which is exactly the
                // configuration its own comment says that barrier becomes
                // load-bearing in.
                arena.zero(cmd);

                const VkBuffer loopExtras[5] = {s.trace.buffer, s.env.buffer, s.nee.buffer,
                                                s.film.buffer, arena.buffer()};
                loop.record(cmd, buffers, bounces, loopExtras);

                // Host-read availability for exactly what this function reads
                // back: the film (mapped) and, on the replay run, the arena
                // (mapped, through GradientArena::readback). vkQueueWaitIdle
                // does not make writes visible in the host domain and
                // vmaInvalidateAllocation covers only the CPU cache side.
                VkBufferMemoryBarrier toHost[3]{};
                VkBuffer hostRead[3] = {s.film.buffer, arena.buffer(), VK_NULL_HANDLE};
                uint32_t hostReadCount = isReplay ? 2u : 1u;
                // Any host readback must be NAMED in this barrier, not merely
                // waited for: vkQueueWaitIdle does not make writes visible in
                // the host domain, and a VkBufferMemoryBarrier's memory scope
                // covers only the buffers it lists.
                if (!isReplay && options.outForwardTrace != nullptr) {
                    hostRead[hostReadCount] = s.trace.buffer;
                    ++hostReadCount;
                }
                for (uint32_t i = 0; i < hostReadCount; ++i) {
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
                                     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, hostReadCount,
                                     toHost, 0, nullptr);
            });

            if (!isReplay) {
                m_allocator.invalidateBuffer(s.film);
                const auto* mappedFilm = static_cast<const float*>(s.film.getMappedData());
                if (mappedFilm == nullptr) {
                    std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: film buffer "
                                          "not mapped, cannot read back\n");
                    ok = false;
                    break;
                }
                outFilm.assign(mappedFilm,
                               mappedFilm + (static_cast<std::size_t>(filmPixelCount) * 3u));
                // The LAST bounce's vertex trace, if the caller asked for it.
                // It is what makes the frozen-direction claim MEASURABLE
                // rather than argued: two renders that walked the same path
                // wrote bit-identical origins, directions and hit distances
                // into these slots.
                if (options.outForwardTrace != nullptr) {
                    m_allocator.invalidateBuffer(s.trace);
                    const auto* mappedTrace = static_cast<const float*>(s.trace.getMappedData());
                    if (mappedTrace == nullptr) {
                        std::fprintf(stderr,
                                     "[GpuProbeContext] runWavefrontGradientProbe: vertex trace "
                                     "buffer not mapped, cannot read back\n");
                        ok = false;
                        break;
                    }
                    options.outForwardTrace->assign(
                        mappedTrace,
                        mappedTrace + (static_cast<std::size_t>(capacity) * kDebugDrawFloats));
                }
            }
        }
    }

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
    if (emissionTexBuffer.isValid()) m_allocator.destroyBuffer(emissionTexBuffer);
    if (adjointSeedBuffer.isValid()) m_allocator.destroyBuffer(adjointSeedBuffer);
    if (vertexBuffer.isValid()) m_allocator.destroyBuffer(vertexBuffer);
    if (indexBuffer.isValid()) m_allocator.destroyBuffer(indexBuffer);

    return ok;
}

}  // namespace ohao::diff
