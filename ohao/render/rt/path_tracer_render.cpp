// PathTracer::render: the frame, as a sequence of render::Pass scopes in frame order, and the push constants'
// flags (kPTFlag*).

#include "path_tracer.hpp"
#include "render/shader_bindings/rt_path_tracer.hpp"
#include "gpu/shared_layout.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include "core/log.hpp"
#include "render/scene/render_scene.hpp"
#include <glm/gtc/type_ptr.hpp>

// nrd_denoise.hpp / nrd_compose.hpp are safe to include unconditionally; their
// dispatch methods are OHAO_NRD_ENABLED-guarded inside this TU.
#include "render/rt/denoise/nrd_denoise.hpp"
#include "render/rt/denoise/nrd_compose.hpp"
#ifdef OHAO_DLSS_ENABLED
#include "render/rt/denoise/dlss_rr.hpp"
#endif

namespace ohao {
namespace P = shader::rt_path_tracer;

namespace {
constexpr uint32_t kPTFlagEnableAOVs = 1u << 0;
constexpr uint32_t kPTFlagEnableFireflyClamp = 1u << 2;
// Offline NRD: the five NRD inputs (22-26) are the running mean of the frames accumulated, so
// --spp N feeds NRD an N-sample mean (tracerAccumulatesAOVs). rt/path_tracer.slang's kFlag*.
constexpr uint32_t kPTFlagAccumulateAOVs = 1u << 3;
// Phase 5: DLSSRR mode — raygen repurposes normalAOV (binding 7) as the packed
// (worldN, roughness) guide DLSS Ray Reconstruction consumes.
constexpr uint32_t kPTFlagDLSSRR = 1u << 4;
// The realtime profile: paths end in the radiance cache (rt/radiance_cache.slang).
constexpr uint32_t kPTFlagRadianceCache = 1u << 5;
// Path guiding (sub-project 23): rt/path_tracer.slang's kFlagGuiding.
constexpr uint32_t kPTFlagGuiding = 1u << 6;
// Caustics (sub-project 24): rt/path_tracer.slang's kFlagCaustics (bits 7-9 are guiding's debug).
constexpr uint32_t kPTFlagCaustics = 1u << 10;
// Caustics step 2: kFlagManifold (the camera side drops the manifold pass's terms) and kFlagManifoldRequests (it
// writes the pass's requests); bits 11-18 are OHAO_PT_CLASS.
constexpr uint32_t kPTFlagManifold = 1u << 19;
constexpr uint32_t kPTFlagManifoldRequests = 1u << 20;
constexpr uint32_t kPTFlagManifoldGlass = 1u << 23;   // step 2b: the sun through owned solids
}  // namespace

void PathTracer::render(const render::Frame& frame, const glm::mat4& view, const glm::mat4& proj) {
    using render::BufferUse;
    using render::Old;
    using render::Pass;
    const VkCommandBuffer cmd = frame.cmd;
    const uint32_t frameSlot = frame.slot;
    if (!m_rtPipeline || m_tlas == VK_NULL_HANDLE) return;
    if (frameSlot >= MAX_FRAMES_IN_FLIGHT) return;
    // This frame slot's fence was waited: its set is no longer read by the GPU.
    const VkDescriptorSet set = m_descriptorSets[frameSlot];
    if (!m_vertexArena || !m_indexArena || !m_meshTable || !m_triangleSlots || !m_materialTable ||
        !m_instances || !m_materialRefs || !m_lightBuffer || !m_emissive || !m_scene) return;   // the renderer's tables
    // One stage is timed at a time: a stage's timer ends where the next begins, the last with the frame's passes.
    // The passes inside are not named.
    std::optional<render::Timed> timed;
    const auto timePass = [&](const char* pass) {
        timed.reset();
        timed.emplace(frame, pass);
    };
    // Caustics (sub-project 24): offline only (the realtime profile's paths end in the radiance
    // cache), on its own program (raygenCaustics; the default raygen has none of its code), when a
    // directional light and a smooth-capable caster are in the scene. OHAO_CAUSTICS_SPLIT_ONLY=1
    // drops the light pass's terms without running it (check_caustics' c1_split_only).
    const bool causticsWanted = m_renderSettings.caustics && m_renderSettings.profile == RTRenderProfile::Offline;
    if (causticsWanted) m_caustics.setScene(*m_scene, gpu::shared::kCausticRough);
    const bool causticsOn = causticsWanted && m_caustics.active() && m_caustics.ensure(m_width * m_height) &&
                            ensureCausticsPipeline();
    // Path guiding's buffers before this frame's set is written (the first guided frame allocates them), and its
    // program beside the one this frame traces with (raygenGuided or raygenCausticsGuided).
    const bool guideOn = m_renderSettings.guiding && m_guiding.ensure(m_width * m_height) &&
                         ensureGuidedProgram(causticsOn);
    static const bool causticsSplitOnly = std::getenv("OHAO_CAUSTICS_SPLIT_ONLY") != nullptr;
    // Caustics step 2 (docs/specs/2026-11-04-caustics-step2-design.md): with caustics, whenever an owned caster is
    // in the scene. OHAO_MANIFOLD_OFF=1 leaves step 1 alone (check_caustics' m3_sum); OHAO_MANIFOLD_SPLIT_ONLY=1 drops
    // the pass's terms without running it.
    static const bool manifoldOff = std::getenv("OHAO_MANIFOLD_OFF") != nullptr;
    static const bool manifoldSplitOnly = std::getenv("OHAO_MANIFOLD_SPLIT_ONLY") != nullptr;
    // The manifold pass adds its light to the running mean after raygenCaustics has packed the denoiser's diffuse and
    // specular inputs, so a denoiser that builds the frame from those (NRD) would never show it, while the camera side
    // dropped step 2a's share: under NRD the pass stays off and the camera side keeps those terms.
    const bool manifoldFrame = !denoiseNeedsDiffSpecSplit(m_renderSettings.denoiseMode);
    if (causticsOn && !manifoldOff && manifoldFrame)
        m_manifold.prepare(frameSlot, *m_scene, gpu::shared::kCausticRough);
    const bool manifoldOn = causticsOn && !manifoldOff && manifoldFrame && m_manifold.active() &&
                            m_manifold.ensureRequests(m_width * m_height) &&
                            (manifoldSplitOnly || (m_manifoldRounds.ensure(m_width, m_height, m_caustics.lightCount()) && ensureManifoldPipeline()));
    const bool manifoldPass = manifoldOn && !manifoldSplitOnly;
    // The emissive triangle lights: this frame's table, before anything binds or traces them.
    m_emissive->prepare(frameSlot, m_emissiveInstances, m_emissiveVersion, m_instanceSlotEnd, m_analyticLightPower);
    timePass("emissive.lights");
    m_emissive->record(frame, *m_scene, m_textureSet);
    // Caustics step 2: the owned casters' triangles, clusters and cones, rebuilt every frame the pass runs.
    if (manifoldOn) {
        timePass("pt.manifold.gather");
        m_manifold.record(frame, *m_scene);
    }
    // The frame's setup is timed with its first trace.
    const char* const firstTrace = m_renderSettings.radianceCache && m_radianceCache.capacity() > 1u ? "pt.cache.update"
                                   : causticsOn                                                     ? "pt.caustics"
                                                                                                    : "pt.trace";
    timePass(firstTrace);

#ifdef OHAO_DLSS_ENABLED
    // --- Phase 1: lazy DLSS-RR NGX init + feature creation ---
    // First frame in DLSSRR mode: initialize NGX and create the DLSS-RR feature at
    // render resolution. NGX records feature-setup GPU work into `cmd`, which the
    // renderer submits with this frame. One-shot (m_dlssInitAttempted latches even
    // on failure). Phase 1 does NOT dispatch denoising — after creation the render
    // falls through to raw beauty output.
    if (m_renderSettings.denoiseMode == DenoiseMode::DLSSRR && !m_dlssInitAttempted) {
        m_dlssInitAttempted = true;
        m_dlssRR = std::make_unique<DlssRR>();
        if (m_dlssInstance != VK_NULL_HANDLE &&
            m_dlssRR->initialize(m_dlssInstance, m_physicalDevice, m_device,
                                 OHAO_DLSS_SNIPPET_DIR, OHAO_DLSS_APPDATA_DIR)) {
            if (!m_dlssRR->createFeature(cmd, m_width, m_height, m_outW, m_outH, dlssQualityFromEnv()) ||
                !m_dlssRR->createTonemapPipeline()) {
                m_dlssRR->shutdown();
                m_dlssRR.reset();
            }
        } else {
            OHAO_LOG(Warn, render, "DLSS-RR: init skipped/failed (no VkInstance or NGX init failed)"
                                   " — falling back to raw output");
            m_dlssRR.reset();
        }
    }
#endif

    // --- Sub-plan 4.F T4: Halton(2,3) pixel jitter for NRD temporal diversity ---
    // Realtime 1spp + jitter + temporal ≈ offline 8-16spp equivalent quality: the
    // sub-pixel offset shifts the sample grid each frame so NRD's temporal
    // accumulation integrates over a wider footprint than center-sampling would.
    // Period 16 is enough to hit every 4x4 sub-pixel cell twice over 32 frames —
    // classic TAA pattern. Skip index 0 (Halton value is 0 there, which would
    // equal "no jitter" and waste one of the 16 slots).
    auto halton = [](uint32_t i, uint32_t base) -> float {
        float f = 1.0f, r = 0.0f;
        while (i > 0) { f /= float(base); r += f * float(i % base); i /= base; }
        return r;
    };
    // DLSS-RR (Phase 5) also REQUIRES sub-pixel camera jitter — same Halton(2,3)
    // pattern NRD uses. The offset is fed to InJitterOffsetX/Y so DLSS can align
    // and integrate the sub-pixel samples across frames.
    // Policy: denoiseNeedsPixelJitter (NRD + DLSS-RR) — single source in rt_meta.
    // An upscaling DLSS needs more phases to cover each output pixel: 8 x the area ratio
    // (NVIDIA's DLSS programming guide), 16 at least.
    if (denoiseNeedsPixelJitter(m_renderSettings.denoiseMode)) {
        const float ratio = float(m_outW) / float(std::max(m_width, 1u));
        const uint32_t phases = std::max(16u, uint32_t(std::ceil(8.0f * ratio * ratio)));
        const uint32_t idx = (m_haltonIndex % phases) + 1u;
        m_jitterCurrent = glm::vec2(halton(idx, 2) - 0.5f, halton(idx, 3) - 0.5f);
        m_haltonIndex++;
    } else {
        // None and OIDN keep pixel-center sampling — zero jitter preserves parity.
        m_jitterCurrent = glm::vec2(0.0f);
    }

    // --- Update descriptor set with current frame's data ---

    // Binding 0: TLAS
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    VkAccelerationStructureKHR tlas = m_tlas;
    asWrite.pAccelerationStructures = &tlas;

    // Bindings 1 and 3: this frame's accumulation (written) and the previous frame's
    // (read: temporal history). Ping-ponged, so no invocation reads a pixel that another
    // invocation of this dispatch writes.
    const uint32_t accumWrite = m_accumWriteIndex;
    const uint32_t accumPrev = 1u - accumWrite;
    m_accumLatest = accumWrite;
    const VkDescriptorImageInfo accumInfo{VK_NULL_HANDLE, m_accum[accumWrite].view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo accumHistoryInfo{VK_NULL_HANDLE, m_accum[accumPrev].view, VK_IMAGE_LAYOUT_GENERAL};

    // Binding 2: Output image (storage image, RGBA8)
    VkDescriptorImageInfo outputInfo{};
    outputInfo.imageView = image(Target::Output).view;
    outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Binding 6: Albedo AOV (storage image, RGBA32F)
    VkDescriptorImageInfo albedoAOVInfo{};
    albedoAOVInfo.imageView = image(Target::Albedo).view;
    albedoAOVInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Binding 7: Normal AOV (storage image, RGBA32F)
    VkDescriptorImageInfo normalAOVInfo{};
    normalAOVInfo.imageView = image(Target::Normal).view;
    normalAOVInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;


    VkWriteDescriptorSet writes[64] = {};
    uint32_t writeCount = 0;
    const auto addImage = [&](uint32_t binding, const VkDescriptorImageInfo* info) {
        VkWriteDescriptorSet& w = writes[writeCount++];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = info;
    };

    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = set;
    writes[writeCount].dstBinding = P::scene.binding;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[writeCount].pNext = &asWrite;
    writeCount++;

    addImage(P::accumBuffer.binding, &accumInfo);
    addImage(P::outputImage.binding, &outputInfo);
    addImage(P::accumHistory.binding, &accumHistoryInfo);
    addImage(P::albedoAOV.binding, &albedoAOVInfo);
    addImage(P::normalAOV.binding, &normalAOVInfo);
    const VkDescriptorImageInfo emissionInfo{VK_NULL_HANDLE, image(Target::Emission).view, VK_IMAGE_LAYOUT_GENERAL};
    addImage(P::emissionAOV.binding, &emissionInfo);
    // Binding 43: the emissive lights' power image, every level, sampled (read with Load).
    const VkDescriptorImageInfo powerInfo{VK_NULL_HANDLE, m_emissive->power().view, VK_IMAGE_LAYOUT_GENERAL};
    {
        VkWriteDescriptorSet& w = writes[writeCount++];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = P::emissivePower.binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = &powerInfo;
    }

    // RenderScene buffers: 4 vertex arena, 5 index arena, 8 mesh table, 9 triangle
    // material slots, 10 material table, 11 RT lights, 36 instances, 37 material refs.
    const struct { uint32_t binding; VkBuffer buffer; } sceneBuffers[] = {
        {P::vertices.binding, m_vertexArena}, {P::indices.binding, m_indexArena}, {P::meshes.binding, m_meshTable}, {P::triangleSlots.binding, m_triangleSlots},
        {P::materials.binding, m_materialTable}, {P::lightData.binding, m_lightBuffer}, {P::instances.binding, m_instances}, {P::materialRefs.binding, m_materialRefs},
        {P::openpbrTables.binding, m_openpbrTables.buffer()}, {P::cacheKeys.binding, m_radianceCache.keys()}, {P::cacheAccum.binding, m_radianceCache.accum()},
        {P::cacheResolved.binding, m_radianceCache.resolved()}, {P::emissiveLights.binding, m_emissive->lights().buffer}, {P::emissiveRanges.binding, m_emissive->ranges(frameSlot).buffer},
        {P::emissiveFirst.binding, m_emissive->first(frameSlot).buffer}, {P::guideStates.binding, m_guiding.states()}, {P::guideProposals.binding, m_guiding.proposals()},
        {P::guidePixel.binding, m_guiding.pixel()}, {P::causticCasters.binding, m_caustics.table()}, {P::causticSplat.binding, m_caustics.splat()}, {P::causticStats.binding, m_caustics.stats()},
        {P::manifoldTris.binding, m_manifold.tris()}, {P::manifoldNodes.binding, m_manifold.nodes()}, {P::manifoldDraws.binding, m_manifold.draws(frameSlot)}, {P::manifoldRequests.binding, m_manifold.requests()},
        {P::manifoldRecords.binding, m_manifoldRounds.records()}, {P::manifoldItems.binding, m_manifoldRounds.items()}, {P::manifoldRoots.binding, m_manifoldRounds.roots()},
        {P::manifoldCounts.binding, m_manifoldRounds.counts()}, {P::manifoldSum.binding, m_manifoldRounds.sum()},
    };
    VkDescriptorBufferInfo sceneBufferInfos[std::size(sceneBuffers)]{};
    for (std::size_t b = 0; b < std::size(sceneBuffers); ++b) {
        sceneBufferInfos[b] = VkDescriptorBufferInfo{sceneBuffers[b].buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet& w = writes[writeCount++];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = sceneBuffers[b].binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &sceneBufferInfos[b];
    }

    // Binding 12: bindless textures. The whole array is written (entries past the table's views
    // repeat view 0), so no index below the capacity reads an unwritten descriptor; the shaders
    // treat indices >= the capacity as no texture.
    std::vector<VkDescriptorImageInfo> texInfos;
    const uint32_t textureCount = m_bindlessTextureCount > 0 ? m_bindlessCapacity : 0u;
    texInfos.resize(textureCount);
    for (uint32_t i = 0; i < textureCount; i++) {
        const uint32_t src = i < m_bindlessTextureCount ? i : 0u;
        texInfos[i].imageView = m_bindlessImageViews[src];
        texInfos[i].sampler = m_bindlessSamplers[src];
        texInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    if (textureCount > 0) {
        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[writeCount].dstSet = set;
        writes[writeCount].dstBinding = P::textures.binding;
        writes[writeCount].descriptorCount = textureCount;
        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[writeCount].pImageInfo = texInfos.data();
        writeCount++;
    }

    // Binding 17: the environment's 2D CDF (a one-float buffer without an environment).
    VkDescriptorBufferInfo envCdfInfo{};
    if (m_envCdf && m_envCdf->buffer != VK_NULL_HANDLE) {
        envCdfInfo.buffer = m_envCdf->buffer;
        envCdfInfo.offset = 0;
        envCdfInfo.range = VK_WHOLE_SIZE;
        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[writeCount].dstSet = set;
        writes[writeCount].dstBinding = P::envCdf.binding;
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[writeCount].pBufferInfo = &envCdfInfo;
        writeCount++;
    }

    // Binding 19: motion vector AOV
    VkDescriptorImageInfo motionVectorInfo{};
    motionVectorInfo.imageView = image(Target::Motion).view;
    motionVectorInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = set;
    writes[writeCount].dstBinding = P::motionVector.binding;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo = &motionVectorInfo;
    writeCount++;

    // Binding 20: depth AOV — Sub-plan 3.B
    VkDescriptorImageInfo depthInfo{};
    depthInfo.imageView   = image(Target::Depth).view;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = set;
    writes[writeCount].dstBinding      = P::depthAOV.binding;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &depthInfo;
    writeCount++;

    // Binding 21: roughness AOV — Sub-plan 3.B
    VkDescriptorImageInfo roughInfo{};
    roughInfo.imageView   = image(Target::Roughness).view;
    roughInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = set;
    writes[writeCount].dstBinding      = P::roughnessAOV.binding;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &roughInfo;
    writeCount++;

    // Binding 22: diffuse radiance — Sub-plan 3.C
    VkDescriptorImageInfo diffRadianceInfo{};
    diffRadianceInfo.imageView   = image(Target::DiffuseRadiance).view;
    diffRadianceInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = set;
    writes[writeCount].dstBinding      = P::diffuseRadiance.binding;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &diffRadianceInfo;
    writeCount++;

    // Binding 23: specular radiance — Sub-plan 3.C
    VkDescriptorImageInfo specRadianceInfo{};
    specRadianceInfo.imageView   = image(Target::SpecularRadiance).view;
    specRadianceInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = set;
    writes[writeCount].dstBinding      = P::specularRadiance.binding;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &specRadianceInfo;
    writeCount++;

    // Binding 24: diffuse albedo — Sub-plan 3.C.6
    VkDescriptorImageInfo diffAlbedoInfo{};
    diffAlbedoInfo.imageView   = image(Target::DiffuseAlbedo).view;
    diffAlbedoInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = set;
    writes[writeCount].dstBinding      = P::diffuseAlbedoAOV.binding;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &diffAlbedoInfo;
    writeCount++;

    // Binding 25: specular color — Sub-plan 3.C.6
    VkDescriptorImageInfo specColorInfo{};
    specColorInfo.imageView   = image(Target::SpecularColor).view;
    specColorInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = set;
    writes[writeCount].dstBinding      = P::specColorAOV.binding;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &specColorInfo;
    writeCount++;

    // Binding 26: normal+roughness packed — Sub-plan 4.B
    VkDescriptorImageInfo normalRoughnessInfo{};
    normalRoughnessInfo.imageView   = image(Target::NormalRoughness).view;
    normalRoughnessInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = set;
    writes[writeCount].dstBinding      = P::normalRoughnessAOV.binding;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &normalRoughnessInfo;
    writeCount++;

    vkUpdateDescriptorSets(m_device, writeCount, writes, 0, nullptr);

    // --- Bind pipeline and descriptors ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_rtPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            m_pipelineLayout, 0, 1, &set, 0, nullptr);

    // --- Push constants ---
    PTPushConstants pc{};
    pc.invView = glm::inverse(view);
    pc.invProj = glm::inverse(proj);
    pc.prevViewProj = m_prevViewProj;
    // A moved view restarts the trace's running mean (not the denoisers' history).
    if (m_viewChangedThisFrame) m_accumFrames = 0;
    pc.params = glm::uvec4(m_width, m_height, m_sampleIndex, m_maxBounces | (m_guiding.depths() << 16u));
    pc.control = glm::uvec4(0u);
    if (m_renderSettings.enableAuxiliaryAOVs) pc.control.x |= kPTFlagEnableAOVs;
    // The firefly clamp (biased) only protects a temporal denoiser fed one sample a frame; a running
    // mean converges without it.
    if (m_renderSettings.enableFireflyClamp && !tracerAccumulates(m_renderSettings))
        pc.control.x |= kPTFlagEnableFireflyClamp;
    if (tracerAccumulatesAOVs(m_renderSettings)) pc.control.x |= kPTFlagAccumulateAOVs;
    if (m_renderSettings.denoiseMode == DenoiseMode::DLSSRR) pc.control.x |= kPTFlagDLSSRR;
    const bool cacheOn = m_renderSettings.radianceCache && m_radianceCache.capacity() > 1u;
    if (cacheOn) pc.control.x |= kPTFlagRadianceCache;
    if (guideOn) pc.control.x |= kPTFlagGuiding;
    if (causticsOn) {
        pc.control.x |= kPTFlagCaustics;
        m_causticsRan = true;
    }
    static const char* guideDebug = std::getenv("OHAO_GUIDE_DEBUG");
    if (guideOn && guideDebug) pc.control.x |= (1u << 7) | ((uint32_t(std::atoi(guideDebug)) & 3u) << 8u);   // kFlagGuideDebug, its mode
    // OHAO_PT_CLASS=mask: caustics' raygen keeps only the light of these path classes (rt/path_tracer.slang's
    // kClass*), to find what a pixel's light is.
    static const char* ptClass = std::getenv("OHAO_PT_CLASS");
    if (causticsOn && ptClass) pc.control.x |= (uint32_t(std::atoi(ptClass)) & 255u) << 11u;
    if (manifoldOn) {
        pc.control.x |= kPTFlagManifold;
        if (manifoldPass) pc.control.x |= kPTFlagManifoldRequests;
        static const char* stage = std::getenv("OHAO_MANIFOLD_STAGE");   // profiling: where the manifold pass stops
        if (stage) pc.control.x |= (uint32_t(std::atoi(stage)) & 3u) << 21u;
        // Step 2b (manifoldGlass), the sun through owned solids: opt-in, OHAO_MANIFOLD_GLASS=1. Its re-find walks run
        // for seconds in a few lanes of some frames (check_caustics m9: 2,048 spp over 15 minutes with it, 42 s without),
        // a GPU dispatch the driver cannot preempt, until its work is bounded per dispatch.
        static const bool glassOn = [] {
            const char* v = std::getenv("OHAO_MANIFOLD_GLASS");
            return v != nullptr && std::string_view(v) != "0";
        }();
        if (glassOn) pc.control.x |= kPTFlagManifoldGlass;
        m_manifoldRan = true;
    }
    // The frames already in the running mean (0: this frame's sample alone, which a temporal
    // denoiser takes), and whether last frame's view gives motion vectors.
    pc.control.y = tracerAccumulates(m_renderSettings) ? m_accumFrames : 0u;
    pc.control.z = m_hasPreviousView ? 1u : 0u;
    pc.control.w = m_envWidth;
    pc.tuning = glm::vec4(m_renderSettings.fireflyClampLuminance, float(m_envHeight), m_displayExposure,
                          std::bit_cast<float>(static_cast<uint32_t>(m_displayCurve)));
    pc.jitter = glm::vec4(m_jitterCurrent.x, m_jitterCurrent.y, m_guiding.share(), m_guiding.prior());

    // Store current viewProj for next frame's reprojection
    m_prevViewProj = proj * view;

    vkCmdPushConstants(cmd, m_pipelineLayout, kPushStages, 0, sizeof(PTPushConstants), &pc);

    // --- What the frame's passes declare ---
    // What every trace reads: the scene's tables and acceleration structures, the emissive lights and their power
    // image (every level, sampled in GENERAL), OpenPBR's tables, the environment (in the texture table) and its CDF.
    const render::RenderScene::Buffers sb = m_scene->buffers();
    const auto readsScene = [&](Pass& pass) {
        for (GpuBuffer* b : {&sb.vertices, &sb.indices, &sb.meshes, &sb.triangleSlots, &sb.materials, &sb.rtLights,
                             &sb.instances, &sb.materialRefs})
            pass.read(*b, BufferUse::Storage);
        m_scene->traced(pass);
        pass.read(m_emissive->lights(), BufferUse::Storage)
            .read(m_emissive->ranges(frameSlot), BufferUse::Storage)
            .read(m_emissive->first(frameSlot), BufferUse::Storage);
        pass.sampledGeneral(m_emissive->power());
        pass.read(m_openpbrTables.tracked(), BufferUse::Storage);
        if (m_envCdf) pass.read(*m_envCdf, BufferUse::Storage);
        if (m_envMap) pass.sampled(*m_envMap);
    };
    // The images the frame's trace writes, declared by the first trace of the frame: every program binds the one
    // set, and each image of it is in its descriptor's layout from that trace on. This frame's accumulation image
    // is rewritten whole (its old contents are not wanted) from the previous frame's, which is only read; the
    // guides are rewritten whole, or under offline NRD blended into the frames accumulated, as the emission is.
    bool targetsDeclared = false;
    const auto writesTargets = [&](Pass& pass) {
        if (std::exchange(targetsDeclared, true)) return;
        pass.storageWrite(m_accum[accumWrite]).storageRead(m_accum[accumPrev]).storageWrite(image(Target::Output));
        if (m_renderSettings.enableAuxiliaryAOVs) {
            for (Target t : {Target::Albedo, Target::Normal, Target::Motion, Target::Depth, Target::Roughness})
                pass.storageWrite(image(t));
            const bool blended = tracerAccumulatesAOVs(m_renderSettings);
            for (Target t : {Target::DiffuseRadiance, Target::SpecularRadiance, Target::DiffuseAlbedo,
                             Target::SpecularColor, Target::NormalRoughness}) {
                if (blended) pass.storageReadWrite(image(t));
                else pass.storageWrite(image(t));
            }
        }
        pass.storageReadWrite(image(Target::Emission));
    };
    // The frame's trace (raygen, raygenGuided, raygenCaustics or raygenCausticsGuided).
    const auto tracesFrame = [&](Pass& pass) {
        readsScene(pass);
        writesTargets(pass);
        if (cacheOn) m_radianceCache.lookedUpBy(pass);
        if (guideOn) m_guiding.tracedBy(pass);
        if (causticsOn) m_caustics.cameraPass(pass);
        if (manifoldOn) m_manifold.cameraPass(pass, frameSlot);
    };

    // --- The radiance cache: this frame's update (1 pixel in 5x5) and resolve, before the trace
    //     that ends its paths in it. ---
    if (cacheOn) {
        m_radianceCache.recordBeginFrame(frame);
        {
            Pass update(frame, nullptr, Pass::RayTracing);
            readsScene(update);
            writesTargets(update);
            m_radianceCache.fedBy(update);
            update.begin();
            vkCmdTraceRaysKHR(cmd, &m_cacheRgenRegion, &m_missRegion, &m_hitRegion, &m_callRegion,
                              (m_width + 4u) / 5u, (m_height + 4u) / 5u, 1);
        }
        timePass("pt.cache.resolve");
        m_radianceCache.recordResolve(frame);
        // Push constants are shared by the bind points: the resolve's replaced the trace's.
        vkCmdPushConstants(cmd, m_pipelineLayout, kPushStages, 0, sizeof(PTPushConstants), &pc);
    }

    // --- Trace rays! ---
    if (cacheOn) timePass("pt.trace");
    if (guideOn) m_guiding.recordBeginFrame(frame);
    if (causticsOn) {
        // Caustics' program, or with guiding its guided twin (the same groups and regions).
        const GuidedProgram& cg = m_guided[1];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, guideOn ? cg.pipeline : m_causticsPipeline);
        const VkStridedDeviceAddressRegionKHR* cRgen = guideOn ? &cg.rgen : &m_causticsRgenRegion;
        const VkStridedDeviceAddressRegionKHR* cMiss = guideOn ? &cg.miss : &m_causticsMissRegion;
        const VkStridedDeviceAddressRegionKHR* cHit = guideOn ? &cg.hit : &m_causticsHitRegion;
        const VkStridedDeviceAddressRegionKHR* cTrace = guideOn ? &cg.extra : &m_causticTraceRegion;
        m_caustics.recordBeginFrame(frame);
        if (!causticsSplitOnly) {
            // kCausticRaysPerPixel light paths a pixel; OHAO_CAUSTIC_RAYS (any positive number) overrides it for
            // tuning (ruling C3). The pass counts its paths from the dispatch's size.
            static const float raysPerPixel = [] {
                const char* s = std::getenv("OHAO_CAUSTIC_RAYS");
                const float v = s ? std::strtof(s, nullptr) : 0.0f;
                return v > 0.0f ? v : float(gpu::shared::kCausticRaysPerPixel);
            }();
            const uint32_t rows = std::max(1u, uint32_t(std::lround(double(m_height) * raysPerPixel)));
            Pass light(frame, nullptr, Pass::RayTracing);
            readsScene(light);
            writesTargets(light);
            m_caustics.lightPass(light);
            light.begin();
            vkCmdTraceRaysKHR(cmd, cTrace, cMiss, cHit, &m_callRegion, m_width, rows, 1);
        }
        timePass("pt.trace");
        {
            // The light pass's splats are read (and cleared) here.
            Pass trace(frame, nullptr, Pass::RayTracing);
            tracesFrame(trace);
            trace.begin();
            vkCmdTraceRaysKHR(cmd, cRgen, cMiss, cHit, &m_callRegion, m_width, m_height, 1);
        }
        if (manifoldPass) {
            // Caustics step 2: the requests raygenCaustics wrote, answered into the running mean it wrote, in batches
            // of rows, each in rounds of bounded items (render/rt/manifold_rounds.hpp; the same set and push constants
            // throughout). Timed whole as pt.manifold, or with OHAO_MANIFOLD_TIMING=stages each stage of each batch on
            // its own.
            static const bool stageTimes = [] {
                const char* e = std::getenv("OHAO_MANIFOLD_TIMING");
                return e && std::string_view(e) == "stages";
            }();
            const auto stage = [&](const char* name) {
                if (stageTimes) timePass(name);
            };
            if (!stageTimes) timePass("pt.manifold");
            m_manifoldRounds.recordReset(frame);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_manifoldPipeline);
            using R = render::ManifoldRounds;
            const VkDeviceAddress counts = getBufferDeviceAddress(m_manifoldRounds.counts());
            // The rounds a pass: the walk's four (one a queue, records to sub-clusters), the solve on the candidates, the
            // connect on the roots: round r on queue r.
            constexpr uint32_t kRounds = 6;
            // The batch and pass are pushed as pc.jitter.z's bits (batch | pass << 16; path guiding's word, which no
            // pass after the trace reads; put back after the last batch).
            constexpr uint32_t kBatchOffset = offsetof(PTPushConstants, jitter) + 2 * sizeof(float);
            const uint32_t rows = m_manifoldRounds.rowsPerBatch();
            for (uint32_t b = 0; b < m_manifoldRounds.batches(); ++b) {
                vkCmdPushConstants(cmd, m_pipelineLayout, kPushStages, kBatchOffset, sizeof b, &b);
                stage("pt.manifold.setup");
                {
                    // The batch's rows.
                    Pass setup(frame, nullptr, Pass::RayTracing);
                    readsScene(setup);
                    m_caustics.manifoldPass(setup);
                    m_manifold.manifoldPass(setup, frameSlot);
                    m_manifoldRounds.setupPass(setup);
                    setup.begin();
                    vkCmdTraceRaysKHR(cmd, &m_manifoldRgenRegion, &m_manifoldMissRegion, &m_manifoldHitRegion,
                                      &m_callRegion, m_width, std::min(rows, m_height - b * rows), 1);
                }
                // Each pass the rounds follow the levels (records, groups, clusters, sub-clusters), then the solve and
                // the connect: each traces as many threads as its queue holds this pass (the items the rounds before
                // it queued, and those it carried from the last pass). A pass after the first is all carries.
                for (uint32_t p = 0; p < R::kPasses; ++p) {
                    const uint32_t bp = b | p << 16;
                    if (p > 0) vkCmdPushConstants(cmd, m_pipelineLayout, kPushStages, kBatchOffset, sizeof bp, &bp);
                    for (uint32_t r = 0; r < kRounds; ++r) {
                        if (p == 0 && r == 0) stage("pt.manifold.walk");
                        if (p == 0 && r == R::Candidates) stage("pt.manifold.solve");
                        if (p == 0 && r == R::Roots) stage("pt.manifold.connect");
                        if (p == 1 && r == 0) stage("pt.manifold.carry");
                        Pass round(frame, nullptr, Pass::RayTracing);
                        m_manifold.roundPass(round, frameSlot);
                        if (r < R::Candidates) m_manifoldRounds.walkPass(round);
                        else if (r == R::Candidates) m_manifoldRounds.solvePass(round);
                        else {
                            readsScene(round);
                            m_manifoldRounds.connectPass(round);
                        }
                        if (r >= R::Candidates) m_caustics.manifoldPass(round);
                        round.begin();
                        vkCmdTraceRaysIndirectKHR(cmd, &m_manifoldRoundRegions[r], &m_manifoldMissRegion,
                                                  &m_manifoldHitRegion, &m_callRegion,
                                                  counts + R::argsOffset(b, p, static_cast<R::Queue>(r)));
                    }
                }
            }
            vkCmdPushConstants(cmd, m_pipelineLayout, kPushStages, kBatchOffset, sizeof(float), &pc.jitter.z);
            stage("pt.manifold.resolve");
            {
                Pass resolve(frame, nullptr, Pass::Compute);
                resolve.storageReadWrite(m_accum[accumWrite]).storageWrite(image(Target::Output), Old::Keep);
                m_caustics.manifoldPass(resolve);
                m_manifoldRounds.resolvePass(resolve);
                resolve.begin();
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &set, 0, nullptr);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_manifoldResolvePipe);
                vkCmdDispatch(cmd, (m_width * m_height + 63u) / 64u, 1, 1);
            }
        }
    } else {
        Pass trace(frame, nullptr, Pass::RayTracing);
        tracesFrame(trace);
        trace.begin();
        if (guideOn) {
            const GuidedProgram& g = m_guided[0];
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, g.pipeline);
            vkCmdTraceRaysKHR(cmd, &g.rgen, &g.miss, &g.hit, &m_callRegion, m_width, m_height, 1);
        } else {
            vkCmdTraceRaysKHR(cmd, &m_rgenRegion, &m_missRegion, &m_hitRegion, &m_callRegion, m_width, m_height, 1);
        }
    }
    if (guideOn) {
        // What this frame's paths found becomes the grid the next frame reads.
        timePass("pt.guide.update");
        m_guiding.recordLearn(frame, m_width * m_height, m_sampleIndex, glm::vec3(pc.invView[3]));
    }
    timePass("pt.denoise");

#ifdef OHAO_DLSS_ENABLED
    // --- Phase 5: DLSS Ray Reconstruction denoise + tonemap ---
    // The raygen produced a FRESH 1-spp HDR frame (accum, control.y=0) plus the
    // packed normal/roughness guide (the normal image) this frame. Wrap the guide buffers
    // as NGX resources, run NGX_VULKAN_EVALUATE_DLSSD_EXT, then tonemap the HDR
    // COLOR_OUT into the RGBA8 output the standard readback uses.
    if (m_renderSettings.denoiseMode == DenoiseMode::DLSSRR && m_dlssRR &&
        m_dlssRR->isFeatureCreated()) {
        gpu::Image& colorIn = m_accum[accumWrite];
        gpu::Image& colorOut = image(Target::DlssOutput);
        gpu::Image& diffAlbedo = image(Target::DiffuseAlbedo);
        gpu::Image& specAlbedo = image(Target::SpecularColor);
        gpu::Image& normalRough = image(Target::Normal);
        gpu::Image& depth = image(Target::Depth);
        gpu::Image& motion = image(Target::Motion);

        // Fill eval inputs. glm column-major view/proj passed as-is (DLSS wants
        // row-major + left-multiply; the two transposes cancel — see wrapper).
        const glm::mat4 viewM = glm::inverse(pc.invView);
        const glm::mat4 projM = glm::inverse(pc.invProj);

        DlssRR::EvalInputs ei{};
        ei.colorInImage     = colorIn.image;     ei.colorInView     = colorIn.view;     ei.colorInFormat     = colorIn.desc.format;
        ei.colorOutImage    = colorOut.image;    ei.colorOutView    = colorOut.view;    ei.colorOutFormat    = colorOut.desc.format;
        ei.diffAlbedoImage  = diffAlbedo.image;  ei.diffAlbedoView  = diffAlbedo.view;  ei.diffAlbedoFormat  = diffAlbedo.desc.format;
        ei.specAlbedoImage  = specAlbedo.image;  ei.specAlbedoView  = specAlbedo.view;  ei.specAlbedoFormat  = specAlbedo.desc.format;
        ei.normalRoughImage = normalRough.image; ei.normalRoughView = normalRough.view; ei.normalRoughFormat = normalRough.desc.format;
        ei.depthImage       = depth.image;       ei.depthView       = depth.view;       ei.depthFormat       = depth.desc.format;
        ei.motionImage      = motion.image;      ei.motionView      = motion.view;      ei.motionFormat      = motion.desc.format;
        ei.renderW = m_width; ei.renderH = m_height;
        ei.outW = m_outW; ei.outH = m_outH;   // DLSS upscales into the output size
        ei.jitterX = m_jitterCurrent.x; ei.jitterY = m_jitterCurrent.y;
        ei.mvScaleX = -1.0f; ei.mvScaleY = -1.0f;   // OHAO writes currPix-prevPix; DLSS wants prevPix-currPix
        ei.worldToView = glm::value_ptr(viewM);
        ei.viewToClip  = glm::value_ptr(projM);
        // Reset only on the genuine first frame / accumulation reset (NOT camera
        // move — DLSS reprojects through motion via the motion vectors).
        ei.reset = (m_historyFrameCount == 0u);

        bool evaluated = false;
        {
            // NGX records barriers of its own: it takes every image in GENERAL, the raygen's writes visible to
            // its compute shaders' reads and writes, and leaves each in GENERAL.
            Pass dlss(frame, nullptr, Pass::Compute);
            for (gpu::Image* img : {&colorIn, &colorOut, &diffAlbedo, &specAlbedo, &normalRough, &depth, &motion})
                dlss.external(*img).leaves(*img, VK_IMAGE_LAYOUT_GENERAL);
            dlss.begin();
            evaluated = m_dlssRR->evaluate(cmd, ei);
        }
        if (evaluated) {
            // DLSS's HDR frame through the display transform into the output (over the raygen's write).
            Pass display(frame, nullptr, Pass::Compute);
            display.storageRead(colorOut).storageWrite(image(Target::Output), Old::Keep);
            display.begin();
            m_dlssRR->tonemap(cmd, frameSlot, colorOut.view, image(Target::Output).view, m_displayExposure,
                              static_cast<uint32_t>(m_displayCurve), m_outW, m_outH);
        }
    }
#endif  // OHAO_DLSS_ENABLED

#ifdef OHAO_NRD_ENABLED
    // --- NRD REBLUR_DIFFUSE_SPECULAR, then nrd_compose, which writes the frame's output (and its HDR) ---
    // Only in NRD mode.
    if (m_nrdDenoiser && m_renderSettings.enableAuxiliaryAOVs && m_renderSettings.denoiseMode == DenoiseMode::NRD) {
        if (m_nrdCompositor) m_nrdCompositor->setFrameSlot(frameSlot);

        NrdCameraInputs camera {};
        const glm::mat4 viewM = glm::inverse(pc.invView);
        const glm::mat4 projM = glm::inverse(pc.invProj);
        std::memcpy(camera.viewMatrix.data(),     glm::value_ptr(viewM),               sizeof(float) * 16);
        std::memcpy(camera.viewMatrixPrev.data(), glm::value_ptr(m_prevViewMatrix),    sizeof(float) * 16);
        std::memcpy(camera.projMatrix.data(),     glm::value_ptr(projM),               sizeof(float) * 16);
        std::memcpy(camera.projMatrixPrev.data(), glm::value_ptr(m_prevProjMatrix),    sizeof(float) * 16);
        camera.motionVectorScale = {1.0f, 1.0f, 0.0f};
        // Sub-plan 4.F T4: feed Halton jitter to NRD so it undoes the sub-pixel
        // shift during temporal reprojection (NRD expects "where this frame's ray
        // origin was offset to vs the canonical pixel center"). jitterPrev is
        // LAST frame's jitter; m_jitterPrev is captured at end of previous
        // render() (see below). On first frame + every view reset, both are 0,
        // which matches the frameIndex=0 bootstrap branch.
        camera.jitter     = {m_jitterCurrent.x, m_jitterCurrent.y};
        camera.jitterPrev = {m_jitterPrev.x,    m_jitterPrev.y};
        camera.frameIndex = m_historyFrameCount;   // 0 only after resetAccumulation: a moved view reprojects
        camera.isMotionVectorInWorldSpace = false;

        m_nrdDenoiser->setCommonSettings(camera);

        // 4.E T2: capture current frame's V/P for NEXT frame's NRD input.
        // On the first frame m_prev*Matrix are identity, so NRD sees "no
        // history" (spatial-only) — matches pre-T2 behavior exactly.
        m_prevViewMatrix = viewM;
        m_prevProjMatrix = projM;
        // 4.F T4: capture current jitter as jitterPrev for next frame's NRD
        // reprojection. Paired with viewChanged → NRD sees both "same pose" and
        // "same jitter" → zero disocclusion, stays in temporal-only mode.
        m_jitterPrev = m_jitterCurrent;

        const auto resource = [&](Target t) {
            return NrdDenoiser::NrdInputResource{image(t).image, image(t).desc.format};
        };
        NrdDenoiser::NrdInputResources res {};
        res.motionVector           = resource(Target::Motion);
        res.viewZ                  = resource(Target::Depth);
        res.normalRoughness        = resource(Target::NormalRoughness);
        res.diffRadianceHitDist    = resource(Target::DiffuseRadiance);
        res.specRadianceHitDist    = resource(Target::SpecularRadiance);
        res.outDiffRadianceHitDist = resource(Target::DenoisedDiffuse);
        res.outSpecRadianceHitDist = resource(Target::DenoisedSpecular);
        m_nrdDenoiser->setInputResources(res);

        {
            // NRI records barriers of its own. NrdDenoiser::denoise tells it each image comes in GENERAL, last
            // touched as a storage image by a compute shader, and to give it back that way (restoreInitialState):
            // its inputs with the raygen's writes, its outputs with nothing wanted.
            Pass nrd(frame, nullptr, Pass::Compute);
            for (Target t : {Target::Motion, Target::Depth, Target::NormalRoughness, Target::DiffuseRadiance,
                             Target::SpecularRadiance})
                nrd.storageReadWrite(image(t)).leaves(image(t), VK_IMAGE_LAYOUT_GENERAL);
            for (Target t : {Target::DenoisedDiffuse, Target::DenoisedSpecular})
                nrd.storageWrite(image(t)).leaves(image(t), VK_IMAGE_LAYOUT_GENERAL);
            nrd.begin();
            m_nrdDenoiser->denoise(cmd);
        }

        if (m_nrdCompositor) {
            // NRD's denoised diffuse remodulated by its albedo, its specular and the camera ray's emission: the
            // HDR frame, and through the display transform the output (over the raygen's write).
            Pass compose(frame, nullptr, Pass::Compute);
            for (Target t : {Target::DiffuseAlbedo, Target::SpecularColor, Target::DenoisedDiffuse,
                             Target::DenoisedSpecular, Target::Emission})
                compose.storageRead(image(t));
            compose.storageWrite(image(Target::NrdComposed), Old::Keep).storageWrite(image(Target::Output), Old::Keep);
            compose.begin();
            NrdComposeInputs ci {};
            ci.diffRadiance = image(Target::DenoisedDiffuse).view;
            ci.specRadiance = image(Target::DenoisedSpecular).view;
            ci.diffAlbedo   = image(Target::DiffuseAlbedo).view;
            ci.specColor    = image(Target::SpecularColor).view;
            ci.emission     = image(Target::Emission).view;
            ci.composedOut  = image(Target::NrdComposed).view;
            ci.outputLdr    = image(Target::Output).view;   // the frame: a window presents it, a capture reads it
            ci.exposure     = m_displayExposure;
            ci.curve        = static_cast<uint32_t>(m_displayCurve);
            m_nrdCompositor->dispatch(cmd, ci);
        }
    }
#endif  // OHAO_NRD_ENABLED

    // Advance sample sequence and track accumulation history separately.
    m_sampleIndex++;
    m_historyFrameCount++;
    m_accumFrames++;
    m_viewChangedThisFrame = false;
    m_hasPreviousView = true;
    m_accumWriteIndex = accumPrev;   // accum() stays the image just written
}

} // namespace ohao
