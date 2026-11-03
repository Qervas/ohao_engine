// The path tracer's ray tracing pipelines and their shader binding tables. Two programs share the
// layout, the miss and the hit group: the default one (raygen, then the radiance cache's update
// raygen) and caustics' (sub-project 24: raygenCaustics, then causticTrace), made the first time a
// frame needs it, so the default raygen carries none of caustics' code and compiles as before.

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
        return false;
    }
    m_rtPipeline = buildRtPipeline(m_shaderSet.raygenSpv, m_shaderSet.raygenEntry, m_shaderSet.cacheUpdateSpv,
                                   m_shaderSet.cacheUpdateEntry, "path_tracer");
    if (!m_rtPipeline) return false;
    OHAO_LOG(Debug, render, "PathTracer: RT pipeline created");
    return true;
}

VkPipeline PathTracer::buildRtPipeline(const char* rgenSpv, const char* rgenEntry, const char* extraSpv,
                                       const char* extraEntry, const char* cacheName) {
    auto rgenCode = readFile(rgenSpv);
    auto rmissCode = readFile(m_shaderSet.missSpv);
    auto rchitCode = readFile(m_shaderSet.closestHitSpv);
    auto rahitCode = readFile(m_shaderSet.anyHitSpv);
    auto extraCode = readFile(extraSpv);

    if (rgenCode.empty() || rmissCode.empty() || rchitCode.empty() || extraCode.empty()) {
        OHAO_LOG(Error, render, "PathTracer: failed to load RT shader SPVs ({})", cacheName);
        return VK_NULL_HANDLE;
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
    VkShaderModule extraModule = createModule(extraCode);

    // Shader stages: 0=raygen, 1=miss, 2=closest-hit, 3=any-hit (optional), then the second raygen.
    const uint32_t extraStage = hasAnyHit ? 4 : 3;
    uint32_t stageCount = extraStage + 1;
    VkPipelineShaderStageCreateInfo stages[5] = {};

    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[0].module = rgenModule;
    stages[0].pName = rgenEntry;

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
    stages[extraStage].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[extraStage].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[extraStage].module = extraModule;
    stages[extraStage].pName = extraEntry;

    // Shader groups: raygen, miss, the hit group (closest-hit + any-hit), the second raygen
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
    groups[3].generalShader = extraStage;
    groups[3].closestHitShader = VK_SHADER_UNUSED_KHR;
    groups[3].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[3].intersectionShader = VK_SHADER_UNUSED_KHR;

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
    cache.open(m_device, m_physicalDevice, cacheName);
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = vkCreateRayTracingPipelinesKHR(
        m_device, VK_NULL_HANDLE, cache.handle(), 1, &pipelineInfo, nullptr, &pipeline);
    if (result == VK_SUCCESS) cache.save();

    // Cleanup shader modules (no longer needed after pipeline creation)
    vkDestroyShaderModule(m_device, rgenModule, nullptr);
    vkDestroyShaderModule(m_device, rmissModule, nullptr);
    vkDestroyShaderModule(m_device, rchitModule, nullptr);
    if (rahitModule) vkDestroyShaderModule(m_device, rahitModule, nullptr);
    vkDestroyShaderModule(m_device, extraModule, nullptr);

    if (result != VK_SUCCESS) {
        OHAO_LOG(Error, render, "PathTracer: failed to create RT pipeline {} (err={})", cacheName, static_cast<int>(result));
        return VK_NULL_HANDLE;
    }

    if (rtc::pipelineStats()) {
        // The driver may report no executables for a ray tracing pipeline (NVIDIA's does not); the
        // raygen group's stack is what raygen's live state spills across each trace.
        rtc::logPipelineStats(m_device, pipeline, cacheName);
        const auto stackSize = reinterpret_cast<PFN_vkGetRayTracingShaderGroupStackSizeKHR>(
            vkGetDeviceProcAddr(m_device, "vkGetRayTracingShaderGroupStackSizeKHR"));
        const VkDeviceSize raygenStack =
            stackSize ? stackSize(m_device, pipeline, 0, VK_SHADER_GROUP_SHADER_GENERAL_KHR) : 0;
        const VkDeviceSize extraStack =
            stackSize ? stackSize(m_device, pipeline, 3, VK_SHADER_GROUP_SHADER_GENERAL_KHR) : 0;
        OHAO_LOG(Info, render, "pipeline stats {}: raygen stack {} bytes, second raygen {} bytes", cacheName,
                 raygenStack, extraStack);
    }
    return pipeline;
}

bool PathTracer::ensureCausticsPipeline() {
    if (m_causticsPipeline) return true;
    if (m_causticsPipelineFailed) return false;
    m_causticsPipeline = buildRtPipeline(m_shaderSet.raygenCausticsSpv, m_shaderSet.raygenCausticsEntry,
                                         m_shaderSet.causticTraceSpv, m_shaderSet.causticTraceEntry, "path_tracer_caustics");
    if (!m_causticsPipeline ||
        !buildShaderBindingTable(m_causticsPipeline, m_causticsSbtBuffer, m_causticsSbtMemory, m_causticsRgenRegion,
                                 m_causticsMissRegion, m_causticsHitRegion, m_causticTraceRegion)) {
        OHAO_LOG(Error, render, "PathTracer: caustics' pipeline unavailable: caustics stay off");
        m_causticsPipelineFailed = true;
        return false;
    }
    return true;
}

// ─── Shader Binding Table ────────────────────────────────────────────

bool PathTracer::createShaderBindingTable() {
    return buildShaderBindingTable(m_rtPipeline, m_sbtBuffer, m_sbtMemory, m_rgenRegion, m_missRegion, m_hitRegion,
                                   m_cacheRgenRegion);
}

bool PathTracer::buildShaderBindingTable(VkPipeline pipeline, VkBuffer& sbtBuffer, VkDeviceMemory& sbtMemory,
                                         VkStridedDeviceAddressRegionKHR& rgenRegion,
                                         VkStridedDeviceAddressRegionKHR& missRegion,
                                         VkStridedDeviceAddressRegionKHR& hitRegion,
                                         VkStridedDeviceAddressRegionKHR& extraRegion) {
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

    // 4 groups: rgen, miss, closest-hit, the second rgen
    uint32_t groupCount = 4;

    // Get shader group handles from the pipeline
    std::vector<uint8_t> handles(groupCount * handleSize);
    if (vkGetRayTracingShaderGroupHandlesKHR(m_device, pipeline, 0, groupCount,
                                              handles.size(), handles.data()) != VK_SUCCESS) {
        OHAO_LOG(Error, render, "PathTracer: failed to get shader group handles");
        return false;
    }

    // Each region must be baseAlignment-aligned
    uint32_t rgenSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t missSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t hitSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t totalSize = rgenSize + missSize + hitSize + rgenSize;   // the second rgen last

    // Create SBT buffer
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = totalSize;
    bufInfo.usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &sbtBuffer) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, sbtBuffer, &memReqs);

    VkMemoryAllocateFlagsInfo allocFlags{};
    allocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    allocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &allocFlags;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &sbtMemory) != VK_SUCCESS) return false;
    vkBindBufferMemory(m_device, sbtBuffer, sbtMemory, 0);

    // Write handles into SBT buffer
    void* mapped;
    vkMapMemory(m_device, sbtMemory, 0, totalSize, 0, &mapped);
    memset(mapped, 0, totalSize);

    uint8_t* dst = static_cast<uint8_t*>(mapped);
    // Rgen at offset 0
    memcpy(dst, handles.data() + 0 * handleSize, handleSize);
    // Miss at offset rgenSize
    memcpy(dst + rgenSize, handles.data() + 1 * handleSize, handleSize);
    // Closest-hit at offset rgenSize + missSize
    memcpy(dst + rgenSize + missSize, handles.data() + 2 * handleSize, handleSize);
    // The second raygen after the hit group
    memcpy(dst + rgenSize + missSize + hitSize, handles.data() + 3 * handleSize, handleSize);

    vkUnmapMemory(m_device, sbtMemory);

    // Set up strided device address regions
    VkDeviceAddress sbtAddr = getBufferDeviceAddress(sbtBuffer);

    rgenRegion.deviceAddress = sbtAddr;
    // A raygen region holds exactly one record, and the spec requires its size
    // to equal its stride (VUID-vkCmdTraceRaysKHR-size-04023).
    rgenRegion.stride = rgenSize;
    rgenRegion.size = rgenSize;

    missRegion.deviceAddress = sbtAddr + rgenSize;
    missRegion.stride = handleSizeAligned;
    missRegion.size = missSize;

    hitRegion.deviceAddress = sbtAddr + rgenSize + missSize;
    hitRegion.stride = handleSizeAligned;
    hitRegion.size = hitSize;

    extraRegion.deviceAddress = sbtAddr + rgenSize + missSize + hitSize;
    extraRegion.stride = rgenSize;
    extraRegion.size = rgenSize;

    m_callRegion = {};  // not used

    OHAO_LOG(Debug, render, "PathTracer: SBT created (handleSize={}, aligned={})",
             handleSize, handleSizeAligned);
    return true;
}

} // namespace ohao
