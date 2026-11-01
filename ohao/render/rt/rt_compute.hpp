#pragma once

// What the RT compute techniques (RT shadows, RT GI) share: device-local images that carry
// their layout, barriers on them, compute pipelines from SPIR-V, the descriptor boilerplate,
// and the small host-written buffers a frame slot owns.

#include <vulkan/vulkan.h>
#include <span>
#include <cstdint>

namespace ohao::rtc {

struct Image {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    uint32_t width{0};
    uint32_t height{0};
};

// A buffer the host writes every frame and every pixel reads: device-local memory the host
// can map (resizable BAR) when there is some, else host memory, mapped for its lifetime.
struct HostBuffer {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    void* mapped{nullptr};
};

uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);

// A 2D device-local image of `format` (at least 1x1) and its view, in UNDEFINED layout.
bool createImage(VkDevice device, VkPhysicalDevice physicalDevice, Image& img, VkFormat format,
                 VkImageUsageFlags usage, uint32_t width, uint32_t height);
void destroyImage(VkDevice device, Image& img);

// A barrier on `img` to `layout`; an UNDEFINED old layout when `discard` (or the image is new).
void transition(VkCommandBuffer cmd, Image& img, VkImageLayout layout, VkPipelineStageFlags src,
                VkAccessFlags srcAccess, VkPipelineStageFlags dst, VkAccessFlags dstAccess, bool discard);

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

bool createHostBuffer(VkDevice device, VkPhysicalDevice physicalDevice, VkDeviceSize size, VkBufferUsageFlags usage,
                      HostBuffer& out);
void destroyHostBuffer(VkDevice device, HostBuffer& buf);

VkDescriptorSetLayoutBinding layoutBinding(uint32_t index, VkDescriptorType type,
                                           VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT);
VkWriteDescriptorSet descriptorWrite(VkDescriptorSet set, uint32_t index, VkDescriptorType type);

} // namespace ohao::rtc
