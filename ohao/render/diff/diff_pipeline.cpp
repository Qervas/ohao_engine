#include "render/diff/diff_pipeline.hpp"

#include <cstring>
#include <iostream>
#include <vector>

namespace ohao::diff {

std::uint32_t DiffPipeline::findMemType(std::uint32_t typeBits,
                                        VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &mp);
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0;
}

DiffStatus DiffPipeline::init(VkDevice device, VkPhysicalDevice physical, std::uint32_t mapW,
                              std::uint32_t mapH, std::uint32_t beautyW, std::uint32_t beautyH) {
    destroy();
    if (!device || !physical || mapW == 0 || mapH == 0) return DiffStatus::Failed;

    device_ = device;
    physical_ = physical;
    mapDesc_.width = mapW;
    mapDesc_.height = mapH;
    mapDesc_.channels = 4;
    beautyDesc_.width = beautyW;
    beautyDesc_.height = beautyH;

    std::uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qCount, qprops.data());
    queueFamily_ = 0;
    for (std::uint32_t i = 0; i < qCount; ++i) {
        if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queueFamily_ = i;
            break;
        }
    }
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = queueFamily_;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(device_, &pci, nullptr, &cmdPool_) != VK_SUCCESS)
        return DiffStatus::Failed;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    ici.extent = {mapW, mapH, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ici, nullptr, &albedoImage_) != VK_SUCCESS)
        return DiffStatus::Failed;

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device_, albedoImage_, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex =
        findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &mai, nullptr, &albedoMem_) != VK_SUCCESS)
        return DiffStatus::Failed;
    vkBindImageMemory(device_, albedoImage_, albedoMem_, 0);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = albedoImage_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &vci, nullptr, &albedoView_) != VK_SUCCESS)
        return DiffStatus::Failed;

    stagingBytes_ = static_cast<VkDeviceSize>(mapW) * mapH * 4u * sizeof(float);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = stagingBytes_;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(device_, &bci, nullptr, &stagingBuf_) != VK_SUCCESS)
        return DiffStatus::Failed;
    vkGetBufferMemoryRequirements(device_, stagingBuf_, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = findMemType(
        mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device_, &mai, nullptr, &stagingMem_) != VK_SUCCESS)
        return DiffStatus::Failed;
    vkBindBufferMemory(device_, stagingBuf_, stagingMem_, 0);
    if (vkMapMemory(device_, stagingMem_, 0, stagingBytes_, 0, &stagingMapped_) != VK_SUCCESS)
        return DiffStatus::Failed;

    // Transition image to GENERAL once for repeated up/download.
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmdPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &ai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = albedoImage_;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar.srcAccessMask = 0;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                        VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, cmdPool_, 1, &cmd);

    ready_ = true;
    std::cout << "  [diff-vk] pipeline ready map=" << mapW << "x" << mapH
              << " beauty=" << beautyW << "x" << beautyH << " albedoImage=ok\n";
    return DiffStatus::Ok;
}

void DiffPipeline::resize(std::uint32_t beautyW, std::uint32_t beautyH) {
    beautyDesc_.width = beautyW;
    beautyDesc_.height = beautyH;
}

void DiffPipeline::destroy() noexcept {
    if (!device_) {
        ready_ = false;
        return;
    }
    vkDeviceWaitIdle(device_);
    if (stagingMapped_) {
        vkUnmapMemory(device_, stagingMem_);
        stagingMapped_ = nullptr;
    }
    if (stagingBuf_) vkDestroyBuffer(device_, stagingBuf_, nullptr);
    if (stagingMem_) vkFreeMemory(device_, stagingMem_, nullptr);
    if (albedoView_) vkDestroyImageView(device_, albedoView_, nullptr);
    if (albedoImage_) vkDestroyImage(device_, albedoImage_, nullptr);
    if (albedoMem_) vkFreeMemory(device_, albedoMem_, nullptr);
    if (cmdPool_) vkDestroyCommandPool(device_, cmdPool_, nullptr);
    stagingBuf_ = VK_NULL_HANDLE;
    stagingMem_ = VK_NULL_HANDLE;
    albedoView_ = VK_NULL_HANDLE;
    albedoImage_ = VK_NULL_HANDLE;
    albedoMem_ = VK_NULL_HANDLE;
    cmdPool_ = VK_NULL_HANDLE;
    ready_ = false;
    device_ = VK_NULL_HANDLE;
    physical_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
}

DiffStatus DiffPipeline::uploadAlbedoMap(const float* rgb, std::uint32_t w, std::uint32_t h) {
    if (!ready_ || !rgb || w != mapDesc_.width || h != mapDesc_.height || !stagingMapped_)
        return DiffStatus::Failed;
    auto* dst = static_cast<float*>(stagingMapped_);
    for (std::uint32_t i = 0; i < w * h; ++i) {
        dst[i * 4 + 0] = rgb[i * 3 + 0];
        dst[i * 4 + 1] = rgb[i * 3 + 1];
        dst[i * 4 + 2] = rgb[i * 3 + 2];
        dst[i * 4 + 3] = 1.f;
    }

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmdPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &ai, &cmd) != VK_SUCCESS) return DiffStatus::Failed;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuf_, albedoImage_, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, cmdPool_, 1, &cmd);
    return DiffStatus::Ok;
}

DiffStatus DiffPipeline::downloadAlbedoMap(float* rgbOut, std::uint32_t w, std::uint32_t h) {
    if (!ready_ || !rgbOut || w != mapDesc_.width || h != mapDesc_.height || !stagingMapped_)
        return DiffStatus::Failed;

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmdPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &ai, &cmd) != VK_SUCCESS) return DiffStatus::Failed;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, albedoImage_, VK_IMAGE_LAYOUT_GENERAL, stagingBuf_, 1, &region);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, cmdPool_, 1, &cmd);

    const auto* src = static_cast<const float*>(stagingMapped_);
    for (std::uint32_t i = 0; i < w * h; ++i) {
        rgbOut[i * 3 + 0] = src[i * 4 + 0];
        rgbOut[i * 3 + 1] = src[i * 4 + 1];
        rgbOut[i * 3 + 2] = src[i * 4 + 2];
    }
    return DiffStatus::Ok;
}

} // namespace ohao::diff
