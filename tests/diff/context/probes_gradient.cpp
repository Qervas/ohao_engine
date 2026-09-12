// The gradient probe.
//
// Lifted verbatim out of gpu_probe_context.cpp: same member, same
// signature, same body. A linkage change, not a value change.
#include "gpu_probe_context.hpp"

#include "context/probe_scene.hpp"

#include "diff/wavefront/compute_pipeline.hpp"
#include "diff/wavefront/gradient_frame.hpp"
#include "diff/wavefront/gradient_render.hpp"
#include "diff/wavefront/scatter_sinks.hpp"
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
void GpuProbeContext::destroyOwnedStages(GradientStages& stages) {
    stages.destroy(m_device);
}

bool GpuProbeContext::buildOwnedScene(std::span<const float> positions,
                                      std::span<const std::uint32_t> indices, OwnedScene& out) {
    destroyOwnedScene(out);
    out.vertexBuffer = m_allocator.createBufferFromSpan<float>(
        positions, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    out.indexBuffer = m_allocator.createBufferFromSpan<uint32_t>(
        indices, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!out.vertexBuffer.isValid() || !out.indexBuffer.isValid()) {
        std::fprintf(stderr, "[GpuProbeContext] buildOwnedScene: vertex/index buffer alloc\n");
        destroyOwnedScene(out);
        return false;
    }

    auto accel = std::make_shared<RTAccelerationStructure>();
    if (!accel->init(m_device, m_physicalDevice, m_queue, m_queueFamily, m_commandPool,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) {
        std::fprintf(stderr, "[GpuProbeContext] buildOwnedScene: accel init\n");
        destroyOwnedScene(out);
        return false;
    }
    BlasHandle blas = INVALID_BLAS;
    runImmediate([&](VkCommandBuffer cmd) {
        blas = accel->createBLASFromPositions(
            out.vertexBuffer.buffer, static_cast<uint32_t>(positions.size() / 3),
            out.indexBuffer.buffer, static_cast<uint32_t>(indices.size()),
            /*indexByteOffset=*/0, cmd);
    });
    if (blas == INVALID_BLAS) {
        std::fprintf(stderr, "[GpuProbeContext] buildOwnedScene: createBLASFromPositions\n");
        destroyOwnedScene(out);
        return false;
    }
    accel->clearInstances();
    accel->addInstance(blas, glm::mat4(1.0f));
    runImmediate([&](VkCommandBuffer cmd) { accel->buildTLAS(cmd); });
    if (accel->getTLAS() == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[GpuProbeContext] buildOwnedScene: buildTLAS produced no TLAS\n");
        destroyOwnedScene(out);
        return false;
    }
    out.tlas = accel->getTLAS();
    out.accel = std::move(accel);
    return true;
}

void GpuProbeContext::destroyOwnedScene(OwnedScene& scene) {
    // The RTAccelerationStructure releases its own device objects when the
    // last reference goes; the two buffers are ours.
    scene.accel.reset();
    scene.tlas = VK_NULL_HANDLE;
    if (scene.vertexBuffer.isValid()) m_allocator.destroyBuffer(scene.vertexBuffer);
    if (scene.indexBuffer.isValid()) m_allocator.destroyBuffer(scene.indexBuffer);
}

bool GpuProbeContext::runWavefrontGradientProbe(
    WavefrontBuffers& buffers, uint32_t width, uint32_t height, uint32_t bounces,
    const WavefrontGenerateCamera& camera, std::span<const float> positions,
    std::span<const uint32_t> indices, float albedo, const WavefrontScatterMaterial& material,
    uint32_t iterationSeed, GradientArena& arena, uint32_t gradArenaFloats,
    uint32_t gradAlbedoOffset, std::vector<float>& outFilm,
    const WavefrontGradientOptions& options) {
    // The camera push block and both loop configurations are assembled by
    // ohao/diff/wavefront/gradient_frame.{hpp,cpp} now. What used to be
    // seventy lines of field-by-field copying here -- with the reasoning for
    // which field goes to which run written as comments a test file owned --
    // is a GradientFrame and two calls, and diff_unit_tests asserts the split
    // that reasoning describes.
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
    //
    // OR NONE OF THAT, when the caller brought its own (options.scene): an
    // engine has a persistent BLAS and is not going to hand over a span of
    // floats, and a check that renders four hundred times should not build
    // four hundred acceleration structures. The handles below are what the
    // dispatches actually bind, and they come from whichever source applies.
    const bool ownScene = (options.scene == nullptr);
    if (!ownScene && !options.scene->valid()) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: options.scene was "
                              "supplied but is incomplete -- a TLAS, a vertex buffer and an "
                              "index buffer are all required\n");
        return false;
    }
    GpuBuffer vertexBuffer{};
    GpuBuffer indexBuffer{};
    VkAccelerationStructureKHR sceneTlas =
        ownScene ? VK_NULL_HANDLE : options.scene->tlas;
    VkBuffer sceneVertexBuffer =
        ownScene ? VK_NULL_HANDLE : options.scene->vertexBuffer;
    VkBuffer sceneIndexBuffer = ownScene ? VK_NULL_HANDLE : options.scene->indexBuffer;

    RTAccelerationStructure accel;
    if (ownScene) {
    vertexBuffer = m_allocator.createBufferFromSpan<float>(
        positions, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    indexBuffer = m_allocator.createBufferFromSpan<uint32_t>(
        indices, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ok = vertexBuffer.isValid() && indexBuffer.isValid();
    if (!ok) {
        std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: failed to create scene "
                              "vertex/index buffers\n");
    }

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
    if (ok) {
        sceneTlas = accel.getTLAS();
        sceneVertexBuffer = vertexBuffer.buffer;
        sceneIndexBuffer = indexBuffer.buffer;
    }
    }  // ownScene

    // --- Sinks. Two INDEPENDENT sets, one per instantiation, for
    // runWavefrontReplayProbe's reason: nothing the replay run writes may
    // reach a byte the forward run's film is read out of. debugDraws (3),
    // envSamples (6) and neeSamples (7) are allocated and bound but never
    // read -- a descriptor set must cover every binding the shader statically
    // uses -- and still go through extraBarrierBuffers, because every scatter
    // dispatch overwrites the same per-path offsets in them.
    // ITEM 1. The sinks now live in the LIBRARY (ohao/diff/wavefront/
    // scatter_sinks.hpp) rather than as locals here, because a record-only
    // gradient entry point cannot own them: DiffRenderer records into a
    // caller-supplied command buffer and does not submit, so the buffers the
    // caller reads after its OWN submit have to outlive the recording call.
    //
    // The STRIDES stay here, and are passed in. They are tied to the shader's
    // writes by checkNeeStrideTie and checkWfScatterSinkLayoutTie, which parse
    // wf_scatter.comp and refuse to run if the numbers disagree; a copy in the
    // library would be a third home for the same fact and the only one nothing
    // checks.
    ScatterSinks sinks;
    const uint32_t filmPixelCount = width * height;
    if (ok) {
        const ScatterSinks::Strides strides{kDebugDrawFloats, kEnvSampleFloats,
                                            kNeeSampleFloats};
        ok = sinks.create(m_allocator, capacity, width, height, strides);
        if (!ok) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: scatter sink "
                                  "allocation failed\n");
        }
    }
    // Non-const: invalidateBuffer needs a mutable handle for the host reads.
    ScatterSinkSet* const sinkSets[2] = {&sinks.forward(), &sinks.replay()};

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

    // Spec 10.2's map, when the caller wants one. A one-float placeholder
    // otherwise -- binding 13 is statically used by the one traversal source,
    // so both instantiations need a descriptor for it whether or not anything
    // writes it.
    const bool wantsSensitivity = (options.outSensitivity != nullptr);
    const std::uint32_t sensitivityFloats = wantsSensitivity ? filmPixelCount : 0u;
    GpuBuffer sensitivityBuffer;
    if (ok) {
        const std::vector<float> zeros(wantsSensitivity ? filmPixelCount : 1u, 0.0f);
        sensitivityBuffer = m_allocator.createBufferFromSpan<float>(
            std::span<const float>(zeros),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        if (!sensitivityBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: sensitivity map "
                                  "buffer allocation failed\n");
            ok = false;
        }
    }

    // Binding 14's environment radiance, when the caller supplies one.
    const bool hasEnvImage = !options.envImage.empty();
    const std::vector<float> kEnvImagePlaceholder{0.0f};
    GpuBuffer envImageBuffer;
    if (ok) {
        envImageBuffer = m_allocator.createBufferFromSpan<float>(
            hasEnvImage ? std::span<const float>(options.envImage)
                        : std::span<const float>(kEnvImagePlaceholder),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!envImageBuffer.isValid()) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: environment image "
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

    // The pipelines: the caller's if it brought some, otherwise ours for the
    // duration of this call. The references mean everything below is unaware
    // of which, and `st.built` is what makes a caller's set build once.
    GradientStages localStages;
    GradientStages& st = (options.stages != nullptr) ? *options.stages : localStages;
    const bool ownStages = (options.stages == nullptr);

    // The binding tables and the build now live in GradientStages. The push
    // SIZES are still the caller's, because the caller fills the blocks --
    // and all four are library types now that GenerateCameraPush has moved
    // into gradient_frame.hpp.
    if (ok) {
        const GradientStages::PushSizes pushSizes{
            static_cast<std::uint32_t>(sizeof(GenerateCameraPush)),
            static_cast<std::uint32_t>(sizeof(WavefrontLoop::PrepareIndirectPush)),
            static_cast<std::uint32_t>(sizeof(WavefrontLoop::IntersectPush)),
            static_cast<std::uint32_t>(sizeof(WavefrontLoop::ScatterPush))};
        ok = st.build(m_device, pushSizes);
        if (!ok) {
            std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: stage build\n");
        }
    }

    WavefrontStage& generate = st.generate();
    WavefrontStage& prepareIndirect = st.prepareIndirect();
    WavefrontStage& intersect = st.intersect();
    WavefrontStage* const scatterStages[2] = {&st.scatter(0u), &st.scatter(1u)};

    // The binding tables and the five build calls that used to sit here are now
    // GradientStages::build, in the library. They are not test-side knowledge:
    // they are what shaders/diff/ requires of anyone who dispatches it.

    if (ok) {
        // Every descriptor the five stages declare, written by the library.
        // Called every dispatch rather than once: the adjoint-seed and
        // emission-texture buffers are genuinely new each time, so it is the
        // pipelines that are reusable and not the bindings.
        const GradientStages::Scene sceneHandles{sceneTlas, sceneVertexBuffer, sceneIndexBuffer};
        const GradientStages::Attachments attachments{arena.buffer(), emissionTexBuffer.buffer,
                                                      adjointSeedBuffer.buffer,
                                                      sensitivityBuffer.buffer,
                                                      envImageBuffer.buffer};
        ok = st.bindAll(m_device, buffers, sceneHandles, sinks, attachments);
    }

    if (ok) {
        // ONE description of this render, from which both instantiations'
        // configurations are derived. Assembled here rather than taken as an
        // argument because this function's signature predates it; an engine
        // call site builds a GradientFrame directly.
        GradientFrame frame;
        frame.camera = camera;
        frame.width = width;
        frame.height = height;
        frame.bounces = bounces;
        frame.iterationSeed = iterationSeed;
        frame.albedo = albedo;
        frame.material = material;
        frame.diffParam = options.diffParam;
        frame.gradArenaFloats = gradArenaFloats;
        frame.gradParamOffset = gradAlbedoOffset;
        frame.adjointSeedFloats =
            hasAdjointSeed ? static_cast<std::uint32_t>(options.adjointSeed.size()) : 0u;
        frame.filmPixelCount = filmPixelCount;
        frame.emission = options.emission;
        if (hasEmissionTexture) {
            frame.emissionTexWidth = options.emissionTexWidth;
            frame.emissionTexHeight = options.emissionTexHeight;
            frame.emissionTexChannels = options.emissionTexChannels;
            frame.emissionUvScaleU = options.emissionUvScaleU;
            frame.emissionUvScaleV = options.emissionUvScaleV;
            frame.emissionUvBiasU = options.emissionUvBiasU;
            frame.emissionUvBiasV = options.emissionUvBiasV;
        }
        frame.sensitivityFloats = sensitivityFloats;
        frame.envImageTexels =
            hasEnvImage ? static_cast<std::uint32_t>(options.envImage.size()) : 0u;
        frame.freezeSampling = options.freezeSampling;
        frame.samplingAlbedo = options.samplingAlbedo;
        frame.samplingMaterial = options.samplingMaterial;

        // The stage configuration, both loop configurations and the whole
        // recorded body are ohao::diff::recordGradientRun's now. What is
        // left here is what a TEST does and an ENGINE does not: submit each
        // run on its own and read the result back before the next.
        GradientResources resources;
        resources.buffers = &buffers;
        resources.stages = &st;
        resources.sinks = &sinks;
        resources.scene = GradientStages::Scene{sceneTlas, sceneVertexBuffer, sceneIndexBuffer};
        resources.emissionTexture = emissionTexBuffer.buffer;
        resources.adjointSeed = adjointSeedBuffer.buffer;
        resources.sensitivity = sensitivityBuffer.buffer;
        resources.envImage = envImageBuffer.buffer;

        for (int variant = 0; ok && variant < 2; ++variant) {
            const bool isReplay = (variant == 1);
            ScatterSinkSet& s = *sinkSets[variant];
            const GradientRun run = isReplay ? GradientRun::Replay : GradientRun::Forward;

            // ONE SUBMIT PER RUN, which is this function's shape and not the
            // library's: the film has to be read back between them, because
            // several callers derive the replay run's adjoint seed from it on
            // the host. An engine records both into one frame's command
            // buffer and submits once -- which is exactly why the recording
            // moved out and the submitting did not.
            runImmediate([&](VkCommandBuffer cmd) {
                if (!recordGradientRun(cmd, run, frame, resources, arena, !options.accumulate)) {
                    ok = false;
                    return;
                }
                // Host-read availability for exactly what THIS function maps:
                // the film, the arena on the replay run, and the vertex trace
                // when the caller asked for it. Each must be NAMED --
                // vkQueueWaitIdle does not make writes visible in the host
                // domain, and a VkBufferMemoryBarrier's memory scope covers
                // only the buffers it lists.
                VkBuffer hostRead[4] = {s.film.buffer, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                        VK_NULL_HANDLE};
                std::size_t hostReadCount = 1u;
                if (isReplay) hostRead[hostReadCount++] = arena.buffer();
                if (isReplay && wantsSensitivity) {
                    hostRead[hostReadCount++] = sensitivityBuffer.buffer;
                }
                if (!isReplay && options.outForwardTrace != nullptr) {
                    hostRead[hostReadCount++] = s.trace.buffer;
                }
                recordHostReadBarrier(cmd, std::span<const VkBuffer>(hostRead, hostReadCount));
            });
            if (!ok) {
                std::fprintf(stderr,
                             "[GpuProbeContext] runWavefrontGradientProbe: recordGradientRun "
                             "refused the %s run; its own message above says why\n",
                             isReplay ? "replay" : "forward");
                break;
            }

            if (isReplay && wantsSensitivity) {
                // Spec 10.2's map, read off the REPLAY run because that is
                // the only run that writes it.
                m_allocator.invalidateBuffer(sensitivityBuffer);
                const auto* mappedMap =
                    static_cast<const float*>(sensitivityBuffer.getMappedData());
                if (mappedMap == nullptr) {
                    std::fprintf(stderr, "[GpuProbeContext] runWavefrontGradientProbe: "
                                          "sensitivity buffer not mapped, cannot read back\n");
                    ok = false;
                    break;
                }
                options.outSensitivity->assign(mappedMap, mappedMap + filmPixelCount);
            }

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

    // Only ours. A caller's stages outlive this call by definition -- that
    // is what they are for -- and destroying them here would leave the next
    // call recording against pipelines that no longer exist.
    if (ownStages) destroyOwnedStages(localStages);
    // One call now: ScatterSinks owns the release, in the reverse order of
    // creation, and is idempotent.
    sinks.destroy(m_allocator);
    if (emissionTexBuffer.isValid()) m_allocator.destroyBuffer(emissionTexBuffer);
    if (adjointSeedBuffer.isValid()) m_allocator.destroyBuffer(adjointSeedBuffer);
    if (sensitivityBuffer.isValid()) m_allocator.destroyBuffer(sensitivityBuffer);
    if (envImageBuffer.isValid()) m_allocator.destroyBuffer(envImageBuffer);
    if (vertexBuffer.isValid()) m_allocator.destroyBuffer(vertexBuffer);
    if (indexBuffer.isValid()) m_allocator.destroyBuffer(indexBuffer);

    return ok;
}

}  // namespace ohao::diff
