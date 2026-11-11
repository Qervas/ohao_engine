#include "rt_compute.hpp"
#include "core/log.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace ohao::rtc {

namespace {

bool g_pipelineStats = false;

std::vector<char> readFile(const std::string& path) {
    const std::string name = path.substr(path.find_last_of("/\\") + 1);
    for (const std::string& p : {path, "build/shaders/" + name, "build/Release/bin/shaders/" + name}) {
        std::ifstream file(p, std::ios::ate | std::ios::binary);
        if (!file.is_open()) continue;
        const auto size = static_cast<std::size_t>(file.tellg());
        std::vector<char> buffer(size);
        file.seekg(0);
        file.read(buffer.data(), static_cast<std::streamsize>(size));
        return buffer;
    }
    return {};
}

} // namespace

uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return UINT32_MAX;
}

bool createComputePipeline(VkDevice device, const char* spirv, const char* entry, VkDescriptorSetLayout setLayout,
                           uint32_t pushBytes, VkPipelineLayout& layout, VkPipeline& pipeline) {
    return createComputePipeline(device, spirv, entry, std::span<const VkDescriptorSetLayout>(&setLayout, 1), pushBytes,
                                 layout, pipeline);
}

bool createComputePipeline(VkDevice device, const char* spirv, const char* entry,
                           std::span<const VkDescriptorSetLayout> setLayouts, uint32_t pushBytes, VkPipelineLayout& layout,
                           VkPipeline& pipeline) {
    const auto code = readFile(std::string("bin/shaders/") + spirv);
    if (code.empty()) {
        OHAO_LOG(Error, render, "RT: failed to load {}", spirv);
        return false;
    }
    VkShaderModuleCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = code.size();
    mi.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &mi, nullptr, &module) != VK_SUCCESS) return false;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.size = pushBytes;
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    layoutInfo.pushConstantRangeCount = pushBytes > 0 ? 1u : 0u;   // a zero-size range is invalid
    layoutInfo.pPushConstantRanges = pushBytes > 0 ? &pushRange : nullptr;
    VkResult result = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout);
    if (result == VK_SUCCESS) {
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = entry;
        ci.layout = layout;
        if (g_pipelineStats) ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
    }
    vkDestroyShaderModule(device, module, nullptr);
    if (result != VK_SUCCESS)
        OHAO_LOG(Error, render, "RT: {} pipeline of {} failed (err={})", entry, spirv, static_cast<int>(result));
    else if (g_pipelineStats)
        logPipelineStats(device, pipeline, spirv);
    return result == VK_SUCCESS;
}

void setPipelineStats(bool enabled) noexcept {
    g_pipelineStats = enabled;
}

bool pipelineStats() noexcept {
    return g_pipelineStats;
}

void logPipelineStats(VkDevice device, VkPipeline pipeline, const char* spirv) {
    const auto getProperties = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
        vkGetDeviceProcAddr(device, "vkGetPipelineExecutablePropertiesKHR"));
    const auto getStatistics = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
        vkGetDeviceProcAddr(device, "vkGetPipelineExecutableStatisticsKHR"));
    if (!getProperties || !getStatistics) return;
    VkPipelineInfoKHR info{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    info.pipeline = pipeline;
    uint32_t executables = 0;
    getProperties(device, &info, &executables, nullptr);
    for (uint32_t e = 0; e < executables; ++e) {
        VkPipelineExecutableInfoKHR exec{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        exec.pipeline = pipeline;
        exec.executableIndex = e;
        uint32_t count = 0;
        getStatistics(device, &exec, &count, nullptr);
        std::vector<VkPipelineExecutableStatisticKHR> stats(count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        getStatistics(device, &exec, &count, stats.data());
        std::string line;
        for (const auto& s : stats) {
            line += s.name;
            line += '=';
            switch (s.format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: line += s.value.b32 ? "true" : "false"; break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: line += std::to_string(s.value.i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: line += std::to_string(s.value.u64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: line += std::to_string(s.value.f64); break;
            default: line += '?'; break;
            }
            line += "; ";
        }
        OHAO_LOG(Info, render, "pipeline stats {} [{}]: {}", spirv, e, line);
    }
}


VkShaderModule loadShaderModule(VkDevice device, const char* spirv) {
    const auto code = readFile(std::string("bin/shaders/") + spirv);
    if (code.empty()) {
        OHAO_LOG(Error, render, "failed to load {}", spirv);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = code.size();
    mi.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &mi, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

GpuBuffer createHostBuffer(GpuAllocator& allocator, VkDeviceSize size, VkBufferUsageFlags usage) {
    GpuBuffer buffer = allocator.createBuffer(size, usage, AllocationUsage::CpuToGpu, true);
    if (!buffer.isValid() || !buffer.getMappedData()) {
        allocator.destroyBuffer(buffer);
        return GpuBuffer{};
    }
    std::memset(buffer.getMappedData(), 0, static_cast<std::size_t>(size));
    return buffer;
}

} // namespace ohao::rtc
