#include "rt_gi_technique.hpp"
#include <fstream>
#include <cstring>
#include <algorithm>

namespace ohao {

RTGITechnique::~RTGITechnique() {
    destroy();
}

bool RTGITechnique::loadFunctionPointers() {
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

uint32_t RTGITechnique::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return UINT32_MAX;
}

VkDeviceAddress RTGITechnique::getBufferDeviceAddress(VkBuffer buffer) {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddressFn(m_device, &info);
}

static std::vector<char> readFile(const std::string& path) {
    std::vector<std::string> searchPaths = {
        path,
        "build/shaders/" + path.substr(path.find_last_of("/\\") + 1),
        "build/Release/bin/shaders/" + path.substr(path.find_last_of("/\\") + 1),
    };
    for (const auto& p : searchPaths) {
        std::ifstream file(p, std::ios::ate | std::ios::binary);
        if (file.is_open()) {
            size_t size = (size_t)file.tellg();
            std::vector<char> buffer(size);
            file.seekg(0);
            file.read(buffer.data(), size);
            return buffer;
        }
    }
    return {};
}

bool RTGITechnique::init(VkDevice device, VkPhysicalDevice physicalDevice,
                          uint32_t width, uint32_t height) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_width = width;
    m_height = height;

    if (!loadFunctionPointers()) {
        std::cerr << "[RTGI] Failed to load RT function pointers" << std::endl;
        return false;
    }

    if (!createOutputImage()) {
        std::cerr << "[RTGI] Failed to create output image" << std::endl;
        return false;
    }

    if (!createMaterialBuffer()) {
        std::cerr << "[RTGI] Failed to create material buffer" << std::endl;
        return false;
    }

    if (!createDescriptorResources()) {
        std::cerr << "[RTGI] Failed to create descriptor resources" << std::endl;
        return false;
    }

    if (!createRTPipeline()) {
        std::cerr << "[RTGI] Failed to create RT pipeline" << std::endl;
        return false;
    }

    if (!createShaderBindingTable()) {
        std::cerr << "[RTGI] Failed to create SBT" << std::endl;
        return false;
    }

    std::cout << "[RTGI] Initialized (" << width << "x" << height << ")" << std::endl;
    return true;
}

bool RTGITechnique::createOutputImage() {
    // GI output: RGBA16F for HDR indirect light
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.extent = {m_width, m_height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_giOutput) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_device, m_giOutput, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_giOutputMemory) != VK_SUCCESS) return false;
    vkBindImageMemory(m_device, m_giOutput, m_giOutputMemory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_giOutput;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_giOutputView) != VK_SUCCESS)
        return false;

    // History buffer (same format, used as sampler for temporal blend)
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_giHistory) != VK_SUCCESS) return false;
    vkGetImageMemoryRequirements(m_device, m_giHistory, &memReqs);
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_giHistoryMemory) != VK_SUCCESS) return false;
    vkBindImageMemory(m_device, m_giHistory, m_giHistoryMemory, 0);
    viewInfo.image = m_giHistory;
    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_giHistoryView) != VK_SUCCESS) return false;

    return true;
}

bool RTGITechnique::createMaterialBuffer() {
    // Pre-allocate material buffer for up to 256 instances (vec4 each)
    // HOST_VISIBLE so CPU can write albedo colors directly
    const uint32_t maxInstances = 256;
    m_materialData.resize(maxInstances, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f));

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = maxInstances * sizeof(glm::vec4);
    bufInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &m_materialBuffer) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, m_materialBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_materialMemory) != VK_SUCCESS) return false;
    vkBindBufferMemory(m_device, m_materialBuffer, m_materialMemory, 0);

    // Write default albedos
    void* mapped;
    vkMapMemory(m_device, m_materialMemory, 0, maxInstances * sizeof(glm::vec4), 0, &mapped);
    memcpy(mapped, m_materialData.data(), maxInstances * sizeof(glm::vec4));
    vkUnmapMemory(m_device, m_materialMemory);

    return true;
}

void RTGITechnique::setMaterialAlbedos(const std::vector<glm::vec3>& albedos,
                                        const std::vector<float>& flags) {
    for (size_t i = 0; i < albedos.size() && i < m_materialData.size(); i++) {
        float alpha = (i < flags.size()) ? flags[i] : 1.0f;
        m_materialData[i] = glm::vec4(albedos[i], alpha);
    }

    // Upload to GPU
    void* mapped;
    VkDeviceSize size = m_materialData.size() * sizeof(glm::vec4);
    vkMapMemory(m_device, m_materialMemory, 0, size, 0, &mapped);
    memcpy(mapped, m_materialData.data(), size);
    vkUnmapMemory(m_device, m_materialMemory);
}

bool RTGITechnique::createDescriptorResources() {
    // Layout: 8 bindings
    //   0: TLAS (acceleration structure) — RAYGEN
    //   1: GI output (storage image) — RAYGEN
    //   2: GBuffer position (sampled image) — RAYGEN
    //   3: GBuffer normal (sampled image) — RAYGEN
    //   4: GBuffer albedo (sampled image) — RAYGEN
    //   5: Direct lighting (sampled image) — RAYGEN
    //   6: Material buffer SSBO — CLOSEST_HIT
    //   7: GI history (sampled image) — RAYGEN (temporal blend)
    VkDescriptorSetLayoutBinding bindings[8] = {};

    // 0: Acceleration structure
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // 1: GI output (storage image)
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

    // 4: GBuffer albedo (sampled image)
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // 5: Direct lighting (sampled image)
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // 6: Material buffer SSBO (per-instance albedo for closest-hit)
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    // 7: GI history (sampled image for temporal accumulation)
    bindings[7].binding = 7;
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 8;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
        return false;

    // Pool
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5},  // 4 gbuffer + 1 history
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 4;
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

bool RTGITechnique::createRTPipeline() {
    // Load shader SPVs
    auto rgenCode = readFile("bin/shaders/rt_rt_gi.rgen.spv");
    auto rmissCode = readFile("bin/shaders/rt_rt_gi.rmiss.spv");
    auto rchitCode = readFile("bin/shaders/rt_rt_gi.rchit.spv");

    if (rgenCode.empty() || rmissCode.empty() || rchitCode.empty()) {
        std::cerr << "[RTGI] Failed to load RT shader SPVs" << std::endl;
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
    VkShaderModule rchitModule = createModule(rchitCode);

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
    stages[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[2].module = rchitModule;
    stages[2].pName = "main";

    // Shader groups
    VkRayTracingShaderGroupCreateInfoKHR groups[3] = {};

    // Group 0: Ray generation (GENERAL)
    groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;  // rgen stage index
    groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Group 1: Miss (GENERAL)
    groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;  // miss stage index
    groups[1].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Group 2: Closest-hit (TRIANGLES_HIT_GROUP)
    groups[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].generalShader = VK_SHADER_UNUSED_KHR;
    groups[2].closestHitShader = 2;   // rchit stage index
    groups[2].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Pipeline layout with push constants
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    pushRange.offset = 0;
    pushRange.size = sizeof(GIPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        std::cerr << "[RTGI] Failed to create pipeline layout" << std::endl;
        return false;
    }

    // Create RT pipeline
    VkRayTracingPipelineCreateInfoKHR pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipelineInfo.stageCount = 3;
    pipelineInfo.pStages = stages;
    pipelineInfo.groupCount = 3;
    pipelineInfo.pGroups = groups;
    pipelineInfo.maxPipelineRayRecursionDepth = 1;  // 1-bounce GI
    pipelineInfo.layout = m_pipelineLayout;

    VkResult result = vkCreateRayTracingPipelinesKHR(
        m_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_rtPipeline);

    // Cleanup shader modules
    vkDestroyShaderModule(m_device, rgenModule, nullptr);
    vkDestroyShaderModule(m_device, rmissModule, nullptr);
    vkDestroyShaderModule(m_device, rchitModule, nullptr);

    if (result != VK_SUCCESS) {
        std::cerr << "[RTGI] Failed to create RT pipeline (err=" << result << ")" << std::endl;
        return false;
    }

    std::cout << "[RTGI] RT pipeline created" << std::endl;
    return true;
}

bool RTGITechnique::createShaderBindingTable() {
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

    // Get shader group handles from the pipeline
    std::vector<uint8_t> handles(groupCount * handleSize);
    if (vkGetRayTracingShaderGroupHandlesKHR(m_device, m_rtPipeline, 0, groupCount,
                                              handles.size(), handles.data()) != VK_SUCCESS) {
        std::cerr << "[RTGI] Failed to get shader group handles" << std::endl;
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

    std::cout << "[RTGI] SBT created (handleSize=" << handleSize
              << ", aligned=" << handleSizeAligned << ")" << std::endl;
    return true;
}

void RTGITechnique::render(VkCommandBuffer cmd, const GIInput& input) {
    if (!m_rtPipeline || !input.accel || !input.accel->getTLAS()) return;

    // Update descriptor set with current frame's data
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    VkAccelerationStructureKHR tlas = input.accel->getTLAS();
    asWrite.pAccelerationStructures = &tlas;

    VkDescriptorImageInfo giOutputInfo{};
    giOutputInfo.imageView = m_giOutputView;
    giOutputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

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

    VkDescriptorImageInfo albedoInfo{};
    albedoInfo.imageView = input.albedoBuffer;
    albedoInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    albedoInfo.sampler = sampler;

    VkDescriptorImageInfo directLightInfo{};
    directLightInfo.imageView = input.depthBuffer;
    directLightInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    directLightInfo.sampler = sampler;

    VkDescriptorBufferInfo materialBufInfo{};
    materialBufInfo.buffer = m_materialBuffer;
    materialBufInfo.offset = 0;
    materialBufInfo.range = m_materialData.size() * sizeof(glm::vec4);

    VkDescriptorImageInfo historyInfo{};
    historyInfo.imageView = m_giHistoryView;
    historyInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    historyInfo.sampler = sampler;

    VkWriteDescriptorSet writes[8] = {};

    // 0: Acceleration structure
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[0].pNext = &asWrite;

    // 1: GI output (storage image)
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &giOutputInfo;

    // 2: GBuffer position
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_descriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &posInfo;

    // 3: GBuffer normal
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = m_descriptorSet;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].pImageInfo = &normalInfo;

    // 4: GBuffer albedo
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = m_descriptorSet;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[4].pImageInfo = &albedoInfo;

    // 5: Direct lighting
    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = m_descriptorSet;
    writes[5].dstBinding = 5;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[5].pImageInfo = &directLightInfo;

    // 6: Material buffer SSBO
    writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = m_descriptorSet;
    writes[6].dstBinding = 6;
    writes[6].descriptorCount = 1;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[6].pBufferInfo = &materialBufInfo;

    // 7: GI history (sampled image for temporal blend)
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = m_descriptorSet;
    writes[7].dstBinding = 7;
    writes[7].descriptorCount = 1;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[7].pImageInfo = &historyInfo;

    vkUpdateDescriptorSets(m_device, 8, writes, 0, nullptr);

    // Transition GI output to GENERAL, history to SHADER_READ_ONLY
    VkImageMemoryBarrier barriers[2] = {};
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].image = m_giOutput;
    barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[1].oldLayout = (m_frameIndex == 0) ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[1].image = m_giHistory;
    barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0, 0, nullptr, 0, nullptr, 2, barriers);

    // Bind RT pipeline and descriptors
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_rtPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    // Push constants
    GIPushConstants pc{};
    pc.invView = glm::inverse(input.view);
    pc.invProj = glm::inverse(input.proj);
    pc.lightPosAndIntensity = glm::vec4(input.lightPos, input.lightIntensity);
    pc.params = glm::uvec4(input.width, input.height, m_sampleCount, m_frameIndex);

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
                       0, sizeof(GIPushConstants), &pc);

    // Trace rays!
    vkCmdTraceRaysKHR(cmd, &m_rgenRegion, &m_missRegion, &m_hitRegion, &m_callRegion,
                      input.width, input.height, 1);

    // Copy output → history for next frame's temporal blend
    // Transition output: GENERAL → TRANSFER_SRC, history: SHADER_READ → TRANSFER_DST
    VkImageMemoryBarrier copyBarriers[2] = {};
    copyBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    copyBarriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    copyBarriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    copyBarriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    copyBarriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    copyBarriers[0].image = m_giOutput;
    copyBarriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    copyBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    copyBarriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    copyBarriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    copyBarriers[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    copyBarriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    copyBarriers[1].image = m_giHistory;
    copyBarriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 2, copyBarriers);

    VkImageCopy copyRegion{};
    copyRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.extent = {input.width, input.height, 1};
    vkCmdCopyImage(cmd, m_giOutput, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   m_giHistory, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    // Transition output: TRANSFER_SRC → SHADER_READ (for lighting pass)
    VkImageMemoryBarrier toRead{};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.image = m_giOutput;
    toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toRead);

    // Cleanup temp sampler
    vkDestroySampler(m_device, sampler, nullptr);

    m_frameIndex++;
}

void RTGITechnique::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;

    // Recreate output image
    if (m_giOutputView) { vkDestroyImageView(m_device, m_giOutputView, nullptr); m_giOutputView = VK_NULL_HANDLE; }
    if (m_giOutput) { vkDestroyImage(m_device, m_giOutput, nullptr); m_giOutput = VK_NULL_HANDLE; }
    if (m_giOutputMemory) { vkFreeMemory(m_device, m_giOutputMemory, nullptr); m_giOutputMemory = VK_NULL_HANDLE; }

    createOutputImage();
}

GIOutput RTGITechnique::getOutput() const {
    return {m_giOutput, m_giOutputView};
}

void RTGITechnique::destroy() {
    if (!m_device) return;
    vkDeviceWaitIdle(m_device);

    if (m_sbtBuffer) { vkDestroyBuffer(m_device, m_sbtBuffer, nullptr); m_sbtBuffer = VK_NULL_HANDLE; }
    if (m_sbtMemory) { vkFreeMemory(m_device, m_sbtMemory, nullptr); m_sbtMemory = VK_NULL_HANDLE; }
    if (m_rtPipeline) { vkDestroyPipeline(m_device, m_rtPipeline, nullptr); m_rtPipeline = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }
    if (m_descriptorPool) { vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr); m_descriptorPool = VK_NULL_HANDLE; }
    if (m_descriptorSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr); m_descriptorSetLayout = VK_NULL_HANDLE; }
    if (m_giOutputView) { vkDestroyImageView(m_device, m_giOutputView, nullptr); m_giOutputView = VK_NULL_HANDLE; }
    if (m_giOutput) { vkDestroyImage(m_device, m_giOutput, nullptr); m_giOutput = VK_NULL_HANDLE; }
    if (m_giOutputMemory) { vkFreeMemory(m_device, m_giOutputMemory, nullptr); m_giOutputMemory = VK_NULL_HANDLE; }
    if (m_giHistoryView) { vkDestroyImageView(m_device, m_giHistoryView, nullptr); m_giHistoryView = VK_NULL_HANDLE; }
    if (m_giHistory) { vkDestroyImage(m_device, m_giHistory, nullptr); m_giHistory = VK_NULL_HANDLE; }
    if (m_giHistoryMemory) { vkFreeMemory(m_device, m_giHistoryMemory, nullptr); m_giHistoryMemory = VK_NULL_HANDLE; }
    if (m_materialBuffer) { vkDestroyBuffer(m_device, m_materialBuffer, nullptr); m_materialBuffer = VK_NULL_HANDLE; }
    if (m_materialMemory) { vkFreeMemory(m_device, m_materialMemory, nullptr); m_materialMemory = VK_NULL_HANDLE; }

    m_descriptorSet = VK_NULL_HANDLE;
}

} // namespace ohao
