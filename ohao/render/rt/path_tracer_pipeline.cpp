// Extracted from path_tracer.cpp — RT pipeline + shader binding table creation.
// Kept as members of class PathTracer; no behavior change.
// The file-local readFile() helper is relocated here since this TU is its only caller.

#include "path_tracer.hpp"

#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

namespace ohao {

namespace {
static std::vector<char> readFile(const std::string& path) {
    // Try multiple search paths for shader SPVs
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
}  // namespace

bool PathTracer::createRTPipeline() {
    // Load shader SPVs
    auto rgenCode = readFile(m_shaderSet.raygenSpv);
    auto rmissCode = readFile(m_shaderSet.missSpv);
    auto rchitCode = readFile(m_shaderSet.closestHitSpv);
    auto rahitCode = readFile(m_shaderSet.anyHitSpv);

    if (rgenCode.empty() || rmissCode.empty() || rchitCode.empty()) {
        std::cerr << "[PathTracer] Failed to load RT shader SPVs" << std::endl;
        return false;
    }
    bool hasAnyHit = !rahitCode.empty();

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
    VkShaderModule rahitModule = hasAnyHit ? createModule(rahitCode) : VK_NULL_HANDLE;

    // Shader stages: 0=raygen, 1=miss, 2=closest-hit, 3=any-hit (optional)
    uint32_t stageCount = hasAnyHit ? 4 : 3;
    VkPipelineShaderStageCreateInfo stages[4] = {};

    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[0].module = rgenModule;
    stages[0].pName = "main";

    // Specialization constant: sampler type baked into the raygen SPIR-V
    // (see shaders/includes/rt/sampler_api.glsl, layout(constant_id=0)).
    VkSpecializationMapEntry samplerEntry{};
    samplerEntry.constantID = kSamplerSpecConstantId;
    samplerEntry.offset = 0;
    samplerEntry.size = sizeof(uint32_t);

    uint32_t samplerTypeVal = static_cast<uint32_t>(m_renderSettings.samplerType);

    VkSpecializationInfo samplerSpecInfo{};
    samplerSpecInfo.mapEntryCount = 1;
    samplerSpecInfo.pMapEntries = &samplerEntry;
    samplerSpecInfo.dataSize = sizeof(uint32_t);
    samplerSpecInfo.pData = &samplerTypeVal;

    stages[0].pSpecializationInfo = &samplerSpecInfo;

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[1].module = rmissModule;
    stages[1].pName = "main";

    stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[2].module = rchitModule;
    stages[2].pName = "main";

    if (hasAnyHit) {
        stages[3].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[3].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
        stages[3].module = rahitModule;
        stages[3].pName = "main";
    }

    // Shader groups: 3 groups (any-hit is part of the hit group, not a separate group)
    VkRayTracingShaderGroupCreateInfoKHR groups[3] = {};

    groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

    groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;
    groups[1].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

    // Group 2: Hit group — closest-hit + any-hit
    groups[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].generalShader = VK_SHADER_UNUSED_KHR;
    groups[2].closestHitShader = 2;
    groups[2].anyHitShader = hasAnyHit ? 3 : VK_SHADER_UNUSED_KHR;
    groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
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
        if (rahitModule) vkDestroyShaderModule(m_device, rahitModule, nullptr);
        return false;
    }

    // Create RT pipeline
    VkRayTracingPipelineCreateInfoKHR pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipelineInfo.stageCount = stageCount;
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
    if (rahitModule) vkDestroyShaderModule(m_device, rahitModule, nullptr);

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

} // namespace ohao
