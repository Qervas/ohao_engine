#include "render_pass_base.hpp"
#include <fstream>
#include "core/log.hpp"
#include <stdexcept>

namespace ohao {

VkShaderModule RenderPassBase::createShaderModule(const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code.size() * sizeof(uint32_t),
        .pCode = code.data(),
    };

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module");
    }
    return shaderModule;
}

VkShaderModule RenderPassBase::loadShaderModule(std::string_view path) {
    // Search multiple paths for shader SPVs
    const std::string pathStr(path);
    std::vector<std::string> searchPaths = {
        s_shaderBasePath + pathStr,
        "build/shaders/" + pathStr,
        "build/Release/bin/shaders/" + pathStr,
    };
    std::ifstream file;
    for (const auto& p : searchPaths) {
        file.open(p, std::ios::ate | std::ios::binary);
        if (file.is_open()) break;
    }
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open shader file: " + pathStr);
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));

    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fileSize));
    file.close();

    return createShaderModule(buffer);
}

uint32_t RenderPassBase::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1u << i)) &&
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

    VkImageCreateInfo imageInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    if (vkCreateImage(m_device, &imageInfo, nullptr, &rt.image) != VK_SUCCESS) {
        return rt;
    }

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(m_device, rt.image, &memReq);

    VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReq.size,
        .memoryTypeIndex = findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &rt.memory) != VK_SUCCESS) {
        vkDestroyImage(m_device, rt.image, nullptr);
        rt.image = VK_NULL_HANDLE;
        return rt;
    }

    vkBindImageMemory(m_device, rt.image, rt.memory, 0);

    VkImageViewCreateInfo viewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = rt.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {
            .aspectMask = aspect,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

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
    VkSamplerCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = filter,
        .minFilter = filter,
        .addressModeU = addressMode,
        .addressModeV = addressMode,
        .addressModeW = addressMode,
    };

    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(m_device, &info, nullptr, &sampler);
    return sampler;
}

std::array<VkSubpassDependency, 2> RenderPassBase::sampledColorPassDependencies() {
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    return deps;
}

void RenderPassBase::transitionImage(VkCommandBuffer cmd, VkImage image,
                                      VkImageLayout oldLayout, VkImageLayout newLayout,
                                      VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                                      VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = srcAccess,
        .dstAccessMask = dstAccess,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool RenderPassBase::createComputePipeline(std::string_view shaderPath,
                                            VkDescriptorSetLayout descriptorLayout,
                                            uint32_t pushConstantSize,
                                            VkPipeline& outPipeline, VkPipelineLayout& outLayout) {
    VkShaderModule compShader = loadShaderModule(shaderPath);

    VkPipelineShaderStageCreateInfo stageInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = compShader,
        .pName = "main",
    };

    VkPushConstantRange pushConstant{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = pushConstantSize,
    };

    VkPipelineLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptorLayout,
        .pushConstantRangeCount = pushConstantSize > 0 ? 1u : 0u,
        .pPushConstantRanges = pushConstantSize > 0 ? &pushConstant : nullptr,
    };

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &outLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, compShader, nullptr);
        return false;
    }

    VkComputePipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = stageInfo,
        .layout = outLayout,
    };

    VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &outPipeline);
    vkDestroyShaderModule(m_device, compShader, nullptr);

    return result == VK_SUCCESS;
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
