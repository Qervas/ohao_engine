#include "rt_compute.hpp"
#include "core/log.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace ohao::rtc {

namespace {

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

bool createImage(VkDevice device, VkPhysicalDevice physicalDevice, Image& img, VkFormat format,
                 VkImageUsageFlags usage, uint32_t width, uint32_t height) {
    img.width = std::max(width, 1u);
    img.height = std::max(height, 1u);
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {img.width, img.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageInfo, nullptr, &img.image) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, img.image, &memReqs);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &img.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(device, img.image, img.memory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = img.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return vkCreateImageView(device, &viewInfo, nullptr, &img.view) == VK_SUCCESS;
}

void destroyImage(VkDevice device, Image& img) {
    if (img.view) vkDestroyImageView(device, img.view, nullptr);
    if (img.image) vkDestroyImage(device, img.image, nullptr);
    if (img.memory) vkFreeMemory(device, img.memory, nullptr);
    img = Image{};
}

void transition(VkCommandBuffer cmd, Image& img, VkImageLayout layout, VkPipelineStageFlags src,
                VkAccessFlags srcAccess, VkPipelineStageFlags dst, VkAccessFlags dstAccess, bool discard) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : img.layout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
    img.layout = layout;
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
        result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
    }
    vkDestroyShaderModule(device, module, nullptr);
    if (result != VK_SUCCESS)
        OHAO_LOG(Error, render, "RT: {} pipeline of {} failed (err={})", entry, spirv, static_cast<int>(result));
    return result == VK_SUCCESS;
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

bool createHostBuffer(VkDevice device, VkPhysicalDevice physicalDevice, VkDeviceSize size, VkBufferUsageFlags usage,
                      HostBuffer& out) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    if (vkCreateBuffer(device, &bi, nullptr, &out.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, out.buffer, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    // Device-local memory the host can write (resizable BAR) when there is some, else
    // system memory, which every read crosses the bus for.
    constexpr VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    ai.memoryTypeIndex = findMemoryType(physicalDevice, req.memoryTypeBits, host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = findMemoryType(physicalDevice, req.memoryTypeBits, host);
    if (ai.memoryTypeIndex == UINT32_MAX) return false;
    if (vkAllocateMemory(device, &ai, nullptr, &out.memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(device, out.buffer, out.memory, 0);
    if (vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) != VK_SUCCESS) return false;
    std::memset(out.mapped, 0, static_cast<std::size_t>(size));
    return true;
}

void destroyHostBuffer(VkDevice device, HostBuffer& buf) {
    if (buf.buffer) vkDestroyBuffer(device, buf.buffer, nullptr);
    if (buf.memory) vkFreeMemory(device, buf.memory, nullptr);   // unmaps
    buf = HostBuffer{};
}

VkDescriptorSetLayoutBinding layoutBinding(uint32_t index, VkDescriptorType type, VkShaderStageFlags stages) {
    VkDescriptorSetLayoutBinding b{};
    b.binding = index;
    b.descriptorType = type;
    b.descriptorCount = 1;
    b.stageFlags = stages;
    return b;
}

VkWriteDescriptorSet descriptorWrite(VkDescriptorSet set, uint32_t index, VkDescriptorType type) {
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = index;
    w.descriptorCount = 1;
    w.descriptorType = type;
    return w;
}

} // namespace ohao::rtc
