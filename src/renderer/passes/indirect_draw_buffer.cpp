#include "indirect_draw_buffer.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace ohao {

IndirectDrawBuffer::~IndirectDrawBuffer() {
    cleanup();
}

bool IndirectDrawBuffer::initialize(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t maxDraws) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_maxDraws = maxDraws;

    m_commandBufferSize = sizeof(IndirectDrawCommand) * maxDraws;
    m_instanceBufferSize = sizeof(DrawInstance) * maxDraws;

    // Create GPU-side command buffer (for indirect draws)
    if (!createBuffer(m_commandBuffer, m_commandMemory, m_commandBufferSize,
                      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        return false;
    }

    // Create GPU-side instance buffer (for per-draw data)
    if (!createBuffer(m_instanceBuffer, m_instanceMemory, m_instanceBufferSize,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        return false;
    }

    // Create draw count buffer (for vkCmdDrawIndexedIndirectCount)
    if (!createBuffer(m_drawCountBuffer, m_drawCountMemory, sizeof(uint32_t),
                      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        return false;
    }

    // Create staging buffers (host-visible)
    VkBufferCreateInfo stagingInfo{};
    stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size = m_commandBufferSize;
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &stagingInfo, nullptr, &m_stagingCommandBuffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, m_stagingCommandBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_stagingCommandMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(m_device, m_stagingCommandBuffer, m_stagingCommandMemory, 0);
    vkMapMemory(m_device, m_stagingCommandMemory, 0, m_commandBufferSize, 0, &m_stagingCommandMapped);

    // Instance staging buffer
    stagingInfo.size = m_instanceBufferSize;
    if (vkCreateBuffer(m_device, &stagingInfo, nullptr, &m_stagingInstanceBuffer) != VK_SUCCESS) {
        return false;
    }

    vkGetBufferMemoryRequirements(m_device, m_stagingInstanceBuffer, &memReqs);
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_stagingInstanceMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(m_device, m_stagingInstanceBuffer, m_stagingInstanceMemory, 0);
    vkMapMemory(m_device, m_stagingInstanceMemory, 0, m_instanceBufferSize, 0, &m_stagingInstanceMapped);

    // Count staging buffer
    stagingInfo.size = sizeof(uint32_t);
    if (vkCreateBuffer(m_device, &stagingInfo, nullptr, &m_stagingCountBuffer) != VK_SUCCESS) {
        return false;
    }

    vkGetBufferMemoryRequirements(m_device, m_stagingCountBuffer, &memReqs);
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_stagingCountMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(m_device, m_stagingCountBuffer, m_stagingCountMemory, 0);
    vkMapMemory(m_device, m_stagingCountMemory, 0, sizeof(uint32_t), 0, &m_stagingCountMapped);

    // Reserve CPU vectors
    m_cpuCommands.reserve(maxDraws);
    m_cpuInstances.reserve(maxDraws);

    std::cout << "IndirectDrawBuffer initialized with max " << maxDraws << " draws" << std::endl;
    return true;
}

void IndirectDrawBuffer::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    auto destroyBuffer = [this](VkBuffer& buf, VkDeviceMemory& mem) {
        if (buf != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, buf, nullptr);
            buf = VK_NULL_HANDLE;
        }
        if (mem != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, mem, nullptr);
            mem = VK_NULL_HANDLE;
        }
    };

    destroyBuffer(m_commandBuffer, m_commandMemory);
    destroyBuffer(m_instanceBuffer, m_instanceMemory);
    destroyBuffer(m_drawCountBuffer, m_drawCountMemory);
    destroyBuffer(m_stagingCommandBuffer, m_stagingCommandMemory);
    destroyBuffer(m_stagingInstanceBuffer, m_stagingInstanceMemory);
    destroyBuffer(m_stagingCountBuffer, m_stagingCountMemory);

    m_cpuCommands.clear();
    m_cpuInstances.clear();
    m_drawCount = 0;
}

void IndirectDrawBuffer::reset() {
    m_cpuCommands.clear();
    m_cpuInstances.clear();
    m_drawCount = 0;
}

uint32_t IndirectDrawBuffer::addDraw(const IndirectDrawCommand& cmd, const DrawInstance& instance) {
    if (m_drawCount >= m_maxDraws) {
        std::cerr << "IndirectDrawBuffer: max draws exceeded" << std::endl;
        return UINT32_MAX;
    }

    uint32_t index = m_drawCount++;
    m_cpuCommands.push_back(cmd);
    m_cpuInstances.push_back(instance);
    return index;
}

void IndirectDrawBuffer::addDraws(const std::vector<IndirectDrawCommand>& cmds,
                                   const std::vector<DrawInstance>& instances) {
    if (cmds.size() != instances.size()) {
        std::cerr << "IndirectDrawBuffer: command/instance count mismatch" << std::endl;
        return;
    }

    for (size_t i = 0; i < cmds.size(); ++i) {
        addDraw(cmds[i], instances[i]);
    }
}

void IndirectDrawBuffer::upload(VkCommandBuffer cmd) {
    if (m_drawCount == 0) {
        return;
    }

    // Copy to staging buffers
    VkDeviceSize commandSize = sizeof(IndirectDrawCommand) * m_drawCount;
    VkDeviceSize instanceSize = sizeof(DrawInstance) * m_drawCount;

    memcpy(m_stagingCommandMapped, m_cpuCommands.data(), commandSize);
    memcpy(m_stagingInstanceMapped, m_cpuInstances.data(), instanceSize);
    memcpy(m_stagingCountMapped, &m_drawCount, sizeof(uint32_t));

    // Copy from staging to GPU
    VkBufferCopy copyRegion{};
    copyRegion.size = commandSize;
    vkCmdCopyBuffer(cmd, m_stagingCommandBuffer, m_commandBuffer, 1, &copyRegion);

    copyRegion.size = instanceSize;
    vkCmdCopyBuffer(cmd, m_stagingInstanceBuffer, m_instanceBuffer, 1, &copyRegion);

    copyRegion.size = sizeof(uint32_t);
    vkCmdCopyBuffer(cmd, m_stagingCountBuffer, m_drawCountBuffer, 1, &copyRegion);

    // Barrier for indirect buffer use
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.buffer = m_commandBuffer;
    barrier.offset = 0;
    barrier.size = commandSize;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr);

    barrier.buffer = m_instanceBuffer;
    barrier.size = instanceSize;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr);

    barrier.buffer = m_drawCountBuffer;
    barrier.size = sizeof(uint32_t);
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr);
}

bool IndirectDrawBuffer::createBuffer(VkBuffer& buffer, VkDeviceMemory& memory,
                                        VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, buffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(m_device, buffer, memory, 0);
    return true;
}

uint32_t IndirectDrawBuffer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type!");
}

// GpuCullPass implementation
GpuCullPass::~GpuCullPass() {
    cleanup();
}

bool GpuCullPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    if (!createDescriptors()) return false;
    if (!createPipeline()) return false;

    std::cout << "GpuCullPass initialized" << std::endl;
    return true;
}

void GpuCullPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    if (m_cullPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_cullPipeline, nullptr);
        m_cullPipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_descriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_descriptorLayout, nullptr);
        m_descriptorLayout = VK_NULL_HANDLE;
    }
}

void GpuCullPass::setMeshDescriptors(VkBuffer descriptorBuffer, uint32_t meshCount) {
    m_meshDescriptorBuffer = descriptorBuffer;
    m_meshCount = meshCount;
}

void GpuCullPass::setCameraData(const glm::mat4& viewProj, const glm::vec3& cameraPos) {
    m_viewProj = viewProj;
    m_cameraPos = cameraPos;
}

void GpuCullPass::execute(VkCommandBuffer cmd, IndirectDrawBuffer& drawBuffer) {
    if (m_cullPipeline == VK_NULL_HANDLE || m_meshCount == 0) {
        return;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_cullPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    // Extract frustum planes from view-projection matrix
    CullPushConstants pc{};
    pc.viewProj = m_viewProj;
    pc.cameraPos = glm::vec4(m_cameraPos, m_maxDistance);
    pc.meshCount = m_meshCount;
    pc.flags = (m_frustumCulling ? 1 : 0) |
               (m_occlusionCulling ? 2 : 0) |
               (m_distanceCulling ? 4 : 0);

    // Extract frustum planes from VP matrix
    glm::mat4 vp = glm::transpose(m_viewProj);
    pc.frustumPlanes[0] = vp[3] + vp[0]; // Left
    pc.frustumPlanes[1] = vp[3] - vp[0]; // Right
    pc.frustumPlanes[2] = vp[3] + vp[1]; // Bottom
    pc.frustumPlanes[3] = vp[3] - vp[1]; // Top
    pc.frustumPlanes[4] = vp[3] + vp[2]; // Near
    pc.frustumPlanes[5] = vp[3] - vp[2]; // Far

    // Normalize planes
    for (int i = 0; i < 6; ++i) {
        float len = glm::length(glm::vec3(pc.frustumPlanes[i]));
        pc.frustumPlanes[i] /= len;
    }

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(CullPushConstants), &pc);

    // Dispatch one thread per mesh
    uint32_t groupCount = (m_meshCount + 63) / 64;
    vkCmdDispatch(cmd, groupCount, 1, 1);

    // Barrier for indirect draw
    VkMemoryBarrier memBarrier{};
    memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    memBarrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        0, 1, &memBarrier, 0, nullptr, 0, nullptr);
}

bool GpuCullPass::createDescriptors() {
    // Bindings: mesh descriptors, output commands, output count
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};

    // Input mesh descriptors
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Input instance data
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Output draw commands
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Output draw count
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorLayout) != VK_SUCCESS) {
        return false;
    }

    // Create pool
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 4;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    // Allocate set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorLayout;

    return vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet) == VK_SUCCESS;
}

bool GpuCullPass::createPipeline() {
    // Push constant range
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(CullPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        return false;
    }

    // Note: The actual compute shader would be loaded here
    // For now, we'll skip shader loading as it depends on the shader compilation system
    std::cout << "GpuCullPass pipeline layout created (shader loading deferred)" << std::endl;

    return true;
}

} // namespace ohao
