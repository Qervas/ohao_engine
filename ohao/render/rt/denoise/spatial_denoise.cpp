#include "render/rt/denoise/spatial_denoise.hpp"

#include <fstream>
#include <string>
#include <vector>

#include "core/log.hpp"

namespace ohao {

namespace {

std::vector<char> readShaderSpv(const std::string& name) {
    for (const std::string& p : {name, "build/shaders/" + name, "build/Release/bin/shaders/" + name,
                                 "bin/shaders/" + name}) {
        std::ifstream file(p, std::ios::ate | std::ios::binary);
        if (!file.is_open()) continue;
        std::vector<char> buf(static_cast<size_t>(file.tellg()));
        file.seekg(0);
        file.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        return buf;
    }
    return {};
}

}  // namespace

bool SpatialDenoise::initialize(VkDevice device) {
    m_device = device;

    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_setLayout) != VK_SUCCESS) return false;

    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo plInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &m_setLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device, &plInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) return false;

    const std::vector<char> spv = readShaderSpv("rt_pt_spatial_denoise.comp.spv");
    if (spv.empty()) {
        OHAO_LOG(Error, render, "spatial denoise: rt_pt_spatial_denoise.comp.spv not found");
        return false;
    }
    VkShaderModuleCreateInfo smInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smInfo.codeSize = spv.size();
    smInfo.pCode = reinterpret_cast<const uint32_t*>(spv.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &smInfo, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo pipeInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                      module, "main", nullptr};
    pipeInfo.layout = m_pipelineLayout;
    const VkResult pr = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &m_pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    if (pr != VK_SUCCESS) return false;

    // One set per frame in flight: a frame rewrites only its own (VUID 03047).
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 * MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_pool) != VK_SUCCESS) return false;
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts{};
    layouts.fill(m_setLayout);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    ai.pSetLayouts = layouts.data();
    return vkAllocateDescriptorSets(device, &ai, m_sets.data()) == VK_SUCCESS;
}

void SpatialDenoise::shutdown() {
    if (!m_device) return;
    if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    if (m_pipeline) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    m_pool = VK_NULL_HANDLE;
    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_sets = {};
    m_device = VK_NULL_HANDLE;
}

void SpatialDenoise::dispatch(VkCommandBuffer cmd, uint32_t frameSlot, VkImageView accum, VkImageView normal,
                              VkImageView output, uint32_t width, uint32_t height) {
    if (!m_pipeline || frameSlot >= MAX_FRAMES_IN_FLIGHT) return;
    const VkDescriptorSet set = m_sets[frameSlot];
    const VkDescriptorImageInfo infos[3] = {{VK_NULL_HANDLE, accum, VK_IMAGE_LAYOUT_GENERAL},
                                            {VK_NULL_HANDLE, normal, VK_IMAGE_LAYOUT_GENERAL},
                                            {VK_NULL_HANDLE, output, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 1, &set, 0, nullptr);
    const uint32_t size[2] = {width, height};
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size), size);
    vkCmdDispatch(cmd, (width + 15u) / 16u, (height + 15u) / 16u, 1);
}

}  // namespace ohao
