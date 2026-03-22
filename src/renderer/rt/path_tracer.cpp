#include "path_tracer.hpp"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <iostream>

namespace ohao {

PathTracer::~PathTracer() {
    destroy();
}

// ─── Function pointer loading ─────────────────────────────────────────

bool PathTracer::loadFunctionPointers() {
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

// ─── Helpers ──────────────────────────────────────────────────────────

uint32_t PathTracer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return UINT32_MAX;
}

VkDeviceAddress PathTracer::getBufferDeviceAddress(VkBuffer buffer) {
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

// ─── Initialization ──────────────────────────────────────────────────

bool PathTracer::init(VkDevice device, VkPhysicalDevice physicalDevice,
                       uint32_t width, uint32_t height) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_width = width;
    m_height = height;
    m_frameIndex = 0;

    if (!loadFunctionPointers()) {
        std::cerr << "[PathTracer] Failed to load RT function pointers" << std::endl;
        return false;
    }

    if (!createImages()) {
        std::cerr << "[PathTracer] Failed to create output images" << std::endl;
        return false;
    }

    if (!createMaterialBuffer()) {
        std::cerr << "[PathTracer] Failed to create material buffer" << std::endl;
        return false;
    }

    if (!createDescriptorResources()) {
        std::cerr << "[PathTracer] Failed to create descriptor resources" << std::endl;
        return false;
    }

    if (!createRTPipeline()) {
        std::cerr << "[PathTracer] Failed to create RT pipeline" << std::endl;
        return false;
    }

    if (!createShaderBindingTable()) {
        std::cerr << "[PathTracer] Failed to create SBT" << std::endl;
        return false;
    }

    std::cout << "[PathTracer] Initialized (" << width << "x" << height << ")" << std::endl;
    return true;
}

// ─── Image creation ──────────────────────────────────────────────────

bool PathTracer::createImages() {
    // --- Accumulation buffer: RGBA32F for HDR accumulation across frames ---
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_accumBuffer) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_accumBuffer, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_accumMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_accumBuffer, m_accumMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_accumBuffer;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_accumView) != VK_SUCCESS) return false;
    }

    // --- Output image: RGBA8 for tonemapped final output ---
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_outputImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_outputImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_outputMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_outputImage, m_outputMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_outputImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_outputView) != VK_SUCCESS) return false;
    }

    return true;
}

void PathTracer::destroyImages() {
    if (m_accumView) { vkDestroyImageView(m_device, m_accumView, nullptr); m_accumView = VK_NULL_HANDLE; }
    if (m_accumBuffer) { vkDestroyImage(m_device, m_accumBuffer, nullptr); m_accumBuffer = VK_NULL_HANDLE; }
    if (m_accumMemory) { vkFreeMemory(m_device, m_accumMemory, nullptr); m_accumMemory = VK_NULL_HANDLE; }

    if (m_outputView) { vkDestroyImageView(m_device, m_outputView, nullptr); m_outputView = VK_NULL_HANDLE; }
    if (m_outputImage) { vkDestroyImage(m_device, m_outputImage, nullptr); m_outputImage = VK_NULL_HANDLE; }
    if (m_outputMemory) { vkFreeMemory(m_device, m_outputMemory, nullptr); m_outputMemory = VK_NULL_HANDLE; }
}

// ─── Material buffer ─────────────────────────────────────────────────

bool PathTracer::createMaterialBuffer() {
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

void PathTracer::setMaterialAlbedos(const std::vector<glm::vec3>& albedos) {
    for (size_t i = 0; i < albedos.size() && i < m_materialData.size(); i++) {
        m_materialData[i] = glm::vec4(albedos[i], 1.0f);
    }
    void* mapped;
    VkDeviceSize size = m_materialData.size() * sizeof(glm::vec4);
    vkMapMemory(m_device, m_materialMemory, 0, size, 0, &mapped);
    memcpy(mapped, m_materialData.data(), size);
    vkUnmapMemory(m_device, m_materialMemory);
}

void PathTracer::setMaterialData(const std::vector<glm::vec4>& materials) {
    for (size_t i = 0; i < materials.size() && i < m_materialData.size(); i++) {
        m_materialData[i] = materials[i];
    }
    void* mapped;
    VkDeviceSize size = m_materialData.size() * sizeof(glm::vec4);
    vkMapMemory(m_device, m_materialMemory, 0, size, 0, &mapped);
    memcpy(mapped, m_materialData.data(), size);
    vkUnmapMemory(m_device, m_materialMemory);
}

// ─── Descriptor resources ────────────────────────────────────────────

bool PathTracer::createDescriptorResources() {
    // Layout: 6 bindings
    //   0: TLAS (acceleration structure)           — RAYGEN
    //   1: Accumulation buffer (storage image)     — RAYGEN   (RGBA32F)
    //   2: Output image (storage image)            — RAYGEN   (RGBA8)
    //   3: Material buffer SSBO                    — RAYGEN + CLOSEST_HIT
    //   4: Normal buffer SSBO (vec4 per vertex)    — CLOSEST_HIT
    //   5: Index buffer SSBO (uint per index)      — CLOSEST_HIT
    VkDescriptorSetLayoutBinding bindings[6] = {};

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
    bindings[5].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 6;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
        return false;

    // Pool
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},  // material + normals + indices
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

// ─── RT Pipeline ─────────────────────────────────────────────────────

bool PathTracer::createRTPipeline() {
    // Load shader SPVs
    auto rgenCode = readFile("bin/shaders/rt_pt_raygen.rgen.spv");
    auto rmissCode = readFile("bin/shaders/rt_pt_miss.rmiss.spv");
    auto rchitCode = readFile("bin/shaders/rt_pt_closesthit.rchit.spv");

    if (rgenCode.empty() || rmissCode.empty() || rchitCode.empty()) {
        std::cerr << "[PathTracer] Failed to load RT shader SPVs" << std::endl;
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

    // Shader stages: 0=raygen, 1=miss, 2=closest-hit
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

    // Shader groups: 3 groups
    VkRayTracingShaderGroupCreateInfoKHR groups[3] = {};

    // Group 0: Ray generation (GENERAL, shader index 0)
    groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Group 1: Miss (GENERAL, shader index 1)
    groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;
    groups[1].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Group 2: Closest-hit (TRIANGLES_HIT_GROUP, closestHitShader index 2)
    groups[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].generalShader = VK_SHADER_UNUSED_KHR;
    groups[2].closestHitShader = 2;
    groups[2].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Pipeline layout with push constants
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    pushRange.offset = 0;
    pushRange.size = sizeof(PTPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        std::cerr << "[PathTracer] Failed to create pipeline layout" << std::endl;
        vkDestroyShaderModule(m_device, rgenModule, nullptr);
        vkDestroyShaderModule(m_device, rmissModule, nullptr);
        vkDestroyShaderModule(m_device, rchitModule, nullptr);
        return false;
    }

    // Create RT pipeline
    VkRayTracingPipelineCreateInfoKHR pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipelineInfo.stageCount = 3;
    pipelineInfo.pStages = stages;
    pipelineInfo.groupCount = 3;
    pipelineInfo.pGroups = groups;
    pipelineInfo.maxPipelineRayRecursionDepth = 2;  // path tracing needs bounce recursion
    pipelineInfo.layout = m_pipelineLayout;

    VkResult result = vkCreateRayTracingPipelinesKHR(
        m_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_rtPipeline);

    // Cleanup shader modules (no longer needed after pipeline creation)
    vkDestroyShaderModule(m_device, rgenModule, nullptr);
    vkDestroyShaderModule(m_device, rmissModule, nullptr);
    vkDestroyShaderModule(m_device, rchitModule, nullptr);

    if (result != VK_SUCCESS) {
        std::cerr << "[PathTracer] Failed to create RT pipeline (err=" << result << ")" << std::endl;
        return false;
    }

    std::cout << "[PathTracer] RT pipeline created" << std::endl;
    return true;
}

// ─── Shader Binding Table ────────────────────────────────────────────

bool PathTracer::createShaderBindingTable() {
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

    // 3 groups: rgen, miss, closest-hit
    uint32_t groupCount = 3;

    // Get shader group handles from the pipeline
    std::vector<uint8_t> handles(groupCount * handleSize);
    if (vkGetRayTracingShaderGroupHandlesKHR(m_device, m_rtPipeline, 0, groupCount,
                                              handles.size(), handles.data()) != VK_SUCCESS) {
        std::cerr << "[PathTracer] Failed to get shader group handles" << std::endl;
        return false;
    }

    // Each region must be baseAlignment-aligned
    uint32_t rgenSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t missSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t hitSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t totalSize = rgenSize + missSize + hitSize;

    // Create SBT buffer
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
    // Closest-hit at offset rgenSize + missSize
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

    std::cout << "[PathTracer] SBT created (handleSize=" << handleSize
              << ", aligned=" << handleSizeAligned << ")" << std::endl;
    return true;
}

// ─── Render ──────────────────────────────────────────────────────────

void PathTracer::render(VkCommandBuffer cmd, RTAccelerationStructure* accel,
                         const glm::mat4& view, const glm::mat4& proj,
                         const glm::vec3& lightPos, float lightIntensity,
                         const glm::vec3& lightColor, float lightRadius) {
    if (!m_rtPipeline || !accel || !accel->getTLAS()) return;

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

    VkWriteDescriptorSet writes[6] = {};

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

    vkUpdateDescriptorSets(m_device, 6, writes, 0, nullptr);

    // --- Transition accumulation buffer to GENERAL ---
    // On first frame (frameIndex==0), transition from UNDEFINED to clear it;
    // on subsequent frames, keep GENERAL (already there from last trace).
    {
        VkImageMemoryBarrier accumBarrier{};
        accumBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        accumBarrier.srcAccessMask = (m_frameIndex == 0) ? 0 : VK_ACCESS_SHADER_WRITE_BIT;
        accumBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        accumBarrier.oldLayout = (m_frameIndex == 0) ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        accumBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        accumBarrier.image = m_accumBuffer;
        accumBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            (m_frameIndex == 0) ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
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

    // --- Bind pipeline and descriptors ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_rtPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    // --- Push constants ---
    PTPushConstants pc{};
    pc.invView = glm::inverse(view);
    pc.invProj = glm::inverse(proj);
    pc.lightPosAndIntensity = glm::vec4(lightPos, lightIntensity);
    pc.lightColorAndRadius = glm::vec4(lightColor, lightRadius);
    static constexpr uint32_t PT_FLAG_DENOISE_MODE = (1u << 31);
    uint32_t frameIdxWithFlags = m_frameIndex;
    if (m_denoiseMode) frameIdxWithFlags |= PT_FLAG_DENOISE_MODE;
    pc.params = glm::uvec4(m_width, m_height, frameIdxWithFlags, m_maxBounces);

    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
                       0, sizeof(PTPushConstants), &pc);

    // --- Trace rays! ---
    vkCmdTraceRaysKHR(cmd, &m_rgenRegion, &m_missRegion, &m_hitRegion, &m_callRegion,
                      m_width, m_height, 1);

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

    // Increment frame for progressive accumulation
    m_frameIndex++;
}

// ─── Accumulation reset ──────────────────────────────────────────────

void PathTracer::resetAccumulation() {
    m_frameIndex = 0;
}

// ─── Resize ──────────────────────────────────────────────────────────

void PathTracer::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;

    // Recreate both images at new resolution
    destroyImages();
    createImages();

    // Reset accumulation since the buffer dimensions changed
    m_frameIndex = 0;
}

// ─── Cleanup ─────────────────────────────────────────────────────────

void PathTracer::destroy() {
    if (!m_device) return;
    vkDeviceWaitIdle(m_device);

    // SBT
    if (m_sbtBuffer) { vkDestroyBuffer(m_device, m_sbtBuffer, nullptr); m_sbtBuffer = VK_NULL_HANDLE; }
    if (m_sbtMemory) { vkFreeMemory(m_device, m_sbtMemory, nullptr); m_sbtMemory = VK_NULL_HANDLE; }

    // Pipeline
    if (m_rtPipeline) { vkDestroyPipeline(m_device, m_rtPipeline, nullptr); m_rtPipeline = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }

    // Descriptors
    if (m_descriptorPool) { vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr); m_descriptorPool = VK_NULL_HANDLE; }
    if (m_descriptorSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr); m_descriptorSetLayout = VK_NULL_HANDLE; }
    m_descriptorSet = VK_NULL_HANDLE;  // freed with pool

    // Material buffer
    if (m_materialBuffer) { vkDestroyBuffer(m_device, m_materialBuffer, nullptr); m_materialBuffer = VK_NULL_HANDLE; }
    if (m_materialMemory) { vkFreeMemory(m_device, m_materialMemory, nullptr); m_materialMemory = VK_NULL_HANDLE; }

    // Images
    destroyImages();
}

} // namespace ohao
