#pragma once

// What the RT compute techniques (RT shadows, RT GI) share: device-local images that carry
// their layout, barriers on them, compute pipelines from SPIR-V, the descriptor boilerplate,
// and the small host-written buffers a frame slot owns.

#include <vulkan/vulkan.h>
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

// A compute pipeline of `entry` in bin/shaders/<spirv>, one descriptor set, `pushBytes` of
// push constants. Logs and returns false when the SPIR-V is missing or creation fails.
bool createComputePipeline(VkDevice device, const char* spirv, const char* entry, VkDescriptorSetLayout setLayout,
                           uint32_t pushBytes, VkPipelineLayout& layout, VkPipeline& pipeline);

bool createHostBuffer(VkDevice device, VkPhysicalDevice physicalDevice, VkDeviceSize size, VkBufferUsageFlags usage,
                      HostBuffer& out);
void destroyHostBuffer(VkDevice device, HostBuffer& buf);

VkDescriptorSetLayoutBinding layoutBinding(uint32_t index, VkDescriptorType type,
                                           VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT);
VkWriteDescriptorSet descriptorWrite(VkDescriptorSet set, uint32_t index, VkDescriptorType type);

} // namespace ohao::rtc
