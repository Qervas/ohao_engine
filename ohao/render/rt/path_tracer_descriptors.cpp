// Extracted from path_tracer.cpp — descriptor set layout + pool + set creation.
// Kept as a member of class PathTracer; no behavior change.

#include "path_tracer.hpp"

#include <iostream>
#include <vector>

namespace ohao {

bool PathTracer::createDescriptorResources() {
    // Layout: RT resources + realtime history validation images
    //   0: TLAS (acceleration structure)           — RAYGEN
    //   1: Accumulation buffer (storage image)     — RAYGEN   (RGBA32F)
    //   2: Output image (storage image)            — RAYGEN   (RGBA8)
    //   3: Material buffer SSBO                    — RAYGEN + CLOSEST_HIT
    //   4: Normal buffer SSBO (vec4 per vertex)    — CLOSEST_HIT
    //   5: Index buffer SSBO (uint per index)      — CLOSEST_HIT
    //   6: Albedo AOV (storage image)              — RAYGEN   (RGBA32F)
    //   7: Normal AOV (storage image)              — RAYGEN   (RGBA32F)
    VkDescriptorSetLayoutBinding bindings[29] = {};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[7].binding = 7;
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // 8: UV buffer SSBO
    bindings[8].binding = 8;
    bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[8].descriptorCount = 1;
    bindings[8].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    // 9: Material ID buffer (per-triangle)
    bindings[9].binding = 9;
    bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[9].descriptorCount = 1;
    bindings[9].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    // 10: Material color buffer (per-material)
    bindings[10].binding = 10;
    bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[10].descriptorCount = 1;
    bindings[10].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    // 11: Light buffer (SSBO) — accessed by raygen + miss
    bindings[11].binding = 11;
    bindings[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[11].descriptorCount = 1;
    bindings[11].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;

    // 12: Bindless textures — accessed by closest-hit + miss (env map)
    bindings[12].binding = 12;
    bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[12].descriptorCount = m_maxBindlessTextures;
    bindings[12].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    // 13: Previous-frame surface history image
    bindings[13].binding = 13;
    bindings[13].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[13].descriptorCount = 1;
    bindings[13].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // 14: Current-frame surface history image
    bindings[14].binding = 14;
    bindings[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[14].descriptorCount = 1;
    bindings[14].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // 15: Previous-frame shading history image
    bindings[15].binding = 15;
    bindings[15].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[15].descriptorCount = 1;
    bindings[15].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // 16: Current-frame shading history image
    bindings[16].binding = 16;
    bindings[16].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[16].descriptorCount = 1;
    bindings[16].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // 17: Env marginal CDF (storage buffer) — accessed by raygen + miss
    bindings[17].binding = 17;
    bindings[17].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[17].descriptorCount = 1;
    bindings[17].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;

    // 18: Env conditional CDF (storage buffer) — accessed by raygen + miss
    bindings[18].binding = 18;
    bindings[18].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[18].descriptorCount = 1;
    bindings[18].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;

    // 19: Motion vector AOV (RG16F storage image) — Sub-plan 3.A
    bindings[19].binding = 19;
    bindings[19].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[19].descriptorCount = 1;
    bindings[19].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Binding 20: depth AOV (R32F storage image) — Sub-plan 3.B
    bindings[20].binding         = 20;
    bindings[20].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[20].descriptorCount = 1;
    bindings[20].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Binding 21: roughness AOV (R8 UNORM storage image) — Sub-plan 3.B
    bindings[21].binding         = 21;
    bindings[21].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[21].descriptorCount = 1;
    bindings[21].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Binding 22: diffuse radiance (RGBA32F storage image, raw) — Sub-plan 3.C / 3.C.5 / 3.C.6
    bindings[22].binding         = 22;
    bindings[22].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[22].descriptorCount = 1;
    bindings[22].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Binding 23: specular radiance (RGBA32F storage image, raw) — Sub-plan 3.C / 3.C.5 / 3.C.6
    bindings[23].binding         = 23;
    bindings[23].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[23].descriptorCount = 1;
    bindings[23].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Binding 24: diffuse albedo AOV (RGBA8 UNORM storage image) — Sub-plan 3.C.6
    bindings[24].binding         = 24;
    bindings[24].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[24].descriptorCount = 1;
    bindings[24].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Binding 25: specular color AOV (RGBA8 UNORM storage image) — Sub-plan 3.C.6
    bindings[25].binding         = 25;
    bindings[25].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[25].descriptorCount = 1;
    bindings[25].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Binding 26: normal+roughness packed AOV (RGBA8 UNORM storage image) — Sub-plan 4.B
    bindings[26].binding         = 26;
    bindings[26].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[26].descriptorCount = 1;
    bindings[26].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Binding 27: NRD denoised diffuse (RGBA32F storage image) — Sub-plan 4.C
    // Written by NRD compute dispatch (T3). Raygen stage flag kept for layout parity.
    bindings[27].binding         = 27;
    bindings[27].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[27].descriptorCount = 1;
    bindings[27].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Binding 28: NRD denoised specular (RGBA32F storage image) — Sub-plan 4.C
    bindings[28].binding         = 28;
    bindings[28].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[28].descriptorCount = 1;
    bindings[28].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Enable bindless: variable count on the LAST binding only
    VkDescriptorBindingFlags bindingFlags[29] = {};
    bindingFlags[12] = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT
                     | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
                     | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount = 29;
    flagsInfo.pBindingFlags = bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext = &flagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 29;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
        return false;

    // Pool — allocate enough for bindless textures
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 18},  // +1 MV (3.A), +2 depth/roughness (3.B), +2 diff/spec radiance (3.C), +2 albedo/specColor (3.C.6), +1 normalRoughness (4.B), +2 denoised out (4.C)
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 9},  // +2 for env CDF marginal + conditional
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_maxBindlessTextures},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        return false;

    // Allocate set with variable descriptor count for bindless textures
    uint32_t variableCount = m_maxBindlessTextures;
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableInfo{};
    variableInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    variableInfo.descriptorSetCount = 1;
    variableInfo.pDescriptorCounts = &variableCount;

    VkDescriptorSetAllocateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.pNext = &variableInfo;
    setInfo.descriptorPool = m_descriptorPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &m_descriptorSetLayout;

    return vkAllocateDescriptorSets(m_device, &setInfo, &m_descriptorSet) == VK_SUCCESS;
}

} // namespace ohao
