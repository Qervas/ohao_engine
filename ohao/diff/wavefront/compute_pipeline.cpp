#include "diff/wavefront/compute_pipeline.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>

namespace ohao::diff {
namespace {

// Search a few candidate locations for the compiled SPV -- the exact
// relative path depends on whether the calling binary is launched from the
// repo root or from its own output directory. Duplicated (not shared) from
// tests/diff/gpu_probe_context.cpp's identical helper: that copy still
// backs the probe drivers this task does not migrate (runVisibilityProbe,
// runWavefrontGenerateProbe, runWavefrontIntersectProbe,
// runWavefrontScatterProbe -- Task 4), and this library must not depend on
// anything under tests/.
std::vector<uint32_t> loadSpv(const char* filename) {
    const std::vector<std::string> searchPaths = {
        std::string("bin/shaders/") + filename,
        std::string("build/Release/bin/shaders/") + filename,
        std::string("build/Debug/bin/shaders/") + filename,
        std::string("build/shaders/") + filename,
        std::string("shaders/") + filename,
    };

    std::ifstream file;
    for (const auto& p : searchPaths) {
        file.open(p, std::ios::ate | std::ios::binary);
        if (file.is_open()) break;
    }
    if (!file.is_open()) {
        std::fprintf(stderr, "[ComputePipeline] could not find shader SPV: %s\n", filename);
        return {};
    }

    const size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fileSize));
    return buffer;
}

}  // namespace

ComputePipeline::~ComputePipeline() {
    if (m_device != VK_NULL_HANDLE) destroy(m_device);
}

bool ComputePipeline::build(VkDevice device, const char* spvName,
                            std::span<const VkDescriptorType> bindings,
                            uint32_t pushConstantSize) {
    m_device = device;

    // --- Shader module ---
    const std::vector<uint32_t> spv = loadSpv(spvName);
    if (spv.empty()) return false;

    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = spv.size() * sizeof(uint32_t);
    moduleInfo.pCode = spv.data();
    if (vkCreateShaderModule(device, &moduleInfo, nullptr, &m_shaderModule) != VK_SUCCESS) {
        std::fprintf(stderr, "[ComputePipeline] vkCreateShaderModule failed (%s)\n", spvName);
        return false;
    }

    // --- Descriptor set layout: one binding per span entry, index == binding ---
    std::vector<VkDescriptorSetLayoutBinding> layoutBindings(bindings.size());
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        layoutBindings[i].binding = static_cast<uint32_t>(i);
        layoutBindings[i].descriptorType = bindings[i];
        layoutBindings[i].descriptorCount = 1;
        layoutBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = static_cast<uint32_t>(layoutBindings.size());
    setLayoutInfo.pBindings = layoutBindings.data();
    if (vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &m_setLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[ComputePipeline] vkCreateDescriptorSetLayout failed (%s)\n", spvName);
        destroy(device);
        return false;
    }

    // --- Pipeline layout ---
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = pushConstantSize;

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_setLayout;
    if (pushConstantSize > 0) {
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    }
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        std::fprintf(stderr, "[ComputePipeline] vkCreatePipelineLayout failed (%s)\n", spvName);
        destroy(device);
        return false;
    }

    // --- Pipeline ---
    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = m_shaderModule;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = m_pipelineLayout;
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) !=
        VK_SUCCESS) {
        std::fprintf(stderr, "[ComputePipeline] vkCreateComputePipelines failed (%s)\n", spvName);
        destroy(device);
        return false;
    }

    // --- Descriptor pool: one VkDescriptorPoolSize per distinct type in
    // `bindings`, sized to how many bindings requested that type. ---
    std::vector<VkDescriptorPoolSize> poolSizes;
    for (VkDescriptorType type : bindings) {
        auto it = std::find_if(poolSizes.begin(), poolSizes.end(),
                               [type](const VkDescriptorPoolSize& s) { return s.type == type; });
        if (it != poolSizes.end()) {
            it->descriptorCount += 1;
        } else {
            poolSizes.push_back(VkDescriptorPoolSize{type, 1});
        }
    }

    VkDescriptorPoolCreateInfo descPoolInfo{};
    descPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descPoolInfo.maxSets = 1;
    descPoolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    descPoolInfo.pPoolSizes = poolSizes.data();
    if (vkCreateDescriptorPool(device, &descPoolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        std::fprintf(stderr, "[ComputePipeline] vkCreateDescriptorPool failed (%s)\n", spvName);
        destroy(device);
        return false;
    }

    VkDescriptorSetAllocateInfo descAllocInfo{};
    descAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descAllocInfo.descriptorPool = m_descriptorPool;
    descAllocInfo.descriptorSetCount = 1;
    descAllocInfo.pSetLayouts = &m_setLayout;
    if (vkAllocateDescriptorSets(device, &descAllocInfo, &m_descriptorSet) != VK_SUCCESS) {
        std::fprintf(stderr, "[ComputePipeline] vkAllocateDescriptorSets failed (%s)\n", spvName);
        destroy(device);
        return false;
    }

    // Only recorded on full success -- a failed build() must leave
    // bindBuffers/bindAccelerationStructure unable to "succeed" against a
    // half-built object (m_bindingTypes empty makes every binding index
    // out of range).
    m_bindingTypes.assign(bindings.begin(), bindings.end());
    return true;
}

void ComputePipeline::destroy(VkDevice device) {
    // Reverse creation order, mirroring dispatchStorageBufferCompute's
    // cleanup. The descriptor set itself is not separately freed: it was
    // allocated from m_descriptorPool without
    // VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, so destroying the
    // pool implicitly frees it (VkDescriptorPool spec, "Destruction").
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
        m_descriptorSet = VK_NULL_HANDLE;
    }
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_setLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
    if (m_shaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, m_shaderModule, nullptr);
        m_shaderModule = VK_NULL_HANDLE;
    }
    m_bindingTypes.clear();
    // m_device is deliberately left set: a second destroy(device) call (or
    // the destructor, if build() failed after storing a device but before
    // ever succeeding) must remain a no-op, which the VK_NULL_HANDLE guards
    // above already guarantee regardless of m_device's value.
}

bool ComputePipeline::bindBuffers(VkDevice device, std::span<const VkBuffer> buffers) {
    if (buffers.size() > m_bindingTypes.size()) {
        std::fprintf(stderr,
                     "[ComputePipeline] bindBuffers: %zu buffers exceeds %zu declared bindings\n",
                     buffers.size(), m_bindingTypes.size());
        return false;
    }
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        if (m_bindingTypes[i] != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            std::fprintf(stderr,
                         "[ComputePipeline] bindBuffers: binding %zu was not declared "
                         "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER\n",
                         i);
            return false;
        }
    }

    // Sized up front and never reallocated afterwards, so the pBufferInfo
    // pointers taken below stay valid through vkUpdateDescriptorSets.
    std::vector<VkDescriptorBufferInfo> bufferInfos(buffers.size());
    std::vector<VkWriteDescriptorSet> writes(buffers.size());
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        bufferInfos[i].buffer = buffers[i];
        bufferInfos[i].offset = 0;
        bufferInfos[i].range = VK_WHOLE_SIZE;

        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_descriptorSet;
        writes[i].dstBinding = static_cast<uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfos[i];
    }
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return true;
}

bool ComputePipeline::bindAccelerationStructure(VkDevice device, uint32_t binding,
                                                VkAccelerationStructureKHR accel) {
    if (binding >= m_bindingTypes.size() ||
        m_bindingTypes[binding] != VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
        std::fprintf(stderr,
                     "[ComputePipeline] bindAccelerationStructure: binding %u was not declared "
                     "VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR\n",
                     binding);
        return false;
    }

    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &accel;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext = &asWrite;
    write.dstSet = m_descriptorSet;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    return true;
}

}  // namespace ohao::diff
