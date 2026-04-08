#include "rt_shadow_technique.hpp"
#include <fstream>
#include <cstring>
#include <algorithm>

namespace ohao {

RTShadowTechnique::~RTShadowTechnique() {
    destroy();
}

bool RTShadowTechnique::loadFunctionPointers() {
    vkCreateRayTracingPipelinesKHR =
        (PFN_vkCreateRayTracingPipelinesKHR)vkGetDeviceProcAddr(m_device, "vkCreateRayTracingPipelinesKHR");
    vkGetRayTracingShaderGroupHandlesKHR =
        (PFN_vkGetRayTracingShaderGroupHandlesKHR)vkGetDeviceProcAddr(m_device, "vkGetRayTracingShaderGroupHandlesKHR");
    vkCmdTraceRaysKHR =
        (PFN_vkCmdTraceRaysKHR)vkGetDeviceProcAddr(m_device, "vkCmdTraceRaysKHR");
    vkGetBufferDeviceAddressFn =
        (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(m_device, "vkGetBufferDeviceAddress");

    return vkCreateRayTracingPipelinesKHR && vkGetRayTracingShaderGroupHandlesKHR &&
           vkCmdTraceRaysKHR && vkGetBufferDeviceAddressFn;
}

uint32_t RTShadowTechnique::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return UINT32_MAX;
}

VkDeviceAddress RTShadowTechnique::getBufferDeviceAddress(VkBuffer buffer) {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddressFn(m_device, &info);
}

static std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) return {};
    size_t size = (size_t)file.tellg();
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), size);
    return buffer;
}

bool RTShadowTechnique::init(VkDevice device, VkPhysicalDevice physicalDevice,
                              uint32_t width, uint32_t height) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_width = width;
    m_height = height;

    if (!loadFunctionPointers()) {
        std::cerr << "[RTShadow] Failed to load RT function pointers" << std::endl;
        return false;
    }

    if (!createOutputImage()) {
        std::cerr << "[RTShadow] Failed to create output image" << std::endl;
        return false;
    }

    if (!createDescriptorResources()) {
        std::cerr << "[RTShadow] Failed to create descriptor resources" << std::endl;
        return false;
    }

    if (!createRTPipeline()) {
        std::cerr << "[RTShadow] Failed to create RT pipeline" << std::endl;
        return false;
    }

    if (!createShaderBindingTable()) {
        std::cerr << "[RTShadow] Failed to create SBT" << std::endl;
        return false;
    }

    std::cout << "[RTShadow] Initialized (" << width << "x" << height << ")" << std::endl;
    return true;
}

bool RTShadowTechnique::createOutputImage() {
    // Shadow mask: R8_UNORM — 0=shadowed, 1=lit
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8_UNORM;
    imageInfo.extent = {m_width, m_height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_shadowMask) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_device, m_shadowMask, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_shadowMaskMemory) != VK_SUCCESS) return false;
    vkBindImageMemory(m_device, m_shadowMask, m_shadowMaskMemory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_shadowMask;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    return vkCreateImageView(m_device, &viewInfo, nullptr, &m_shadowMaskView) == VK_SUCCESS;
}

bool RTShadowTechnique::createDescriptorResources() {
    // Layout: binding 0 = TLAS, binding 1 = shadow mask (storage image),
    //         binding 2 = GBuffer position, binding 3 = GBuffer normal
    VkDescriptorSetLayoutBinding bindings[4] = {};

    // 0: Acceleration structure
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // 1: Shadow mask output (storage image)
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // 2: GBuffer position (sampled image)
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // 3: GBuffer normal (sampled image)
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
        return false;

    // Pool
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        return false;

    // Allocate set
    VkDescriptorSetAllocateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool = m_descriptorPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &m_descriptorSetLayout;

    return vkAllocateDescriptorSets(m_device, &setInfo, &m_descriptorSet) == VK_SUCCESS;
}

bool RTShadowTechnique::createRTPipeline() {
    // Load shader SPVs
    auto rgenCode = readFile("bin/shaders/rt_rt_shadow.rgen.spv");
    auto rmissCode = readFile("bin/shaders/rt_rt_shadow.rmiss.spv");
    auto rahitCode = readFile("bin/shaders/rt_rt_shadow.rahit.spv");

    if (rgenCode.empty() || rmissCode.empty() || rahitCode.empty()) {
        std::cerr << "[RTShadow] Failed to load RT shader SPVs" << std::endl;
        return false;
    }

    // Create shader modules
    auto createModule = [&](const std::vector<char>& code) -> VkShaderModule {
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.size();
        ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule mod;
        vkCreateShaderModule(m_device, &ci, nullptr, &mod);
        return mod;
    };

    VkShaderModule rgenModule = createModule(rgenCode);
    VkShaderModule rmissModule = createModule(rmissCode);
    VkShaderModule rahitModule = createModule(rahitCode);

    // Shader stages
    VkPipelineShaderStageCreateInfo stages[3] = {};

    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[0].module = rgenModule;
    stages[0].pName = "main";

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[1].module = rmissModule;
    stages[1].pName = "main";

    stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    stages[2].module = rahitModule;
    stages[2].pName = "main";

    // Shader groups
    VkRayTracingShaderGroupCreateInfoKHR groups[3] = {};

    // Group 0: Ray generation
    groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;  // rgen stage index
    groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Group 1: Miss
    groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;  // miss stage index
    groups[1].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Group 2: Hit (any-hit for shadow termination)
    groups[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].generalShader = VK_SHADER_UNUSED_KHR;
    groups[2].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[2].anyHitShader = 2;   // rahit stage index
    groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Pipeline layout with push constants
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    pushRange.offset = 0;
    pushRange.size = sizeof(ShadowPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        std::cerr << "[RTShadow] Failed to create pipeline layout" << std::endl;
        return false;
    }

    // Create RT pipeline
    VkRayTracingPipelineCreateInfoKHR pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipelineInfo.stageCount = 3;
    pipelineInfo.pStages = stages;
    pipelineInfo.groupCount = 3;
    pipelineInfo.pGroups = groups;
    pipelineInfo.maxPipelineRayRecursionDepth = 1;  // shadow rays don't recurse
    pipelineInfo.layout = m_pipelineLayout;

    VkResult result = vkCreateRayTracingPipelinesKHR(
        m_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_rtPipeline);

    // Cleanup shader modules
    vkDestroyShaderModule(m_device, rgenModule, nullptr);
    vkDestroyShaderModule(m_device, rmissModule, nullptr);
    vkDestroyShaderModule(m_device, rahitModule, nullptr);

    if (result != VK_SUCCESS) {
        std::cerr << "[RTShadow] Failed to create RT pipeline (err=" << result << ")" << std::endl;
        return false;
    }

    std::cout << "[RTShadow] RT pipeline created" << std::endl;
    return true;
}

bool RTShadowTechnique::createShaderBindingTable() {
    // Query RT pipeline properties for handle size/alignment
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
    rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &rtProps;
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &props2);

    uint32_t handleSize = rtProps.shaderGroupHandleSize;
    uint32_t handleAlignment = rtProps.shaderGroupHandleAlignment;
    uint32_t baseAlignment = rtProps.shaderGroupBaseAlignment;

    // Aligned handle size (each entry in the SBT must be aligned)
    uint32_t handleSizeAligned = (handleSize + handleAlignment - 1) & ~(handleAlignment - 1);

    // 3 groups: rgen, miss, hit
    uint32_t groupCount = 3;
    uint32_t sbtSize = groupCount * handleSizeAligned;

    // Get shader group handles from the pipeline
    std::vector<uint8_t> handles(groupCount * handleSize);
    if (vkGetRayTracingShaderGroupHandlesKHR(m_device, m_rtPipeline, 0, groupCount,
                                              handles.size(), handles.data()) != VK_SUCCESS) {
        std::cerr << "[RTShadow] Failed to get shader group handles" << std::endl;
        return false;
    }

    // Create SBT buffer
    // Each region must be baseAlignment-aligned
    uint32_t rgenSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t missSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t hitSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t totalSize = rgenSize + missSize + hitSize;

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = totalSize;
    bufInfo.usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &m_sbtBuffer) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, m_sbtBuffer, &memReqs);

    VkMemoryAllocateFlagsInfo allocFlags{};
    allocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    allocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &allocFlags;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_sbtMemory) != VK_SUCCESS) return false;
    vkBindBufferMemory(m_device, m_sbtBuffer, m_sbtMemory, 0);

    // Write handles into SBT buffer
    void* mapped;
    vkMapMemory(m_device, m_sbtMemory, 0, totalSize, 0, &mapped);
    memset(mapped, 0, totalSize);

    uint8_t* dst = static_cast<uint8_t*>(mapped);
    // Rgen at offset 0
    memcpy(dst, handles.data() + 0 * handleSize, handleSize);
    // Miss at offset rgenSize
    memcpy(dst + rgenSize, handles.data() + 1 * handleSize, handleSize);
    // Hit at offset rgenSize + missSize
    memcpy(dst + rgenSize + missSize, handles.data() + 2 * handleSize, handleSize);

    vkUnmapMemory(m_device, m_sbtMemory);

    // Set up strided device address regions
    VkDeviceAddress sbtAddr = getBufferDeviceAddress(m_sbtBuffer);

    m_rgenRegion.deviceAddress = sbtAddr;
    m_rgenRegion.stride = handleSizeAligned;
    m_rgenRegion.size = rgenSize;

    m_missRegion.deviceAddress = sbtAddr + rgenSize;
    m_missRegion.stride = handleSizeAligned;
    m_missRegion.size = missSize;

    m_hitRegion.deviceAddress = sbtAddr + rgenSize + missSize;
    m_hitRegion.stride = handleSizeAligned;
    m_hitRegion.size = hitSize;

    m_callRegion = {};  // not used

    std::cout << "[RTShadow] SBT created (handleSize=" << handleSize
              << ", aligned=" << handleSizeAligned << ")" << std::endl;
    return true;
}

void RTShadowTechnique::render(VkCommandBuffer cmd, const ShadowInput& input) {
    if (!m_rtPipeline || !input.accel || !input.accel->getTLAS()) return;

    // Update descriptor set with current frame's data
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    VkAccelerationStructureKHR tlas = input.accel->getTLAS();
    asWrite.pAccelerationStructures = &tlas;

    VkDescriptorImageInfo shadowMaskInfo{};
    shadowMaskInfo.imageView = m_shadowMaskView;
    shadowMaskInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Create a sampler for GBuffer reads
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler;
    vkCreateSampler(m_device, &samplerInfo, nullptr, &sampler);

    VkDescriptorImageInfo posInfo{};
    posInfo.imageView = input.positionBuffer;
    posInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    posInfo.sampler = sampler;

    VkDescriptorImageInfo normalInfo{};
    normalInfo.imageView = input.normalBuffer;
    normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    normalInfo.sampler = sampler;

    VkWriteDescriptorSet writes[4] = {};

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
    writes[1].pImageInfo = &shadowMaskInfo;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_descriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &posInfo;

    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = m_descriptorSet;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].pImageInfo = &normalInfo;

    vkUpdateDescriptorSets(m_device, 4, writes, 0, nullptr);

    // Transition shadow mask to GENERAL for storage image write
    VkImageMemoryBarrier toGeneral{};
    toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGeneral.srcAccessMask = 0;
    toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.image = m_shadowMask;
    toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0, 0, nullptr, 0, nullptr, 1, &toGeneral);

    // Bind RT pipeline and descriptors
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_rtPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    // Push constants
    ShadowPushConstants pc{};
    pc.invView = glm::inverse(input.view);
    pc.invProj = glm::inverse(input.proj);
    pc.lightDirAndRadius = glm::vec4(input.lightDirection, m_lightRadius);
    pc.lightPosAndRange = glm::vec4(input.lightPosition, input.lightRange);
    pc.params = glm::uvec4(input.width, input.height, input.lightType, m_sampleCount);

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
                       0, sizeof(ShadowPushConstants), &pc);

    // Trace rays!
    vkCmdTraceRaysKHR(cmd, &m_rgenRegion, &m_missRegion, &m_hitRegion, &m_callRegion,
                      input.width, input.height, 1);

    // Transition shadow mask to SHADER_READ_ONLY for the lighting pass to sample
    VkImageMemoryBarrier toRead{};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.image = m_shadowMask;
    toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toRead);

    // Cleanup temp sampler
    vkDestroySampler(m_device, sampler, nullptr);
}

void RTShadowTechnique::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;

    // Recreate output image
    if (m_shadowMaskView) { vkDestroyImageView(m_device, m_shadowMaskView, nullptr); m_shadowMaskView = VK_NULL_HANDLE; }
    if (m_shadowMask) { vkDestroyImage(m_device, m_shadowMask, nullptr); m_shadowMask = VK_NULL_HANDLE; }
    if (m_shadowMaskMemory) { vkFreeMemory(m_device, m_shadowMaskMemory, nullptr); m_shadowMaskMemory = VK_NULL_HANDLE; }

    createOutputImage();
}

ShadowOutput RTShadowTechnique::getOutput() const {
    return {m_shadowMask, m_shadowMaskView};
}

void RTShadowTechnique::destroy() {
    if (!m_device) return;
    vkDeviceWaitIdle(m_device);

    if (m_sbtBuffer) { vkDestroyBuffer(m_device, m_sbtBuffer, nullptr); m_sbtBuffer = VK_NULL_HANDLE; }
    if (m_sbtMemory) { vkFreeMemory(m_device, m_sbtMemory, nullptr); m_sbtMemory = VK_NULL_HANDLE; }
    if (m_rtPipeline) { vkDestroyPipeline(m_device, m_rtPipeline, nullptr); m_rtPipeline = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }
    if (m_descriptorPool) { vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr); m_descriptorPool = VK_NULL_HANDLE; }
    if (m_descriptorSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr); m_descriptorSetLayout = VK_NULL_HANDLE; }
    if (m_shadowMaskView) { vkDestroyImageView(m_device, m_shadowMaskView, nullptr); m_shadowMaskView = VK_NULL_HANDLE; }
    if (m_shadowMask) { vkDestroyImage(m_device, m_shadowMask, nullptr); m_shadowMask = VK_NULL_HANDLE; }
    if (m_shadowMaskMemory) { vkFreeMemory(m_device, m_shadowMaskMemory, nullptr); m_shadowMaskMemory = VK_NULL_HANDLE; }

    m_descriptorSet = VK_NULL_HANDLE;
}

} // namespace ohao
