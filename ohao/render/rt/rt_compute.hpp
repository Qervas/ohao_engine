#pragma once

// What the compute passes share: compute pipelines from SPIR-V, their statistics, shader modules, and the
// small buffers the host writes every frame.

#include "gpu/vulkan/gpu_allocator.hpp"

#include <vulkan/vulkan.h>
#include <span>
#include <cstdint>

namespace ohao::rtc {

// LEGACY(step 4a): ManifoldCasters' host-written buffers (the path tracer's frame, not moved yet); everything
// else makes its own with createHostBuffer(GpuAllocator&, ...).
struct HostBuffer {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    void* mapped{nullptr};
};

uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);

// Pipeline statistics (OHAO_PIPELINE_STATS=1 on a device with VK_KHR_pipeline_executable_properties,
// which the device then enables): every compute pipeline made below logs the driver's statistics
// for it (registers, shared memory, ...) on the render channel. A kernel's occupancy is not in any
// timing; this is how to see it.
void setPipelineStats(bool enabled) noexcept;
bool pipelineStats() noexcept;
// Logs `pipeline`'s statistics, one line per executable (a ray tracing pipeline has one per stage),
// named `name`. The pipeline must have been created with VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR.
void logPipelineStats(VkDevice device, VkPipeline pipeline, const char* name);

// A compute pipeline of `entry` in bin/shaders/<spirv>, one descriptor set, `pushBytes` of
// push constants. Logs and returns false when the SPIR-V is missing or creation fails.
bool createComputePipeline(VkDevice device, const char* spirv, const char* entry, VkDescriptorSetLayout setLayout,
                           uint32_t pushBytes, VkPipelineLayout& layout, VkPipeline& pipeline);
// The same with several descriptor sets, set i = setLayouts[i].
bool createComputePipeline(VkDevice device, const char* spirv, const char* entry,
                           std::span<const VkDescriptorSetLayout> setLayouts, uint32_t pushBytes, VkPipelineLayout& layout,
                           VkPipeline& pipeline);

// A shader module of bin/shaders/<spirv> (null, logged, when missing) for a graphics pipeline.
VkShaderModule loadShaderModule(VkDevice device, const char* spirv);

// A buffer the host writes every frame and every pixel reads: device-local memory the host can map (resizable
// BAR) when there is some, else host memory; mapped for its lifetime and zeroed. Invalid when it fails.
[[nodiscard]] GpuBuffer createHostBuffer(GpuAllocator& allocator, VkDeviceSize size, VkBufferUsageFlags usage);

// LEGACY(step 4a): ManifoldCasters' (see HostBuffer).
bool createHostBuffer(VkDevice device, VkPhysicalDevice physicalDevice, VkDeviceSize size, VkBufferUsageFlags usage,
                      HostBuffer& out);
void destroyHostBuffer(VkDevice device, HostBuffer& buf);


} // namespace ohao::rtc
