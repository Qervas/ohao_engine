// Sub-plan 4.E T1: NrdTonemap — Vulkan compute pipeline that applies ACES +
// sRGB gamma to NRD's composed HDR output.

#include "render/rt/denoise/nrd_tonemap.hpp"

#ifdef OHAO_NRD_ENABLED

#include <array>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>

namespace ohao {

namespace {

// OHAO's shader CMake flattens directory paths with underscores.
std::vector<char> readShaderSpv(const std::string& name) {
    const std::string searchPaths[] = {
        name,
        "build/shaders/" + name,
        "build/Release/bin/shaders/" + name,
    };
    for (const auto& p : searchPaths) {
        std::ifstream file(p, std::ios::ate | std::ios::binary);
        if (file.is_open()) {
            size_t size = static_cast<size_t>(file.tellg());
            std::vector<char> buf(size);
            file.seekg(0);
            file.read(buf.data(), size);
            return buf;
        }
    }
    return {};
}

}  // anonymous namespace

struct NrdTonemap::Impl {
    VkDevice              device         = VK_NULL_HANDLE;
    VkPhysicalDevice      physicalDevice = VK_NULL_HANDLE;
    uint32_t              width          = 0;
    uint32_t              height         = 0;

    VkDescriptorSetLayout setLayout      = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout = VK_NULL_HANDLE;
    VkPipeline            pipeline       = VK_NULL_HANDLE;
    VkDescriptorPool      descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet       descriptorSet  = VK_NULL_HANDLE;
};

NrdTonemap::NrdTonemap()  : m_impl(std::make_unique<Impl>()) {}
NrdTonemap::~NrdTonemap() { shutdown(); }

bool NrdTonemap::initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                             uint32_t width, uint32_t height) {
    m_impl->device         = device;
    m_impl->physicalDevice = physicalDevice;
    m_impl->width          = width;
    m_impl->height         = height;

    // 2 storage-image bindings, COMPUTE stage.
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    if (VkResult r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_impl->setLayout); r != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkCreateDescriptorSetLayout failed: " << int(r) << std::endl;
        return false;
    }

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts    = &m_impl->setLayout;
    if (VkResult r = vkCreatePipelineLayout(device, &plInfo, nullptr, &m_impl->pipelineLayout); r != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkCreatePipelineLayout failed: " << int(r) << std::endl;
        return false;
    }

    auto spv = readShaderSpv("rt_nrd_tonemap.comp.spv");
    if (spv.empty()) {
        std::cerr << "[NRD tonemap] failed to load rt_nrd_tonemap.comp.spv" << std::endl;
        return false;
    }

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = spv.size();
    smInfo.pCode    = reinterpret_cast<const uint32_t*>(spv.data());
    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (VkResult r = vkCreateShaderModule(device, &smInfo, nullptr, &shaderModule); r != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkCreateShaderModule failed: " << int(r) << std::endl;
        return false;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shaderModule;
    stage.pName  = "main";

    VkComputePipelineCreateInfo pipeInfo{};
    pipeInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeInfo.stage  = stage;
    pipeInfo.layout = m_impl->pipelineLayout;
    VkResult pipeResult = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr,
                                                    &m_impl->pipeline);
    vkDestroyShaderModule(device, shaderModule, nullptr);
    if (pipeResult != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkCreateComputePipelines failed: " << int(pipeResult) << std::endl;
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    poolInfo.maxSets       = 1;
    if (VkResult r = vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_impl->descriptorPool); r != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkCreateDescriptorPool failed: " << int(r) << std::endl;
        return false;
    }

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = m_impl->descriptorPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &m_impl->setLayout;
    if (VkResult r = vkAllocateDescriptorSets(device, &dsai, &m_impl->descriptorSet); r != VK_SUCCESS) {
        std::cerr << "[NRD tonemap] vkAllocateDescriptorSets failed: " << int(r) << std::endl;
        return false;
    }

    std::cout << "[NRD tonemap] pipeline ready @ " << width << "x" << height << std::endl;
    return true;
}

void NrdTonemap::shutdown() {
    if (!m_impl || !m_impl->device) return;
    VkDevice d = m_impl->device;
    if (m_impl->descriptorPool) { vkDestroyDescriptorPool(d, m_impl->descriptorPool, nullptr); m_impl->descriptorPool = VK_NULL_HANDLE; }
    if (m_impl->pipeline)       { vkDestroyPipeline(d, m_impl->pipeline, nullptr);              m_impl->pipeline       = VK_NULL_HANDLE; }
    if (m_impl->pipelineLayout) { vkDestroyPipelineLayout(d, m_impl->pipelineLayout, nullptr);  m_impl->pipelineLayout = VK_NULL_HANDLE; }
    if (m_impl->setLayout)      { vkDestroyDescriptorSetLayout(d, m_impl->setLayout, nullptr);  m_impl->setLayout      = VK_NULL_HANDLE; }
    m_impl->descriptorSet = VK_NULL_HANDLE;
    m_impl->device        = VK_NULL_HANDLE;
}

void NrdTonemap::dispatch(VkCommandBuffer cmd, const NrdTonemapInputs& inputs) {
    if (!m_impl->pipeline) return;

    std::array<VkDescriptorImageInfo, 2> imageInfos{};
    imageInfos[0].imageView   = inputs.composedHDR;
    imageInfos[1].imageView   = inputs.tonemappedOut;
    for (auto& ii : imageInfos) ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_impl->descriptorSet;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo      = &imageInfos[i];
    }
    vkUpdateDescriptorSets(m_impl->device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_impl->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_impl->pipelineLayout, 0, 1,
                             &m_impl->descriptorSet, 0, nullptr);

    const uint32_t gx = (m_impl->width  + 7u) / 8u;
    const uint32_t gy = (m_impl->height + 7u) / 8u;
    vkCmdDispatch(cmd, gx, gy, 1);
}

}  // namespace ohao

#else  // OHAO_NRD_ENABLED

namespace ohao {

struct NrdTonemap::Impl {};
NrdTonemap::NrdTonemap()  : m_impl(std::make_unique<Impl>()) {}
NrdTonemap::~NrdTonemap() = default;

bool NrdTonemap::initialize(VkDevice, VkPhysicalDevice, uint32_t, uint32_t) { return false; }
void NrdTonemap::shutdown() {}
void NrdTonemap::dispatch(VkCommandBuffer, const NrdTonemapInputs&) {}

}  // namespace ohao

#endif  // OHAO_NRD_ENABLED
