#include "render_pass_base.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace ohao {

VkShaderModule RenderPassBase::createShaderModule(const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size() * sizeof(uint32_t);
    createInfo.pCode = code.data();

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module");
    }
    return shaderModule;
}

VkShaderModule RenderPassBase::loadShaderModule(const std::string& path) {
    // Resolve shader path - prepend base path for relative paths
    std::string resolvedPath = s_shaderBasePath + path;

    std::ifstream file(resolvedPath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader file: " + resolvedPath);
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));

    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
    file.close();

    return createShaderModule(buffer);
}

uint32_t RenderPassBase::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type");
}

RenderTarget RenderPassBase::createRenderTarget(VkFormat format, uint32_t width, uint32_t height,
                                                 VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    RenderTarget rt{};
    rt.format = format;
    rt.width = width;
    rt.height = height;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &rt.image) != VK_SUCCESS) {
        return rt;
    }

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(m_device, rt.image, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &rt.memory) != VK_SUCCESS) {
        vkDestroyImage(m_device, rt.image, nullptr);
        rt.image = VK_NULL_HANDLE;
        return rt;
    }

    vkBindImageMemory(m_device, rt.image, rt.memory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = rt.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &rt.view) != VK_SUCCESS) {
        vkFreeMemory(m_device, rt.memory, nullptr);
        vkDestroyImage(m_device, rt.image, nullptr);
        rt.image = VK_NULL_HANDLE;
        rt.memory = VK_NULL_HANDLE;
        return rt;
    }

    return rt;
}

VkSampler RenderPassBase::createSampler(VkFilter filter, VkSamplerAddressMode addressMode) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = filter;
    info.minFilter = filter;
    info.addressModeU = addressMode;
    info.addressModeV = addressMode;
    info.addressModeW = addressMode;

    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(m_device, &info, nullptr, &sampler);
    return sampler;
}

void RenderPassBase::transitionImage(VkCommandBuffer cmd, VkImage image,
                                      VkImageLayout oldLayout, VkImageLayout newLayout,
                                      VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                                      VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool RenderPassBase::createComputePipeline(const std::string& shaderPath,
                                            VkDescriptorSetLayout descriptorLayout,
                                            uint32_t pushConstantSize,
                                            VkPipeline& outPipeline, VkPipelineLayout& outLayout) {
    VkShaderModule compShader = loadShaderModule(shaderPath);

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = compShader;
    stageInfo.pName = "main";

    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstant.offset = 0;
    pushConstant.size = pushConstantSize;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorLayout;
    layoutInfo.pushConstantRangeCount = pushConstantSize > 0 ? 1u : 0u;
    layoutInfo.pPushConstantRanges = pushConstantSize > 0 ? &pushConstant : nullptr;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &outLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, compShader, nullptr);
        return false;
    }

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = outLayout;

    VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &outPipeline);
    vkDestroyShaderModule(m_device, compShader, nullptr);

    return result == VK_SUCCESS;
}

bool RenderPassBase::reloadComputeShader(const std::string& absoluteSpvPath,
                                          VkDescriptorSetLayout descriptorLayout,
                                          uint32_t pushConstantSize,
                                          VkPipeline& pipeline, VkPipelineLayout& pipelineLayout) {
    // Wait for GPU to finish using the old pipeline
    vkDeviceWaitIdle(m_device);

    // Load SPV directly (absolute path, no base path prepend)
    std::ifstream file(absoluteSpvPath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "reloadComputeShader: failed to open " << absoluteSpvPath << std::endl;
        return false;
    }
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> code(fileSize / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), fileSize);
    file.close();

    VkShaderModule shaderModule = createShaderModule(code);
    if (shaderModule == VK_NULL_HANDLE) {
        std::cerr << "reloadComputeShader: invalid SPIR-V" << std::endl;
        return false;
    }

    // Destroy old pipeline and layout
    safeDestroy(pipeline);
    safeDestroy(pipelineLayout);

    // Recreate layout
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstant.offset = 0;
    pushConstant.size = pushConstantSize;

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorLayout;
    layoutInfo.pushConstantRangeCount = pushConstantSize > 0 ? 1u : 0u;
    layoutInfo.pPushConstantRanges = pushConstantSize > 0 ? &pushConstant : nullptr;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, shaderModule, nullptr);
        std::cerr << "reloadComputeShader: pipeline layout creation failed" << std::endl;
        return false;
    }

    // Recreate pipeline
    VkPipelineShaderStageCreateInfo stageInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = shaderModule;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = pipelineLayout;

    VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    vkDestroyShaderModule(m_device, shaderModule, nullptr);

    if (result != VK_SUCCESS) {
        std::cerr << "reloadComputeShader: pipeline creation failed" << std::endl;
        return false;
    }

    std::cout << "reloadComputeShader: success for " << getName() << std::endl;
    return true;
}

void RenderPassBase::safeDestroy(VkPipeline& handle) {
    if (handle != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, handle, nullptr); handle = VK_NULL_HANDLE; }
}
void RenderPassBase::safeDestroy(VkPipelineLayout& handle) {
    if (handle != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, handle, nullptr); handle = VK_NULL_HANDLE; }
}
void RenderPassBase::safeDestroy(VkDescriptorPool& handle) {
    if (handle != VK_NULL_HANDLE) { vkDestroyDescriptorPool(m_device, handle, nullptr); handle = VK_NULL_HANDLE; }
}
void RenderPassBase::safeDestroy(VkDescriptorSetLayout& handle) {
    if (handle != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(m_device, handle, nullptr); handle = VK_NULL_HANDLE; }
}
void RenderPassBase::safeDestroy(VkSampler& handle) {
    if (handle != VK_NULL_HANDLE) { vkDestroySampler(m_device, handle, nullptr); handle = VK_NULL_HANDLE; }
}
void RenderPassBase::safeDestroy(VkImageView& handle) {
    if (handle != VK_NULL_HANDLE) { vkDestroyImageView(m_device, handle, nullptr); handle = VK_NULL_HANDLE; }
}
void RenderPassBase::safeDestroy(VkImage& handle) {
    if (handle != VK_NULL_HANDLE) { vkDestroyImage(m_device, handle, nullptr); handle = VK_NULL_HANDLE; }
}
void RenderPassBase::safeDestroy(VkBuffer& handle) {
    if (handle != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, handle, nullptr); handle = VK_NULL_HANDLE; }
}
void RenderPassBase::safeDestroy(VkFramebuffer& handle) {
    if (handle != VK_NULL_HANDLE) { vkDestroyFramebuffer(m_device, handle, nullptr); handle = VK_NULL_HANDLE; }
}
void RenderPassBase::safeDestroy(VkRenderPass& handle) {
    if (handle != VK_NULL_HANDLE) { vkDestroyRenderPass(m_device, handle, nullptr); handle = VK_NULL_HANDLE; }
}
void RenderPassBase::safeFree(VkDeviceMemory& handle) {
    if (handle != VK_NULL_HANDLE) { vkFreeMemory(m_device, handle, nullptr); handle = VK_NULL_HANDLE; }
}

} // namespace ohao
