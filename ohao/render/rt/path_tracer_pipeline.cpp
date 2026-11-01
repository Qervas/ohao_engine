// Extracted from path_tracer.cpp — RT pipeline + shader binding table creation.
// Kept as members of class PathTracer; no behavior change.
// The file-local readFile() helper is relocated here since this TU is its only caller.

#include "path_tracer.hpp"
#include "pipeline_disk_cache.hpp"
#include "render/rt/rt_compute.hpp"

#include <cstring>
#include <fstream>
#include <vector>
#include "core/log.hpp"

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
    auto cacheCode = readFile(m_shaderSet.cacheUpdateSpv);

    if (rgenCode.empty() || rmissCode.empty() || rchitCode.empty() || cacheCode.empty()) {
        OHAO_LOG(Error, render, "PathTracer: failed to load RT shader SPVs");
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
    VkShaderModule cacheModule = createModule(cacheCode);

    // Shader stages: 0=raygen, 1=miss, 2=closest-hit, 3=any-hit (optional), then the radiance
    // cache's update raygen.
    const uint32_t cacheStage = hasAnyHit ? 4 : 3;
    uint32_t stageCount = cacheStage + 1;
    VkPipelineShaderStageCreateInfo stages[5] = {};

    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[0].module = rgenModule;
    stages[0].pName = m_shaderSet.raygenEntry;


    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[1].module = rmissModule;
    stages[1].pName = m_shaderSet.missEntry;

    stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[2].module = rchitModule;
    stages[2].pName = m_shaderSet.closestHitEntry;

    if (hasAnyHit) {
        stages[3].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[3].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
        stages[3].module = rahitModule;
        stages[3].pName = m_shaderSet.anyHitEntry;
    }
    stages[cacheStage].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[cacheStage].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[cacheStage].module = cacheModule;
    stages[cacheStage].pName = m_shaderSet.cacheUpdateEntry;

    // Shader groups: raygen, miss, the hit group (closest-hit + any-hit), the cache's update raygen
    VkRayTracingShaderGroupCreateInfoKHR groups[4] = {};

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

    groups[3].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[3].generalShader = cacheStage;
    groups[3].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[3].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[3].intersectionShader = VK_SHADER_UNUSED_KHR;

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
        OHAO_LOG(Error, render, "PathTracer: failed to create pipeline layout");
        vkDestroyShaderModule(m_device, rgenModule, nullptr);
        vkDestroyShaderModule(m_device, rmissModule, nullptr);
        vkDestroyShaderModule(m_device, rchitModule, nullptr);
        if (rahitModule) vkDestroyShaderModule(m_device, rahitModule, nullptr);
        vkDestroyShaderModule(m_device, cacheModule, nullptr);
        return false;
    }

    // Create RT pipeline
    VkRayTracingPipelineCreateInfoKHR pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipelineInfo.stageCount = stageCount;
    pipelineInfo.pStages = stages;
    pipelineInfo.groupCount = 4;
    pipelineInfo.pGroups = groups;
    pipelineInfo.maxPipelineRayRecursionDepth = 2;  // path tracing needs bounce recursion
    pipelineInfo.layout = m_pipelineLayout;
    if (rtc::pipelineStats()) pipelineInfo.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;

    // Minutes cold: kept in a file so the next process (a headless host beside a window) loads it.
    PipelineDiskCache cache;
    cache.open(m_device, m_physicalDevice, "path_tracer");
    VkResult result = vkCreateRayTracingPipelinesKHR(
        m_device, VK_NULL_HANDLE, cache.handle(), 1, &pipelineInfo, nullptr, &m_rtPipeline);
    if (result == VK_SUCCESS) cache.save();

    // Cleanup shader modules (no longer needed after pipeline creation)
    vkDestroyShaderModule(m_device, rgenModule, nullptr);
    vkDestroyShaderModule(m_device, rmissModule, nullptr);
    vkDestroyShaderModule(m_device, rchitModule, nullptr);
    if (rahitModule) vkDestroyShaderModule(m_device, rahitModule, nullptr);
    vkDestroyShaderModule(m_device, cacheModule, nullptr);

    if (result != VK_SUCCESS) {
        OHAO_LOG(Error, render, "PathTracer: failed to create RT pipeline (err={})", static_cast<int>(result));
        return false;
    }

    if (rtc::pipelineStats()) {
        // The driver may report no executables for a ray tracing pipeline (NVIDIA's does not); the
        // raygen group's stack is what raygen's live state spills across each trace.
        rtc::logPipelineStats(m_device, m_rtPipeline, "path_tracer");
        const auto stackSize = reinterpret_cast<PFN_vkGetRayTracingShaderGroupStackSizeKHR>(
            vkGetDeviceProcAddr(m_device, "vkGetRayTracingShaderGroupStackSizeKHR"));
        const VkDeviceSize raygenStack =
            stackSize ? stackSize(m_device, m_rtPipeline, 0, VK_SHADER_GROUP_SHADER_GENERAL_KHR) : 0;
        OHAO_LOG(Info, render, "pipeline stats path_tracer: raygen stack {} bytes", raygenStack);
    }
    OHAO_LOG(Debug, render, "PathTracer: RT pipeline created");
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

    // 4 groups: rgen, miss, closest-hit, the cache's update rgen
    uint32_t groupCount = 4;

    // Get shader group handles from the pipeline
    std::vector<uint8_t> handles(groupCount * handleSize);
    if (vkGetRayTracingShaderGroupHandlesKHR(m_device, m_rtPipeline, 0, groupCount,
                                              handles.size(), handles.data()) != VK_SUCCESS) {
        OHAO_LOG(Error, render, "PathTracer: failed to get shader group handles");
        return false;
    }

    // Each region must be baseAlignment-aligned
    uint32_t rgenSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t missSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t hitSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t totalSize = rgenSize + missSize + hitSize + rgenSize;   // the cache's update rgen last

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
    // The cache's update raygen after the hit group
    memcpy(dst + rgenSize + missSize + hitSize, handles.data() + 3 * handleSize, handleSize);

    vkUnmapMemory(m_device, m_sbtMemory);

    // Set up strided device address regions
    VkDeviceAddress sbtAddr = getBufferDeviceAddress(m_sbtBuffer);

    m_rgenRegion.deviceAddress = sbtAddr;
    // A raygen region holds exactly one record, and the spec requires its size
    // to equal its stride (VUID-vkCmdTraceRaysKHR-size-04023).
    m_rgenRegion.stride = rgenSize;
    m_rgenRegion.size = rgenSize;

    m_missRegion.deviceAddress = sbtAddr + rgenSize;
    m_missRegion.stride = handleSizeAligned;
    m_missRegion.size = missSize;

    m_hitRegion.deviceAddress = sbtAddr + rgenSize + missSize;
    m_hitRegion.stride = handleSizeAligned;
    m_hitRegion.size = hitSize;

    m_cacheRgenRegion.deviceAddress = sbtAddr + rgenSize + missSize + hitSize;
    m_cacheRgenRegion.stride = rgenSize;
    m_cacheRgenRegion.size = rgenSize;

    m_callRegion = {};  // not used

    OHAO_LOG(Debug, render, "PathTracer: SBT created (handleSize={}, aligned={})",
             handleSize, handleSizeAligned);
    return true;
}

} // namespace ohao
