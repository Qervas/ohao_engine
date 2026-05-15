// Extracted from path_tracer.cpp — per-frame render() method.
// Kept as a member of class PathTracer; no behavior change.
// Push-constant flag constants (kPTFlag*) moved here since this TU is their sole consumer.

#include "path_tracer.hpp"

#include <cstring>
#include <iostream>
#include <vector>
#include <glm/gtc/type_ptr.hpp>

// nrd_denoise.hpp / nrd_compose.hpp are safe to include unconditionally; their
// dispatch methods are OHAO_NRD_ENABLED-guarded inside this TU.
#include "render/rt/denoise/nrd_denoise.hpp"
#include "render/rt/denoise/nrd_compose.hpp"
#include "render/rt/denoise/nrd_cinematic.hpp"

namespace ohao {

namespace {
constexpr uint32_t kPTFlagEnableAOVs = 1u << 0;
constexpr uint32_t kPTFlagEnableInternalDenoise = 1u << 1;
constexpr uint32_t kPTFlagEnableFireflyClamp = 1u << 2;
// Sub-plan 4.F T3: in NRD mode, raygen uses a running mean over N samples for the
// five NRD AOV bindings (22/23/24/25/26) so offline --spp=N feeds NRD an N-spp
// averaged input. Must stay in sync with PT_FLAG_ACCUMULATE_AOVS in the raygen shaders.
constexpr uint32_t kPTFlagAccumulateAOVs = 1u << 3;
}  // namespace

void PathTracer::render(VkCommandBuffer cmd, RTAccelerationStructure* accel,
                         const glm::mat4& view, const glm::mat4& proj,
                         const glm::vec3& lightPos, float lightIntensity,
                         const glm::vec3& lightColor, float lightRadius) {
    if (!m_rtPipeline || !accel || !accel->getTLAS()) return;

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
    if (m_renderSettings.denoiseMode == DenoiseMode::NRD) {
        const uint32_t idx = (m_haltonIndex % 16u) + 1u;
        m_jitterCurrent = glm::vec2(halton(idx, 2) - 0.5f, halton(idx, 3) - 0.5f);
        m_haltonIndex++;
    } else {
        // Non-NRD paths (None/OIDN/OptiX) keep pixel-center sampling — zero jitter
        // here preserves bit-exact parity with pre-T4 behavior.
        m_jitterCurrent = glm::vec2(0.0f);
    }

    // --- Update descriptor set with current frame's data ---

    // Binding 0: TLAS
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    VkAccelerationStructureKHR tlas = accel->getTLAS();
    asWrite.pAccelerationStructures = &tlas;

    // Binding 1: Accumulation buffer (storage image, RGBA32F)
    VkDescriptorImageInfo accumInfo{};
    accumInfo.imageView = m_accumView;
    accumInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Binding 2: Output image (storage image, RGBA8)
    VkDescriptorImageInfo outputInfo{};
    outputInfo.imageView = m_outputView;
    outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Binding 3: Material buffer SSBO
    VkDescriptorBufferInfo materialInfo{};
    materialInfo.buffer = m_materialBuffer;
    materialInfo.offset = 0;
    materialInfo.range = m_materialData.size() * sizeof(glm::vec4);

    // Binding 4: Normal buffer SSBO
    VkDescriptorBufferInfo normalBufInfo{};
    normalBufInfo.buffer = m_normalBuffer != VK_NULL_HANDLE ? m_normalBuffer : m_materialBuffer;
    normalBufInfo.offset = 0;
    normalBufInfo.range = VK_WHOLE_SIZE;

    // Binding 5: Index buffer SSBO
    VkDescriptorBufferInfo indexBufInfo{};
    indexBufInfo.buffer = m_indexBuffer != VK_NULL_HANDLE ? m_indexBuffer : m_materialBuffer;
    indexBufInfo.offset = 0;
    indexBufInfo.range = VK_WHOLE_SIZE;

    // Binding 6: Albedo AOV (storage image, RGBA32F)
    VkDescriptorImageInfo albedoAOVInfo{};
    albedoAOVInfo.imageView = m_albedoAOVView;
    albedoAOVInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Binding 7: Normal AOV (storage image, RGBA32F)
    VkDescriptorImageInfo normalAOVInfo{};
    normalAOVInfo.imageView = m_normalAOVView;
    normalAOVInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    const uint32_t prevSurfaceHistoryIndex = 1u - m_surfaceHistoryWriteIndex;
    VkDescriptorImageInfo prevSurfaceHistoryInfo{};
    prevSurfaceHistoryInfo.imageView = m_surfaceHistoryViews[prevSurfaceHistoryIndex];
    prevSurfaceHistoryInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo currSurfaceHistoryInfo{};
    currSurfaceHistoryInfo.imageView = m_surfaceHistoryViews[m_surfaceHistoryWriteIndex];
    currSurfaceHistoryInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    const uint32_t prevShadingHistoryIndex = 1u - m_shadingHistoryWriteIndex;
    VkDescriptorImageInfo prevShadingHistoryInfo{};
    prevShadingHistoryInfo.imageView = m_shadingHistoryViews[prevShadingHistoryIndex];
    prevShadingHistoryInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo currShadingHistoryInfo{};
    currShadingHistoryInfo.imageView = m_shadingHistoryViews[m_shadingHistoryWriteIndex];
    currShadingHistoryInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[29] = {};

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[0].pNext = &asWrite;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &accumInfo;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_descriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &outputInfo;

    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = m_descriptorSet;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &materialInfo;

    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = m_descriptorSet;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].pBufferInfo = &normalBufInfo;

    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = m_descriptorSet;
    writes[5].dstBinding = 5;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[5].pBufferInfo = &indexBufInfo;

    writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = m_descriptorSet;
    writes[6].dstBinding = 6;
    writes[6].descriptorCount = 1;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[6].pImageInfo = &albedoAOVInfo;

    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = m_descriptorSet;
    writes[7].dstBinding = 7;
    writes[7].descriptorCount = 1;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[7].pImageInfo = &normalAOVInfo;

    // Binding 8: UV buffer
    VkDescriptorBufferInfo uvBufInfo{};
    uvBufInfo.buffer = m_uvBuffer != VK_NULL_HANDLE ? m_uvBuffer : m_materialBuffer;
    uvBufInfo.offset = 0;
    uvBufInfo.range = VK_WHOLE_SIZE;

    writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[8].dstSet = m_descriptorSet;
    writes[8].dstBinding = 8;
    writes[8].descriptorCount = 1;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[8].pBufferInfo = &uvBufInfo;

    // Binding 9: Material ID buffer
    VkDescriptorBufferInfo matIDBufInfo{};
    matIDBufInfo.buffer = m_matIDBuffer != VK_NULL_HANDLE ? m_matIDBuffer : m_materialBuffer;
    matIDBufInfo.offset = 0;
    matIDBufInfo.range = VK_WHOLE_SIZE;

    writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[9].dstSet = m_descriptorSet;
    writes[9].dstBinding = 9;
    writes[9].descriptorCount = 1;
    writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[9].pBufferInfo = &matIDBufInfo;

    // Binding 10: Material color buffer
    VkDescriptorBufferInfo matColorBufInfo{};
    matColorBufInfo.buffer = m_matColorBuffer != VK_NULL_HANDLE ? m_matColorBuffer : m_materialBuffer;
    matColorBufInfo.offset = 0;
    matColorBufInfo.range = VK_WHOLE_SIZE;

    writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[10].dstSet = m_descriptorSet;
    writes[10].dstBinding = 10;
    writes[10].descriptorCount = 1;
    writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[10].pBufferInfo = &matColorBufInfo;

    // Binding 11: Light buffer (SSBO)
    uint32_t writeCount = 11;
    VkDescriptorBufferInfo lightBufInfo{};
    if (m_lightBuffer != VK_NULL_HANDLE) {
        lightBufInfo.buffer = m_lightBuffer;
        lightBufInfo.offset = 0;
        lightBufInfo.range = VK_WHOLE_SIZE;
        writes[11].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[11].dstSet = m_descriptorSet;
        writes[11].dstBinding = 11;
        writes[11].descriptorCount = 1;
        writes[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[11].pBufferInfo = &lightBufInfo;
        writeCount = 12;
    }

    // Binding 12: Bindless textures (MUST be last — variable count)
    std::vector<VkDescriptorImageInfo> texInfos;
    if (!m_bindlessImageViews.empty()) {
        texInfos.resize(m_bindlessTextureCount);
        for (uint32_t i = 0; i < m_bindlessTextureCount; i++) {
            texInfos[i].imageView = m_bindlessImageViews[i];
            texInfos[i].sampler = (i < m_bindlessSamplers.size()) ? m_bindlessSamplers[i] : m_defaultSampler;
            texInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[writeCount].dstSet = m_descriptorSet;
        writes[writeCount].dstBinding = 12;
        writes[writeCount].descriptorCount = m_bindlessTextureCount;
        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[writeCount].pImageInfo = texInfos.data();
        writeCount++;
    }

    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = m_descriptorSet;
    writes[writeCount].dstBinding = 13;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo = &prevSurfaceHistoryInfo;
    writeCount++;

    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = m_descriptorSet;
    writes[writeCount].dstBinding = 14;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo = &currSurfaceHistoryInfo;
    writeCount++;

    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = m_descriptorSet;
    writes[writeCount].dstBinding = 15;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo = &prevShadingHistoryInfo;
    writeCount++;

    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = m_descriptorSet;
    writes[writeCount].dstBinding = 16;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo = &currShadingHistoryInfo;
    writeCount++;

    // Binding 17: env marginal CDF (only if set — dummy buffers from light_upload guarantee this)
    VkDescriptorBufferInfo envMargInfo{};
    if (m_envMarginalCDFBuffer != VK_NULL_HANDLE) {
        envMargInfo.buffer = m_envMarginalCDFBuffer;
        envMargInfo.offset = 0;
        envMargInfo.range = VK_WHOLE_SIZE;
        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[writeCount].dstSet = m_descriptorSet;
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
        writes[writeCount].dstSet = m_descriptorSet;
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
    writes[writeCount].dstSet = m_descriptorSet;
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
    writes[writeCount].dstSet          = m_descriptorSet;
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
    writes[writeCount].dstSet          = m_descriptorSet;
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
    writes[writeCount].dstSet          = m_descriptorSet;
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
    writes[writeCount].dstSet          = m_descriptorSet;
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
    writes[writeCount].dstSet          = m_descriptorSet;
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
    writes[writeCount].dstSet          = m_descriptorSet;
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
    writes[writeCount].dstSet          = m_descriptorSet;
    writes[writeCount].dstBinding      = 26;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &normalRoughnessInfo;
    writeCount++;

    // Binding 27: NRD denoised diffuse — Sub-plan 4.C
    VkDescriptorImageInfo outDiffInfo{};
    outDiffInfo.imageView   = m_outDiffRadianceView;
    outDiffInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = m_descriptorSet;
    writes[writeCount].dstBinding      = 27;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &outDiffInfo;
    writeCount++;

    // Binding 28: NRD denoised specular — Sub-plan 4.C
    VkDescriptorImageInfo outSpecInfo{};
    outSpecInfo.imageView   = m_outSpecRadianceView;
    outSpecInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = m_descriptorSet;
    writes[writeCount].dstBinding      = 28;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &outSpecInfo;
    writeCount++;

    vkUpdateDescriptorSets(m_device, writeCount, writes, 0, nullptr);

    // --- Transition accumulation buffer to GENERAL ---
    // On first accumulated frame, transition from UNDEFINED to clear it;
    // on subsequent frames, keep GENERAL (already there from last trace).
    {
        VkImageMemoryBarrier accumBarrier{};
        accumBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        accumBarrier.srcAccessMask = (m_historyFrameCount == 0) ? 0 : VK_ACCESS_SHADER_WRITE_BIT;
        accumBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        accumBarrier.oldLayout = (m_historyFrameCount == 0) ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        accumBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        accumBarrier.image = m_accumBuffer;
        accumBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            (m_historyFrameCount == 0) ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 1, &accumBarrier);
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
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
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
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 12, aovBarriers);
    }

    // --- Transition surface history images to GENERAL ---
    {
        const uint32_t prevSurfaceHistoryIndex = 1u - m_surfaceHistoryWriteIndex;
        VkImageMemoryBarrier historyBarriers[2] = {};

        historyBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        historyBarriers[0].srcAccessMask = m_surfaceHistoryInitialized[prevSurfaceHistoryIndex] ? VK_ACCESS_SHADER_WRITE_BIT : 0;
        historyBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        historyBarriers[0].oldLayout = m_surfaceHistoryInitialized[prevSurfaceHistoryIndex] ? VK_IMAGE_LAYOUT_GENERAL
                                                                                            : VK_IMAGE_LAYOUT_UNDEFINED;
        historyBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        historyBarriers[0].image = m_surfaceHistoryImages[prevSurfaceHistoryIndex];
        historyBarriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        historyBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        historyBarriers[1].srcAccessMask = m_surfaceHistoryInitialized[m_surfaceHistoryWriteIndex] ? VK_ACCESS_SHADER_WRITE_BIT : 0;
        historyBarriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        historyBarriers[1].oldLayout = m_surfaceHistoryInitialized[m_surfaceHistoryWriteIndex] ? VK_IMAGE_LAYOUT_GENERAL
                                                                                                : VK_IMAGE_LAYOUT_UNDEFINED;
        historyBarriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        historyBarriers[1].image = m_surfaceHistoryImages[m_surfaceHistoryWriteIndex];
        historyBarriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 2, historyBarriers);
    }

    {
        const uint32_t prevShadingHistoryIndex = 1u - m_shadingHistoryWriteIndex;
        VkImageMemoryBarrier historyBarriers[2] = {};

        historyBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        historyBarriers[0].srcAccessMask = m_shadingHistoryInitialized[prevShadingHistoryIndex] ? VK_ACCESS_SHADER_WRITE_BIT : 0;
        historyBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        historyBarriers[0].oldLayout = m_shadingHistoryInitialized[prevShadingHistoryIndex] ? VK_IMAGE_LAYOUT_GENERAL
                                                                                            : VK_IMAGE_LAYOUT_UNDEFINED;
        historyBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        historyBarriers[0].image = m_shadingHistoryImages[prevShadingHistoryIndex];
        historyBarriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        historyBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        historyBarriers[1].srcAccessMask = m_shadingHistoryInitialized[m_shadingHistoryWriteIndex] ? VK_ACCESS_SHADER_WRITE_BIT : 0;
        historyBarriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        historyBarriers[1].oldLayout = m_shadingHistoryInitialized[m_shadingHistoryWriteIndex] ? VK_IMAGE_LAYOUT_GENERAL
                                                                                                : VK_IMAGE_LAYOUT_UNDEFINED;
        historyBarriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        historyBarriers[1].image = m_shadingHistoryImages[m_shadingHistoryWriteIndex];
        historyBarriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 2, historyBarriers);
    }

    // --- Bind pipeline and descriptors ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_rtPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    // --- Push constants ---
    PTPushConstants pc{};
    pc.invView = glm::inverse(view);
    pc.invProj = glm::inverse(proj);
    pc.prevViewProj = m_prevViewProj;
    pc.params = glm::uvec4(m_width, m_height, m_sampleIndex, m_maxBounces);
    pc.control = glm::uvec4(0u);
    if (m_renderSettings.enableAuxiliaryAOVs) pc.control.x |= kPTFlagEnableAOVs;
    if (m_renderSettings.enableInternalDenoise) pc.control.x |= kPTFlagEnableInternalDenoise;
    if (m_renderSettings.enableFireflyClamp) pc.control.x |= kPTFlagEnableFireflyClamp;
    // Sub-plan 4.F T3: NRD wants the mean of N samples on the 5 AOV bindings, not the
    // final sample's 1spp. Raygen handles sample-0 (overwrite) vs N>0 (running mean).
    if (m_renderSettings.denoiseMode == DenoiseMode::NRD) pc.control.x |= kPTFlagAccumulateAOVs;
    pc.control.y = m_historyFrameCount;
    pc.control.z = m_viewChangedThisFrame ? 1u : 0u;
    pc.control.w = m_envCDFWidth;
    pc.tuning = glm::vec4(m_renderSettings.fireflyClampLuminance, float(m_envCDFHeight), m_envCDFIntegral, 0.0f);
    // Sub-plan 4.F T4: propagate pixel jitter to raygen. zw are reserved padding
    // (kept 0 so the push-constant tail is deterministic across frames).
    pc.jitter = glm::vec4(m_jitterCurrent.x, m_jitterCurrent.y, 0.0f, 0.0f);

    // Store current viewProj for next frame's reprojection
    m_prevViewProj = proj * view;

    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                       0, sizeof(PTPushConstants), &pc);

    // --- Trace rays! ---
    vkCmdTraceRaysKHR(cmd, &m_rgenRegion, &m_missRegion, &m_hitRegion, &m_callRegion,
                      m_width, m_height, 1);

#ifdef OHAO_NRD_ENABLED
    // --- Sub-plan 4.C T3b: NRD REBLUR_DIFFUSE_SPECULAR dispatch ---
    // After trace rays, all NRD input AOVs are in VK_IMAGE_LAYOUT_GENERAL with
    // SHADER_WRITE access. NRD's internal _Dispatch will insert its own
    // transitions (GENERAL -> SHADER_READ for inputs, stay GENERAL for outputs)
    // before the compute dispatch, so we only need a memory barrier to publish
    // raygen writes before compute reads. We also need `restoreInitialState=true`
    // in the snapshot so after denoise, inputs come back to GENERAL for
    // downstream readback.
    if (m_nrdDenoiser && m_renderSettings.enableAuxiliaryAOVs) {
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
        camera.frameIndex = m_viewChangedThisFrame ? 0u : m_historyFrameCount;  // 4.F T2: bootstrap on view change
        camera.isMotionVectorInWorldSpace = false;

        // 4.F T2: when view changed this frame, force NRD to treat it as a
        // fresh frame (no history). Override prev V/P with CURRENT values so
        // NRD doesn't reproject from a stale camera pose → prevents ghosting
        // trail while the user moves the camera. Paired with frameIndex=0
        // above, NRD falls back to spatial-only denoise for this frame.
        if (m_viewChangedThisFrame) {
            std::memcpy(camera.viewMatrixPrev.data(), glm::value_ptr(viewM), sizeof(float) * 16);
            std::memcpy(camera.projMatrixPrev.data(), glm::value_ptr(projM), sizeof(float) * 16);
        }
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
            VkImageMemoryBarrier cbIn[4] = {};
            VkImage cbInImages[4] = {
                m_diffAlbedoImage,       // binding 24
                m_specColorImage,        // binding 25
                m_outDiffRadianceImage,  // binding 27
                m_outSpecRadianceImage,  // binding 28
            };
            for (int i = 0; i < 4; ++i) {
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
                0, 0, nullptr, 0, nullptr, 4, cbIn);

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
            ci.composedOut  = m_nrdComposedView;
            m_nrdCompositor->dispatch(cmd, ci);

            // After compose, binding 29 is in GENERAL with SHADER_WRITE access.
            // No further in-frame consumer in 4.D scope — readback transitions
            // GENERAL→TRANSFER_SRC itself when env_demo dumps it.
        }

        if (m_cinematicPost) {
            // Sub-plan 4.G: cinematic chain replaces 4.F single-pass tonemap.
            //   1. bloom extract: HDR (binding 29) → bloom mip 0 (half-res)
            //   2. bloom blur ×2: mip 0 → mip 1; mip 1 → mip 2 (downsample+blur)
            //   3. composite: HDR + 3 sampled bloom mips + depth + env →
            //                 binding 30 (RGBA8 final LDR)
            //
            // The composite shader reads HDR from binding 29 + depth AOV from
            // 20 — both are storage-image reads in GENERAL layout. Bloom mips
            // need a layout flip between extract/blur (GENERAL) and composite
            // (SHADER_READ_ONLY_OPTIMAL).

            // --- Publish 29 (HDR) and 20 (depth) writes for the extract +
            //     composite reads. COMPUTE→COMPUTE for 29; RT→COMPUTE for 20
            //     was already barriered earlier (rayToCompute), but include
            //     defensively. ---
            VkImageMemoryBarrier tbIn[2] = {};
            tbIn[0].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            tbIn[0].srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
            tbIn[0].dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
            tbIn[0].oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
            tbIn[0].newLayout        = VK_IMAGE_LAYOUT_GENERAL;
            tbIn[0].image            = m_nrdComposedImage;
            tbIn[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            tbIn[1].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            tbIn[1].srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
            tbIn[1].dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
            tbIn[1].oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
            tbIn[1].newLayout        = VK_IMAGE_LAYOUT_GENERAL;
            tbIn[1].image            = m_depthAOVImage;
            tbIn[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 2, tbIn);

            // --- Transition bloom mip 0 UNDEFINED→GENERAL (first frame) or
            //     SHADER_READ_ONLY→GENERAL (subsequent frames). ---
            VkImageMemoryBarrier mip0Pre{};
            mip0Pre.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            mip0Pre.srcAccessMask    = m_bloomFirstFrame[0] ? 0 : VK_ACCESS_SHADER_READ_BIT;
            mip0Pre.dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
            mip0Pre.oldLayout        = m_bloomFirstFrame[0] ? VK_IMAGE_LAYOUT_UNDEFINED
                                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            mip0Pre.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
            mip0Pre.image            = m_bloomMipImages[0];
            mip0Pre.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd,
                m_bloomFirstFrame[0] ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                     : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &mip0Pre);
            m_bloomFirstFrame[0] = false;

            // 1) Extract.
            m_cinematicPost->dispatchBloomExtract(
                cmd, m_nrdComposedView, m_bloomMipViews[0],
                m_width, m_height, m_bloomMipWidth[0], m_bloomMipHeight[0]);

            // Loop: blur (downsample) from mip N → mip N+1.
            for (uint32_t mip = 1; mip < 3; ++mip) {
                // Producer (mip N-1) write → reader; transition mip N for write.
                VkImageMemoryBarrier b[2] = {};
                b[0].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b[0].srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
                b[0].dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
                b[0].oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
                b[0].newLayout        = VK_IMAGE_LAYOUT_GENERAL;
                b[0].image            = m_bloomMipImages[mip - 1];
                b[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                b[1].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b[1].srcAccessMask    = m_bloomFirstFrame[mip] ? 0 : VK_ACCESS_SHADER_READ_BIT;
                b[1].dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
                b[1].oldLayout        = m_bloomFirstFrame[mip] ? VK_IMAGE_LAYOUT_UNDEFINED
                                                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                b[1].newLayout        = VK_IMAGE_LAYOUT_GENERAL;
                b[1].image            = m_bloomMipImages[mip];
                b[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdPipelineBarrier(cmd,
                    (m_bloomFirstFrame[mip] ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                            : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                      | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr, 2, b);
                m_bloomFirstFrame[mip] = false;

                // 2,3) Blur (downsample) mip-1 → mip.
                m_cinematicPost->dispatchBloomBlur(
                    cmd, mip - 1, /* slot 0=mip0→mip1, slot 1=mip1→mip2 */
                    m_bloomMipViews[mip - 1], m_bloomMipViews[mip],
                    m_bloomMipWidth[mip - 1], m_bloomMipHeight[mip - 1],
                    m_bloomMipWidth[mip], m_bloomMipHeight[mip]);
            }

            // --- Flip all 3 bloom mips GENERAL→SHADER_READ_ONLY for the
            //     composite (sampled binding). ---
            VkImageMemoryBarrier mipsToRead[3] = {};
            for (uint32_t mip = 0; mip < 3; ++mip) {
                mipsToRead[mip].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                mipsToRead[mip].srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
                mipsToRead[mip].dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
                mipsToRead[mip].oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
                mipsToRead[mip].newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                mipsToRead[mip].image            = m_bloomMipImages[mip];
                mipsToRead[mip].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            }
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 3, mipsToRead);

            // --- Transition binding 30 (final LDR) for write. ---
            VkImageMemoryBarrier tbOut{};
            tbOut.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            tbOut.srcAccessMask    = m_nrdTonemapFirstFrame ? 0 : VK_ACCESS_SHADER_WRITE_BIT;
            tbOut.dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
            tbOut.oldLayout        = m_nrdTonemapFirstFrame ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
            tbOut.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
            tbOut.image            = m_nrdTonemappedImage;
            tbOut.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd,
                m_nrdTonemapFirstFrame ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &tbOut);
            m_nrdTonemapFirstFrame = false;

            // 4) Composite.
            NrdCinematicInputs ci{};
            ci.composedHDR     = m_nrdComposedView;
            ci.tonemappedOut   = m_nrdTonemappedView;
            ci.depthAOV        = m_depthAOVView;
            ci.envMapView      = m_envMapView;
            ci.envMapSampler   = m_envMapSampler;
            ci.bloomMip0View   = m_bloomMipViews[0];
            ci.bloomMip1View   = m_bloomMipViews[1];
            ci.bloomMip2View   = m_bloomMipViews[2];
            ci.bloomSampler    = m_bloomSampler;
            std::memcpy(ci.invView.data(), glm::value_ptr(pc.invView), sizeof(float) * 16);
            std::memcpy(ci.invProj.data(), glm::value_ptr(pc.invProj), sizeof(float) * 16);
            ci.extent[0]       = static_cast<float>(m_width);
            ci.extent[1]       = static_cast<float>(m_height);
            // env_intensity 2.0 (was 1.0): the env contributes both to sky pixels in
            // the cinematic composite AND as IBL via raygen. Doubling the sky-pixel
            // contribution makes the background read brighter — without affecting
            // raygen's lighting calculation (which uses m_envCDFIntegral directly).
            ci.envIntensity    = (m_envCDFIntegral > 0.0f && m_envMapView) ? 2.0f : 0.0f;
            // 4.G v2: rebalanced for general scenes. Original tuning (exposure
            // 1.0, bloom 0.8, vignette 0.6) over-crushed normal-luminance
            // scenes (chess board, BoomBox) into near-black. Lift overall
            // exposure, soften bloom + vignette so the cinematic curve flatters
            // material detail instead of swallowing it.
            ci.exposure         = 1.8f;    // was 1.0 — recover from AgX shadow crush on mid-luminance scenes
            ci.bloomStrength    = 0.45f;   // was 0.8 — subtler highlight glow
            ci.vignetteStrength = 0.3f;    // was 0.6 — product-shot vignette, not noir
            ci.saturation       = 1.15f;   // was 1.25 — gentle AgX desat comp
            ci.contrast         = 1.04f;   // was 1.05 — AgX already strong
            ci.tint             = {1.02f, 1.0f, 0.98f};   // gentle warm
            m_cinematicPost->dispatchComposite(cmd, ci);

            // After composite, binding 30 is in GENERAL with SHADER_WRITE access.
            // Downstream readback (readbackNrdTonemapped) transitions GENERAL→TRANSFER_SRC.
        }
    }
#endif  // OHAO_NRD_ENABLED

    // --- Transition output image to TRANSFER_SRC_OPTIMAL for readback/blit ---
    {
        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer.image = m_outputImage;
        toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT,
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
    m_viewChangedThisFrame = false;
    m_surfaceHistoryInitialized[m_surfaceHistoryWriteIndex] = true;
    m_surfaceHistoryWriteIndex = 1u - m_surfaceHistoryWriteIndex;
    m_shadingHistoryInitialized[m_shadingHistoryWriteIndex] = true;
    m_shadingHistoryWriteIndex = 1u - m_shadingHistoryWriteIndex;
}

} // namespace ohao
