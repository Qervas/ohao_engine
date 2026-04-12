// GPU Compute Skinning — transforms vertices with bone matrices on GPU,
// outputs position buffer for BLAS rebuild.

#include "gpu_skinning.hpp"
#include <iostream>
#include <fstream>
#include <cstring>
#include <array>

namespace ohao {

// Forward: findMemoryType from renderer_impl
uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);

static VkShaderModule loadShaderFromFile(VkDevice device, const std::string& filename) {
    // Search multiple paths
    std::vector<std::string> paths = {
        "build/shaders/" + filename,
        "bin/shaders/" + filename,
        filename
    };
    for (const auto& path : paths) {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) continue;
        size_t fileSize = static_cast<size_t>(file.tellg());
        std::vector<char> buffer(fileSize);
        file.seekg(0);
        file.read(buffer.data(), fileSize);

        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = buffer.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(buffer.data());

        VkShaderModule shaderModule;
        if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) == VK_SUCCESS) {
            return shaderModule;
        }
    }
    return VK_NULL_HANDLE;
}

GPUSkinning::~GPUSkinning() {
    cleanup();
}

bool GPUSkinning::initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                              VkCommandPool commandPool, VkQueue queue) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_commandPool = commandPool;
    m_queue = queue;

    if (!createComputePipeline()) {
        std::cerr << "[GPUSkinning] Failed to create compute pipeline" << std::endl;
        return false;
    }

    // Create descriptor pool (allow up to 64 animated meshes)
    std::array<VkDescriptorPoolSize, 2> poolSizes = {{
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 192 },  // 3 SSBOs per mesh × 64
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64 },   // 1 UBO per mesh × 64
    }};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 64;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    std::cout << "[GPUSkinning] Initialized" << std::endl;
    return true;
}

void GPUSkinning::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);

    for (auto& entry : m_entries) {
        if (entry.skinnedPosBuffer) vkDestroyBuffer(m_device, entry.skinnedPosBuffer, nullptr);
        if (entry.skinnedPosMem) vkFreeMemory(m_device, entry.skinnedPosMem, nullptr);
        if (entry.skinnedNormBuffer) vkDestroyBuffer(m_device, entry.skinnedNormBuffer, nullptr);
        if (entry.skinnedNormMem) vkFreeMemory(m_device, entry.skinnedNormMem, nullptr);
        if (entry.boneUBO) vkDestroyBuffer(m_device, entry.boneUBO, nullptr);
        if (entry.boneUBOMem) vkFreeMemory(m_device, entry.boneUBOMem, nullptr);
    }
    m_entries.clear();

    if (m_pipeline) { vkDestroyPipeline(m_device, m_pipeline, nullptr); m_pipeline = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }
    if (m_descriptorPool) { vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr); m_descriptorPool = VK_NULL_HANDLE; }
    if (m_descriptorLayout) { vkDestroyDescriptorSetLayout(m_device, m_descriptorLayout, nullptr); m_descriptorLayout = VK_NULL_HANDLE; }
}

bool GPUSkinning::createComputePipeline() {
    // Descriptor layout: 4 bindings
    // 0: input vertices (SSBO, readonly)
    // 1: output positions (SSBO, writeonly)
    // 2: output normals (SSBO, writeonly)
    // 3: bone matrices (UBO)
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (int i = 0; i < 3; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorLayout) != VK_SUCCESS)
        return false;

    // Push constants
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(SkinPushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_descriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        return false;

    // Load compute shader
    VkShaderModule compShader = loadShaderFromFile(m_device, "compute_skinning.comp.spv");
    if (compShader == VK_NULL_HANDLE) {
        std::cerr << "[GPUSkinning] Failed to load skinning.comp.spv" << std::endl;
        return false;
    }

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = compShader;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = m_pipelineLayout;

    VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);
    vkDestroyShaderModule(m_device, compShader, nullptr);
    return result == VK_SUCCESS;
}

uint32_t GPUSkinning::registerMesh(VkBuffer srcVertexBuffer, uint32_t vertexCount, uint32_t vertexOffset) {
    SkinEntry entry{};
    entry.srcVertexBuffer = srcVertexBuffer;
    entry.vertexCount = vertexCount;
    entry.vertexOffset = vertexOffset;

    VkDeviceSize posSize = vertexCount * 3 * sizeof(float);
    VkDeviceSize normSize = vertexCount * 3 * sizeof(float);
    VkDeviceSize boneSize = 128 * sizeof(glm::mat4) + 16; // 128 mat4 + boneCount int + padding

    auto createBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
                           VkBuffer& buffer, VkDeviceMemory& memory) {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = size;
        bufInfo.usage = usage;
        vkCreateBuffer(m_device, &bufInfo, nullptr, &buffer);

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(m_device, buffer, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(m_physicalDevice, memReqs.memoryTypeBits, memProps);

        // Device address requires special allocation flag
        VkMemoryAllocateFlagsInfo flagsInfo{};
        if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
            flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
            flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
            allocInfo.pNext = &flagsInfo;
        }

        vkAllocateMemory(m_device, &allocInfo, nullptr, &memory);
        vkBindBufferMemory(m_device, buffer, memory, 0);
    };

    // Output position buffer (device-local, used for BLAS input)
    createBuffer(posSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        entry.skinnedPosBuffer, entry.skinnedPosMem);

    // Output normal buffer
    createBuffer(normSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        entry.skinnedNormBuffer, entry.skinnedNormMem);

    // Bone matrices UBO (host-visible for fast upload)
    createBuffer(boneSize,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        entry.boneUBO, entry.boneUBOMem);
    vkMapMemory(m_device, entry.boneUBOMem, 0, boneSize, 0, &entry.boneUBOMapped);

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = m_descriptorPool;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts = &m_descriptorLayout;
    vkAllocateDescriptorSets(m_device, &dsAlloc, &entry.descriptorSet);

    // Write descriptors
    std::array<VkDescriptorBufferInfo, 4> bufInfos{};
    bufInfos[0].buffer = srcVertexBuffer;
    bufInfos[0].offset = 0;  // shader handles offset via push constant
    bufInfos[0].range = VK_WHOLE_SIZE;  // bind entire buffer

    bufInfos[1].buffer = entry.skinnedPosBuffer;
    bufInfos[1].offset = 0;
    bufInfos[1].range = posSize;

    bufInfos[2].buffer = entry.skinnedNormBuffer;
    bufInfos[2].offset = 0;
    bufInfos[2].range = normSize;

    bufInfos[3].buffer = entry.boneUBO;
    bufInfos[3].offset = 0;
    bufInfos[3].range = boneSize;

    std::array<VkWriteDescriptorSet, 4> writes{};
    for (int i = 0; i < 4; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = entry.descriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = (i < 3) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[i].pBufferInfo = &bufInfos[i];
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    uint32_t handle = static_cast<uint32_t>(m_entries.size());
    m_entries.push_back(std::move(entry));
    return handle;
}

void GPUSkinning::skin(VkCommandBuffer cmd, uint32_t meshHandle,
                        const std::vector<glm::mat4>& boneMatrices) {
    if (meshHandle >= m_entries.size()) return;
    auto& entry = m_entries[meshHandle];

    // Upload bone matrices
    if (entry.boneUBOMapped && !boneMatrices.empty()) {
        size_t copySize = std::min(boneMatrices.size(), size_t(128)) * sizeof(glm::mat4);
        memcpy(entry.boneUBOMapped, boneMatrices.data(), copySize);
        int boneCount = static_cast<int>(std::min(boneMatrices.size(), size_t(128)));
        memcpy(static_cast<uint8_t*>(entry.boneUBOMapped) + 128 * sizeof(glm::mat4),
               &boneCount, sizeof(int));
    }

    // Debug first call
    static bool debugOnce = true;
    if (debugOnce) {
        debugOnce = false;
        std::cout << "[SkinDebug] mesh=" << meshHandle
                  << " vCount=" << entry.vertexCount
                  << " vOffset=" << entry.vertexOffset
                  << " bones=" << boneMatrices.size()
                  << " bone0[0]=" << boneMatrices[0][0][0]
                  << "," << boneMatrices[0][1][1]
                  << "," << boneMatrices[0][2][2]
                  << "," << boneMatrices[0][3][3] << std::endl;
    }

    // Bind compute pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &entry.descriptorSet, 0, nullptr);

    // Push constants
    SkinPushConstants pc{};
    pc.vertexCount = entry.vertexCount;
    pc.vertexStride = 23; // 92 bytes / 4 = 23 floats
    pc.vertexOffset = entry.vertexOffset;
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(SkinPushConstants), &pc);

    // Dispatch
    uint32_t groupCount = (entry.vertexCount + 255) / 256;
    vkCmdDispatch(cmd, groupCount, 1, 1);

    // Barrier: compute write → acceleration structure read
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

VkBuffer GPUSkinning::getSkinnedPositionBuffer(uint32_t meshHandle) const {
    return meshHandle < m_entries.size() ? m_entries[meshHandle].skinnedPosBuffer : VK_NULL_HANDLE;
}

VkBuffer GPUSkinning::getSkinnedNormalBuffer(uint32_t meshHandle) const {
    return meshHandle < m_entries.size() ? m_entries[meshHandle].skinnedNormBuffer : VK_NULL_HANDLE;
}

uint32_t GPUSkinning::getVertexCount(uint32_t meshHandle) const {
    return meshHandle < m_entries.size() ? m_entries[meshHandle].vertexCount : 0;
}

} // namespace ohao
