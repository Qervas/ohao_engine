// Extracted from path_tracer.cpp — per-frame render() method.
// Kept as a member of class PathTracer; no behavior change.
// Push-constant flag constants (kPTFlag*) moved here since this TU is their sole consumer.

#include "path_tracer.hpp"

#include <algorithm>
#include <cstring>
#include <vector>
#include "core/log.hpp"
#include <glm/gtc/type_ptr.hpp>

// nrd_denoise.hpp / nrd_compose.hpp are safe to include unconditionally; their
// dispatch methods are OHAO_NRD_ENABLED-guarded inside this TU.
#include "render/rt/denoise/nrd_denoise.hpp"
#include "render/rt/denoise/nrd_compose.hpp"
#include "render/rt/denoise/atrous_denoise.hpp"
#ifdef OHAO_DLSS_ENABLED
#include "render/rt/denoise/dlss_rr.hpp"
#endif

namespace ohao {

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
}  // namespace

void PathTracer::render(VkCommandBuffer cmd, uint32_t frameSlot, const glm::mat4& view, const glm::mat4& proj) {
    if (!m_rtPipeline || m_tlas == VK_NULL_HANDLE) return;
    if (frameSlot >= MAX_FRAMES_IN_FLIGHT) return;
    // This frame slot's fence was waited: its set is no longer read by the GPU.
    const VkDescriptorSet set = m_descriptorSets[frameSlot];
    if (!m_vertexArena || !m_indexArena || !m_meshTable || !m_triangleSlots || !m_materialTable ||
        !m_instances || !m_materialRefs || !m_lightBuffer) return;

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
            if (!m_dlssRR->createFeature(cmd, m_width, m_height, m_width, m_height) ||
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
    if (denoiseNeedsPixelJitter(m_renderSettings.denoiseMode)) {
        const uint32_t idx = (m_haltonIndex % 16u) + 1u;
        m_jitterCurrent = glm::vec2(halton(idx, 2) - 0.5f, halton(idx, 3) - 0.5f);
        m_haltonIndex++;
    } else {
        // None/OIDN/Atrous keep pixel-center sampling — zero jitter preserves parity.
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
    m_accumBuffer = m_accumImages[accumWrite];
    m_accumView = m_accumViews[accumWrite];
    const VkDescriptorImageInfo accumInfo{VK_NULL_HANDLE, m_accumView, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo accumHistoryInfo{VK_NULL_HANDLE, m_accumViews[accumPrev], VK_IMAGE_LAYOUT_GENERAL};

    // Binding 2: Output image (storage image, RGBA8)
    VkDescriptorImageInfo outputInfo{};
    outputInfo.imageView = m_outputView;
    outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Binding 6: Albedo AOV (storage image, RGBA32F)
    VkDescriptorImageInfo albedoAOVInfo{};
    albedoAOVInfo.imageView = m_albedoAOVView;
    albedoAOVInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Binding 7: Normal AOV (storage image, RGBA32F)
    VkDescriptorImageInfo normalAOVInfo{};
    normalAOVInfo.imageView = m_normalAOVView;
    normalAOVInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;


    VkWriteDescriptorSet writes[41] = {};
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
    writes[writeCount].dstBinding = 0;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[writeCount].pNext = &asWrite;
    writeCount++;

    addImage(1, &accumInfo);
    addImage(2, &outputInfo);
    addImage(3, &accumHistoryInfo);
    addImage(6, &albedoAOVInfo);
    addImage(7, &normalAOVInfo);
    const VkDescriptorImageInfo emissionInfo{VK_NULL_HANDLE, m_emissionAOVView, VK_IMAGE_LAYOUT_GENERAL};
    addImage(13, &emissionInfo);

    // RenderScene buffers: 4 vertex arena, 5 index arena, 8 mesh table, 9 triangle
    // material slots, 10 material table, 11 RT lights, 36 instances, 37 material refs.
    const struct { uint32_t binding; VkBuffer buffer; } sceneBuffers[] = {
        {4, m_vertexArena}, {5, m_indexArena}, {8, m_meshTable}, {9, m_triangleSlots},
        {10, m_materialTable}, {11, m_lightBuffer}, {36, m_instances}, {37, m_materialRefs},
        {38, m_openpbrTables.buffer()}, {39, m_radianceCache.keys()}, {40, m_radianceCache.accum()},
        {41, m_radianceCache.resolved()},
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

    // Binding 12: bindless textures. The whole array is written (entries past the
    // table's views repeat view 0), so no index below the capacity reads an unwritten
    // descriptor; the shaders treat indices >= the capacity as no texture.
    std::vector<VkDescriptorImageInfo> texInfos;
    const uint32_t textureCount = m_bindlessTextureCount > 0 ? m_bindlessCapacity : 0u;
    if (textureCount > 0) {
        texInfos.resize(textureCount);
        for (uint32_t i = 0; i < textureCount; i++) {
            const uint32_t src = i < m_bindlessTextureCount ? i : 0u;
            texInfos[i].imageView = m_bindlessImageViews[src];
            texInfos[i].sampler = m_bindlessSamplers[src];
            texInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[writeCount].dstSet = set;
        writes[writeCount].dstBinding = 12;
        writes[writeCount].descriptorCount = textureCount;
        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[writeCount].pImageInfo = texInfos.data();
        writeCount++;
    }

    // Binding 17: env marginal CDF (only if set — dummy buffers from env_upload.cpp guarantee this)
    VkDescriptorBufferInfo envMargInfo{};
    if (m_envMarginalCDFBuffer != VK_NULL_HANDLE) {
        envMargInfo.buffer = m_envMarginalCDFBuffer;
        envMargInfo.offset = 0;
        envMargInfo.range = VK_WHOLE_SIZE;
        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[writeCount].dstSet = set;
        writes[writeCount].dstBinding = 17;
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[writeCount].pBufferInfo = &envMargInfo;
        writeCount++;
    }

    // Binding 18: env conditional CDF (only if set)
    VkDescriptorBufferInfo envCondInfo{};
    if (m_envConditionalCDFBuffer != VK_NULL_HANDLE) {
        envCondInfo.buffer = m_envConditionalCDFBuffer;
        envCondInfo.offset = 0;
        envCondInfo.range = VK_WHOLE_SIZE;
        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[writeCount].dstSet = set;
        writes[writeCount].dstBinding = 18;
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[writeCount].pBufferInfo = &envCondInfo;
        writeCount++;
    }

    // Binding 19: motion vector AOV
    VkDescriptorImageInfo motionVectorInfo{};
    motionVectorInfo.imageView = m_motionVectorView;
    motionVectorInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = set;
    writes[writeCount].dstBinding = 19;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo = &motionVectorInfo;
    writeCount++;

    // Binding 20: depth AOV — Sub-plan 3.B
    VkDescriptorImageInfo depthInfo{};
    depthInfo.imageView   = m_depthAOVView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = set;
    writes[writeCount].dstBinding      = 20;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &depthInfo;
    writeCount++;

    // Binding 21: roughness AOV — Sub-plan 3.B
    VkDescriptorImageInfo roughInfo{};
    roughInfo.imageView   = m_roughnessAOVView;
    roughInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = set;
    writes[writeCount].dstBinding      = 21;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &roughInfo;
    writeCount++;

    // Binding 22: diffuse radiance — Sub-plan 3.C
    VkDescriptorImageInfo diffRadianceInfo{};
    diffRadianceInfo.imageView   = m_diffuseRadianceView;
    diffRadianceInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = set;
    writes[writeCount].dstBinding      = 22;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &diffRadianceInfo;
    writeCount++;

    // Binding 23: specular radiance — Sub-plan 3.C
    VkDescriptorImageInfo specRadianceInfo{};
    specRadianceInfo.imageView   = m_specularRadianceView;
    specRadianceInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = set;
    writes[writeCount].dstBinding      = 23;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &specRadianceInfo;
    writeCount++;

    // Binding 24: diffuse albedo — Sub-plan 3.C.6
    VkDescriptorImageInfo diffAlbedoInfo{};
    diffAlbedoInfo.imageView   = m_diffAlbedoView;
    diffAlbedoInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = set;
    writes[writeCount].dstBinding      = 24;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &diffAlbedoInfo;
    writeCount++;

    // Binding 25: specular color — Sub-plan 3.C.6
    VkDescriptorImageInfo specColorInfo{};
    specColorInfo.imageView   = m_specColorView;
    specColorInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = set;
    writes[writeCount].dstBinding      = 25;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &specColorInfo;
    writeCount++;

    // Binding 26: normal+roughness packed — Sub-plan 4.B
    VkDescriptorImageInfo normalRoughnessInfo{};
    normalRoughnessInfo.imageView   = m_normalRoughnessView;
    normalRoughnessInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = set;
    writes[writeCount].dstBinding      = 26;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &normalRoughnessInfo;
    writeCount++;

    vkUpdateDescriptorSets(m_device, writeCount, writes, 0, nullptr);

    // --- The accumulation pair to GENERAL ---
    // This frame's image is fully rewritten: its old contents are discarded (UNDEFINED),
    // after the reads of it as last frame's history. The history image keeps the
    // previous frame's write, made visible here; never written yet, nothing reads it
    // (history is read only once a frame has accumulated).
    {
        VkImageMemoryBarrier accumBarriers[2]{};
        for (auto& b : accumBarriers) {
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        accumBarriers[0].srcAccessMask = 0;
        accumBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        accumBarriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        accumBarriers[0].image = m_accumImages[accumWrite];
        const bool historyWritten = m_accumInitialized[accumPrev];
        accumBarriers[1].srcAccessMask = historyWritten ? VK_ACCESS_SHADER_WRITE_BIT : 0;
        accumBarriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        accumBarriers[1].oldLayout = historyWritten ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        accumBarriers[1].image = m_accumImages[accumPrev];
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 2, accumBarriers);
    }

    // --- Transition output image to GENERAL for storage write ---
    {
        VkImageMemoryBarrier outputBarrier{};
        outputBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        outputBarrier.srcAccessMask = 0;
        outputBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        outputBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        outputBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        outputBarrier.image = m_outputImage;
        outputBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 1, &outputBarrier);
    }

    // --- Transition AOV images to GENERAL for storage write ---
    if (m_renderSettings.enableAuxiliaryAOVs) {
        VkImageMemoryBarrier aovBarriers[12] = {};

        aovBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[0].srcAccessMask = 0;
        aovBarriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[0].image = m_albedoAOV;
        aovBarriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        aovBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[1].srcAccessMask = 0;
        aovBarriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[1].image = m_normalAOV;
        aovBarriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // Sub-plan 3.A: motion vector AOV needs same UNDEFINED→GENERAL transition
        // so raygen can imageStore to it. Safe to transition every frame — the
        // shader overwrites the entire image each pass.
        aovBarriers[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[2].srcAccessMask = 0;
        aovBarriers[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[2].image = m_motionVectorImage;
        aovBarriers[2].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // Sub-plan 3.B: depth AOV barrier
        aovBarriers[3].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[3].srcAccessMask = 0;
        aovBarriers[3].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[3].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[3].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[3].image = m_depthAOVImage;
        aovBarriers[3].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // Sub-plan 3.B: roughness AOV barrier
        aovBarriers[4].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[4].srcAccessMask = 0;
        aovBarriers[4].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[4].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[4].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[4].image = m_roughnessAOVImage;
        aovBarriers[4].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // Sub-plan 3.C: diffuse radiance AOV barrier
        aovBarriers[5].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[5].srcAccessMask = 0;
        aovBarriers[5].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[5].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[5].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[5].image = m_diffuseRadianceImage;
        aovBarriers[5].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // Sub-plan 3.C: specular radiance AOV barrier
        aovBarriers[6].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[6].srcAccessMask = 0;
        aovBarriers[6].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[6].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[6].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[6].image = m_specularRadianceImage;
        aovBarriers[6].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // Sub-plan 3.C.6: diffuse albedo AOV barrier
        aovBarriers[7].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[7].srcAccessMask = 0;
        aovBarriers[7].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[7].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[7].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[7].image = m_diffAlbedoImage;
        aovBarriers[7].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // Sub-plan 3.C.6: specular color AOV barrier
        aovBarriers[8].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[8].srcAccessMask = 0;
        aovBarriers[8].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[8].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[8].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[8].image = m_specColorImage;
        aovBarriers[8].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // Sub-plan 4.B: normal+roughness packed AOV barrier
        aovBarriers[9].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[9].srcAccessMask = 0;
        aovBarriers[9].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[9].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[9].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[9].image = m_normalRoughnessImage;
        aovBarriers[9].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // Sub-plan 4.C: denoised diffuse output barrier
        aovBarriers[10].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[10].srcAccessMask = 0;
        aovBarriers[10].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[10].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[10].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[10].image = m_outDiffRadianceImage;
        aovBarriers[10].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // Sub-plan 4.C: denoised specular output barrier
        aovBarriers[11].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[11].srcAccessMask = 0;
        aovBarriers[11].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[11].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[11].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[11].image = m_outSpecRadianceImage;
        aovBarriers[11].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 12, aovBarriers);
    }

    // --- The emission guide to GENERAL: rewritten whole each frame (or blended into the
    //     last frames' under offline NRD, whose values the previous frame left). ---
    {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = m_emissionInitialized ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT : 0;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        b.oldLayout = m_emissionInitialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = m_emissionAOVImage;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &b);
        m_emissionInitialized = true;
    }

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
    pc.params = glm::uvec4(m_width, m_height, m_sampleIndex, m_maxBounces);
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
    // The frames already in the running mean (0: this frame's sample alone, which a temporal
    // denoiser takes), and whether last frame's view gives motion vectors.
    pc.control.y = tracerAccumulates(m_renderSettings) ? m_accumFrames : 0u;
    pc.control.z = m_hasPreviousView ? 1u : 0u;
    pc.control.w = m_envCDFWidth;
    pc.tuning = glm::vec4(m_renderSettings.fireflyClampLuminance, float(m_envCDFHeight), m_envCDFIntegral, 0.0f);
    pc.jitter = glm::vec4(m_jitterCurrent.x, m_jitterCurrent.y, 0.0f, 0.0f);

    // Store current viewProj for next frame's reprojection
    m_prevViewProj = proj * view;

    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                       0, sizeof(PTPushConstants), &pc);

    // --- The radiance cache: this frame's update (1 pixel in 5x5) and resolve, before the trace
    //     that ends its paths in it. ---
    if (cacheOn) {
        m_radianceCache.recordBeginFrame(cmd);
        vkCmdTraceRaysKHR(cmd, &m_cacheRgenRegion, &m_missRegion, &m_hitRegion, &m_callRegion,
                          (m_width + 4u) / 5u, (m_height + 4u) / 5u, 1);
        m_radianceCache.recordResolve(cmd);
        // Push constants are shared by the bind points: the resolve's replaced the trace's.
        vkCmdPushConstants(cmd, m_pipelineLayout,
                           VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                           0, sizeof(PTPushConstants), &pc);
    }

    // --- Trace rays! ---
    vkCmdTraceRaysKHR(cmd, &m_rgenRegion, &m_missRegion, &m_hitRegion, &m_callRegion,
                      m_width, m_height, 1);

    // --- À-trous beauty denoise (DenoiseMode::Atrous) ---
    // Runs edge-aware spatial filtering on the final beauty (binding 2) in
    // place, using the normal AOV (binding 7, N*0.5+0.5) and linear depth AOV
    // (binding 20) for edge-stopping. Because it denoises the correct final
    // PBR image — not demodulated diffuse/specular like NRD — metals/emissive
    // stay physically correct (no black metals, no magenta cast). Independent
    // of NRD, so this lives outside the OHAO_NRD_ENABLED block.
    bool atrousRan = false;
    bool dlssRan   = false;  // Phase 5: set when DLSS-RR tonemap wrote m_outputImage via COMPUTE
    bool nrdRan    = false;  // nrd_compose wrote m_outputImage via COMPUTE
    if (m_renderSettings.denoiseMode == DenoiseMode::Atrous && m_atrousDenoiser) {
        // Publish raygen stores (beauty/normal/depth, RAY_TRACING) to the
        // COMPUTE reads of the first à-trous pass. All three stay in GENERAL.
        VkMemoryBarrier rayToCompute{};
        rayToCompute.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        rayToCompute.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        rayToCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &rayToCompute, 0, nullptr, 0, nullptr);

        AtrousInputs ai{};
        ai.beautyImage   = m_outputImage;
        ai.beautyView    = m_outputView;
        ai.normalView    = m_normalAOVView;
        ai.depthView     = m_depthAOVView;
        ai.motionView    = m_motionVectorView;
        // Reset SVGF history ONLY on the genuine first frame (or explicit
        // resetAccumulation). Do NOT reset on camera move — SVGF is meant to
        // REPROJECT through motion via motion vectors + per-pixel disocclusion
        // rejection. Resetting every moved frame threw away all accumulated
        // samples → raw 1-spp noise while moving (the bug the user reported).
        ai.resetHistory  = (m_historyFrameCount == 0u);
        m_atrousDenoiser->dispatch(cmd, ai);
        atrousRan = true;  // beauty last written by COMPUTE, still GENERAL
    }

#ifdef OHAO_DLSS_ENABLED
    // --- Phase 5: DLSS Ray Reconstruction denoise + tonemap ---
    // The raygen produced a FRESH 1-spp HDR frame (accum, control.y=0) plus the
    // packed normal/roughness guide (normalAOV) this frame. Wrap the guide buffers
    // as NGX resources, run NGX_VULKAN_EVALUATE_DLSSD_EXT, then tonemap the HDR
    // COLOR_OUT into the RGBA8 beauty (m_outputImage) the standard readback uses.
    if (m_renderSettings.denoiseMode == DenoiseMode::DLSSRR && m_dlssRR &&
        m_dlssRR->isFeatureCreated()) {
        // 1) Publish raygen guide/accum writes (RAY_TRACING) to DLSS's compute
        //    reads. All these images remain in GENERAL (DLSS handles its own
        //    internal transitions; we only make the writes visible).
        VkMemoryBarrier rayToDlss{};
        rayToDlss.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        rayToDlss.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        rayToDlss.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &rayToDlss, 0, nullptr, 0, nullptr);

        // 2) DLSS COLOR_OUT: UNDEFINED→GENERAL on first dispatch, else keep GENERAL.
        {
            VkImageMemoryBarrier b{};
            b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask    = m_dlssColorOutFirstFrame ? 0 : VK_ACCESS_SHADER_READ_BIT;
            b.dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
            b.oldLayout        = m_dlssColorOutFirstFrame ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
            b.image            = m_dlssColorOutImage;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd,
                m_dlssColorOutFirstFrame ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &b);
            m_dlssColorOutFirstFrame = false;
        }

        // 3) Fill eval inputs. glm column-major view/proj passed as-is (DLSS wants
        //    row-major + left-multiply; the two transposes cancel — see wrapper).
        const glm::mat4 viewM = glm::inverse(pc.invView);
        const glm::mat4 projM = glm::inverse(pc.invProj);

        DlssRR::EvalInputs ei{};
        ei.colorInImage     = m_accumBuffer;       ei.colorInView     = m_accumView;        ei.colorInFormat     = VK_FORMAT_R32G32B32A32_SFLOAT;
        ei.colorOutImage    = m_dlssColorOutImage; ei.colorOutView    = m_dlssColorOutView; ei.colorOutFormat    = VK_FORMAT_R16G16B16A16_SFLOAT;
        ei.diffAlbedoImage  = m_diffAlbedoImage;   ei.diffAlbedoView  = m_diffAlbedoView;   ei.diffAlbedoFormat  = VK_FORMAT_R8G8B8A8_UNORM;
        ei.specAlbedoImage  = m_specColorImage;    ei.specAlbedoView  = m_specColorView;    ei.specAlbedoFormat  = VK_FORMAT_R8G8B8A8_UNORM;
        ei.normalRoughImage = m_normalAOV;         ei.normalRoughView = m_normalAOVView;    ei.normalRoughFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
        ei.depthImage       = m_depthAOVImage;     ei.depthView       = m_depthAOVView;     ei.depthFormat       = VK_FORMAT_R32_SFLOAT;
        ei.motionImage      = m_motionVectorImage; ei.motionView      = m_motionVectorView; ei.motionFormat      = VK_FORMAT_R16G16_SFLOAT;
        ei.renderW = m_width; ei.renderH = m_height;
        ei.jitterX = m_jitterCurrent.x; ei.jitterY = m_jitterCurrent.y;
        ei.mvScaleX = -1.0f; ei.mvScaleY = -1.0f;   // OHAO writes currPix-prevPix; DLSS wants prevPix-currPix
        ei.worldToView = glm::value_ptr(viewM);
        ei.viewToClip  = glm::value_ptr(projM);
        // Reset only on the genuine first frame / accumulation reset (NOT camera
        // move — DLSS reprojects through motion via the motion vectors).
        ei.reset = (m_historyFrameCount == 0u);

        if (m_dlssRR->evaluate(cmd, ei)) {
            // 4) Publish DLSS COLOR_OUT (COMPUTE write) → tonemap read, and make
            //    m_outputImage writable (WAW vs the raygen's beauty write).
            VkImageMemoryBarrier post[2] = {};
            post[0].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            post[0].srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
            post[0].dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
            post[0].oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
            post[0].newLayout        = VK_IMAGE_LAYOUT_GENERAL;
            post[0].image            = m_dlssColorOutImage;
            post[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            post[1].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            post[1].srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
            post[1].dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
            post[1].oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
            post[1].newLayout        = VK_IMAGE_LAYOUT_GENERAL;
            post[1].image            = m_outputImage;
            post[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 2, post);

            // 5) Tonemap DLSS HDR COLOR_OUT → RGBA8 beauty (ACES+gamma, matches None).
            m_dlssRR->tonemap(cmd, frameSlot, m_dlssColorOutView, m_outputView, m_width, m_height);
            dlssRan = true;  // beauty last written by COMPUTE, still GENERAL
        }
    }
#endif  // OHAO_DLSS_ENABLED

#ifdef OHAO_NRD_ENABLED
    // --- Sub-plan 4.C T3b: NRD REBLUR_DIFFUSE_SPECULAR dispatch ---
    // After trace rays, all NRD input AOVs are in VK_IMAGE_LAYOUT_GENERAL with
    // SHADER_WRITE access. NRD's internal _Dispatch will insert its own
    // transitions (GENERAL -> SHADER_READ for inputs, stay GENERAL for outputs)
    // before the compute dispatch, so we only need a memory barrier to publish
    // raygen writes before compute reads. We also need `restoreInitialState=true`
    // in the snapshot so after denoise, inputs come back to GENERAL for
    // downstream readback.
    // Only in NRD mode: NRD, then nrd_compose, which writes the frame's output (and its HDR).
    if (m_nrdDenoiser && m_renderSettings.enableAuxiliaryAOVs && m_renderSettings.denoiseMode == DenoiseMode::NRD) {
        if (m_nrdCompositor) m_nrdCompositor->setFrameSlot(frameSlot);
        // Publish raygen stores before NRD's compute pipelines read them.
        VkMemoryBarrier rayToCompute {};
        rayToCompute.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        rayToCompute.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        rayToCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &rayToCompute, 0, nullptr, 0, nullptr);

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

        NrdDenoiser::NrdInputResources res {};
        res.motionVector           = {m_motionVectorImage,     VK_FORMAT_R16G16_SFLOAT};
        res.viewZ                  = {m_depthAOVImage,         VK_FORMAT_R32_SFLOAT};
        res.normalRoughness        = {m_normalRoughnessImage,  VK_FORMAT_A2B10G10R10_UNORM_PACK32};
        res.diffRadianceHitDist    = {m_diffuseRadianceImage,  VK_FORMAT_R32G32B32A32_SFLOAT};
        res.specRadianceHitDist    = {m_specularRadianceImage, VK_FORMAT_R32G32B32A32_SFLOAT};
        res.outDiffRadianceHitDist = {m_outDiffRadianceImage,  VK_FORMAT_R32G32B32A32_SFLOAT};
        res.outSpecRadianceHitDist = {m_outSpecRadianceImage,  VK_FORMAT_R32G32B32A32_SFLOAT};
        m_nrdDenoiser->setInputResources(res);

        m_nrdDenoiser->denoise(cmd);

        if (m_nrdCompositor) {
            // Transition inputs for compose: bindings 24, 25, 27, 28.
            // Writer stages differ per slot: 24/25 were written by raygen
            // (RAY_TRACING_SHADER); 27/28 were written by NRD's internal
            // compute dispatches. The upstream NRD-input barrier already
            // chained RAY_TRACING → COMPUTE for the raygen-produced slots
            // (visibility is in COMPUTE stage by the time we get here), so
            // this barrier serves:
            //   - 27/28: a COMPUTE→COMPUTE WAR/RAW acquire
            //   - 24/25: a no-op acquire (memory already visible)
            // NRD's restoreInitialState=true left 22/23/26/27/28 in GENERAL.
            VkImageMemoryBarrier cbIn[5] = {};
            VkImage cbInImages[5] = {
                m_diffAlbedoImage,       // binding 24
                m_specColorImage,        // binding 25
                m_outDiffRadianceImage,  // binding 27
                m_outSpecRadianceImage,  // binding 28
                m_emissionAOVImage,      // binding 13
            };
            for (int i = 0; i < 5; ++i) {
                cbIn[i].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                cbIn[i].srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
                cbIn[i].dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
                cbIn[i].oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
                cbIn[i].newLayout        = VK_IMAGE_LAYOUT_GENERAL;
                cbIn[i].image            = cbInImages[i];
                cbIn[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            }
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 5, cbIn);

            // Transition binding 29: UNDEFINED→GENERAL on first dispatch, GENERAL→GENERAL after.
            // Using m_nrdComposeFirstFrame (per-instance member, NOT a function-local static) so
            // offline + realtime PT profiles each get their own first-frame counter.
            VkImageMemoryBarrier cbOut{};
            cbOut.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            cbOut.srcAccessMask    = m_nrdComposeFirstFrame ? 0 : VK_ACCESS_SHADER_WRITE_BIT;
            cbOut.dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
            cbOut.oldLayout        = m_nrdComposeFirstFrame ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
            cbOut.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
            cbOut.image            = m_nrdComposedImage;
            cbOut.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd,
                m_nrdComposeFirstFrame ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &cbOut);
            m_nrdComposeFirstFrame = false;

            // Dispatch
            NrdComposeInputs ci {};
            ci.diffRadiance = m_outDiffRadianceView;
            ci.specRadiance = m_outSpecRadianceView;
            ci.diffAlbedo   = m_diffAlbedoView;
            ci.specColor    = m_specColorView;
            ci.emission     = m_emissionAOVView;
            ci.composedOut  = m_nrdComposedView;
            ci.outputLdr    = m_outputView;   // the frame: a window presents it, a capture reads it
            m_nrdCompositor->dispatch(cmd, ci);
            nrdRan = true;
        }

    }
#endif  // OHAO_NRD_ENABLED

    // --- Transition output image to TRANSFER_SRC_OPTIMAL for readback/blit ---
    {
        // In à-trous mode the final beauty write comes from the COMPUTE stage,
        // so the transfer must wait on COMPUTE (not just RAY_TRACING). Adding
        // COMPUTE to the wait is harmless for other modes (nothing wrote the
        // output via compute there).
        const VkPipelineStageFlags outSrcStage =
            (atrousRan || dlssRan || nrdRan) ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                                 : VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer.image = m_outputImage;
        toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            outSrcStage, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    }

    // --- Transition AOV images to TRANSFER_SRC_OPTIMAL for denoiser readback ---
    if (m_renderSettings.enableAuxiliaryAOVs) {
        VkImageMemoryBarrier aovBarriers[2] = {};

        aovBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        aovBarriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        aovBarriers[0].image = m_albedoAOV;
        aovBarriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        aovBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        aovBarriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        aovBarriers[1].image = m_normalAOV;
        aovBarriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 2, aovBarriers);
    }

    // Advance sample sequence and track accumulation history separately.
    m_sampleIndex++;
    m_historyFrameCount++;
    m_accumFrames++;
    m_viewChangedThisFrame = false;
    m_hasPreviousView = true;
    m_accumInitialized[accumWrite] = true;   // m_accumBuffer stays the image just written
    m_accumWriteIndex = accumPrev;
}

} // namespace ohao
