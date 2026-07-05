// Extracted from path_tracer.cpp — image + memory + material-buffer methods.
// Kept as members of class PathTracer; no behavior change.

#include "path_tracer.hpp"

#include <cstring>
#include <iostream>
#include <vector>

namespace ohao {

bool PathTracer::createImages() {
    // --- Accumulation buffer: RGBA32F for HDR accumulation across frames ---
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // SAMPLED added so DLSS-RR (Phase 5) can bind the accum buffer as its HDR
        // COLOR_IN guide; harmless for every other mode (usage only, no pixel change).
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_accumBuffer) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_accumBuffer, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_accumMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_accumBuffer, m_accumMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_accumBuffer;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_accumView) != VK_SUCCESS) return false;
    }

    // --- Output image: RGBA8 for tonemapped final output ---
    // Sized to the OUTPUT/display resolution (m_outW/m_outH), NOT the render res:
    // in a DLSS upscaling preset this is the full-res target the DLSS tonemap
    // writes; the raygen's own beauty write (render res) into it is discarded.
    // In every non-upscaling mode render==output so it is identical to before.
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {m_outW, m_outH, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_outputImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_outputImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_outputMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_outputImage, m_outputMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_outputImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_outputView) != VK_SUCCESS) return false;
    }

    // --- Albedo AOV: RGBA32F for denoiser guide buffer ---
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_albedoAOV) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_albedoAOV, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_albedoAOVMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_albedoAOV, m_albedoAOVMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_albedoAOV;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_albedoAOVView) != VK_SUCCESS) return false;
    }

    // --- Normal AOV: RGBA32F for denoiser guide buffer ---
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // SAMPLED added for DLSS-RR (Phase 5) — the DLSSRR raygen path repurposes
        // this AOV as the packed (worldN, roughness) guide fed to pInNormals/pInRoughness.
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_normalAOV) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_normalAOV, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_normalAOVMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_normalAOV, m_normalAOVMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_normalAOV;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_normalAOVView) != VK_SUCCESS) return false;
    }

    // --- Surface history ping-pong: RGBA32F for world-position validation ---
    for (uint32_t i = 0; i < 2; ++i) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_surfaceHistoryImages[i]) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_surfaceHistoryImages[i], &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_surfaceHistoryMemory[i]) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_surfaceHistoryImages[i], m_surfaceHistoryMemory[i], 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_surfaceHistoryImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_surfaceHistoryViews[i]) != VK_SUCCESS) return false;
        m_surfaceHistoryInitialized[i] = false;
    }
    m_surfaceHistoryWriteIndex = 0;

    // --- Shading history ping-pong: RGBA32F for normal/roughness validation ---
    for (uint32_t i = 0; i < 2; ++i) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_shadingHistoryImages[i]) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_shadingHistoryImages[i], &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_shadingHistoryMemory[i]) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_shadingHistoryImages[i], m_shadingHistoryMemory[i], 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_shadingHistoryImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_shadingHistoryViews[i]) != VK_SUCCESS) return false;
        m_shadingHistoryInitialized[i] = false;
    }
    m_shadingHistoryWriteIndex = 0;

    // --- ReSTIR GI reservoir ping-pong: 3 RGBA32F planes × 2 (curr/prev) ---
    for (uint32_t plane = 0; plane < 3; ++plane) {
        for (uint32_t i = 0; i < 2; ++i) {
            VkImageCreateInfo imageInfo{};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
            imageInfo.extent = {m_width, m_height, 1};
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            if (vkCreateImage(m_device, &imageInfo, nullptr, &m_giReservoirImages[plane][i]) != VK_SUCCESS) return false;

            VkMemoryRequirements memReqs;
            vkGetImageMemoryRequirements(m_device, m_giReservoirImages[plane][i], &memReqs);

            VkMemoryAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memReqs.size;
            allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

            if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_giReservoirMemory[plane][i]) != VK_SUCCESS) return false;
            vkBindImageMemory(m_device, m_giReservoirImages[plane][i], m_giReservoirMemory[plane][i], 0);

            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = m_giReservoirImages[plane][i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;

            if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_giReservoirViews[plane][i]) != VK_SUCCESS) return false;
        }
    }
    m_giReservoirInitialized[0] = false;
    m_giReservoirInitialized[1] = false;
    m_giReservoirWriteIndex = 0;

    // ---- Feature 3.A: Motion vector AOV (RG16F) ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R16G16_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_motionVectorImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_motionVectorImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_motionVectorMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_motionVectorImage, m_motionVectorMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_motionVectorImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_motionVectorView) != VK_SUCCESS) return false;
    }

    // ---- Feature 3.B: Depth AOV (R32F) ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_depthAOVImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_depthAOVImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_depthAOVMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_depthAOVImage, m_depthAOVMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_depthAOVImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_depthAOVView) != VK_SUCCESS) return false;
    }

    // ---- Feature 3.B: Roughness AOV (R16F, promoted 3.C.5) ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R16_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_roughnessAOVImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_roughnessAOVImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_roughnessAOVMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_roughnessAOVImage, m_roughnessAOVMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_roughnessAOVImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_roughnessAOVView) != VK_SUCCESS) return false;
    }

    // ---- Feature 3.C: Diffuse radiance (RGBA32F, promoted 3.C.5) ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_diffuseRadianceImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_diffuseRadianceImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_diffuseRadianceMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_diffuseRadianceImage, m_diffuseRadianceMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_diffuseRadianceImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_diffuseRadianceView) != VK_SUCCESS) return false;
    }

    // ---- Feature 3.C: Specular radiance (RGBA32F, promoted 3.C.5) ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_specularRadianceImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_specularRadianceImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_specularRadianceMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_specularRadianceImage, m_specularRadianceMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_specularRadianceImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_specularRadianceView) != VK_SUCCESS) return false;
    }

    // ---- Feature 3.C.6: Diffuse albedo AOV (RGBA8 UNORM) ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // SAMPLED added for DLSS-RR (Phase 5) DIFFUSE_ALBEDO guide binding.
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_diffAlbedoImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_diffAlbedoImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_diffAlbedoMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_diffAlbedoImage, m_diffAlbedoMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_diffAlbedoImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_diffAlbedoView) != VK_SUCCESS) return false;
    }

    // ---- Feature 3.C.6: Specular color / F0 AOV (RGBA8 UNORM) ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // SAMPLED added for DLSS-RR (Phase 5) SPECULAR_ALBEDO guide binding.
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_specColorImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_specColorImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_specColorMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_specColorImage, m_specColorMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_specColorImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_specColorView) != VK_SUCCESS) return false;
    }

    // ---- Feature 4.B: Normal+roughness packed AOV (R10G10B10A2 UNORM) for NRD REBLUR ----
    // Format matches NRD_NORMAL_ENCODING=2 (NRD_NORMAL_ENCODING_R10G10B10A2_UNORM) as set in
    // external/cmake/nrd.cmake. Encoding is NRD's rotated oct + sign-in-roughness + 2-bit materialID,
    // ported verbatim from build/_deps/nrd-src/Shaders/NRD.hlsli::_NRD_EncodeNormalRoughness101010.
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_normalRoughnessImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_normalRoughnessImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_normalRoughnessMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_normalRoughnessImage, m_normalRoughnessMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_normalRoughnessImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_normalRoughnessView) != VK_SUCCESS) return false;
    }

    // ---- Sub-plan 4.C: NRD denoised diffuse output (RGBA32F) at binding 27 ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_outDiffRadianceImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_outDiffRadianceImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_outDiffRadianceMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_outDiffRadianceImage, m_outDiffRadianceMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_outDiffRadianceImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_outDiffRadianceView) != VK_SUCCESS) return false;
    }

    // ---- Sub-plan 4.C: NRD denoised specular output (RGBA32F) at binding 28 ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_outSpecRadianceImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_outSpecRadianceImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_outSpecRadianceMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_outSpecRadianceImage, m_outSpecRadianceMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_outSpecRadianceImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_outSpecRadianceView) != VK_SUCCESS) return false;
    }

    // ---- Sub-plan 4.D: NRD composed HDR output (RGBA32F) at binding 29 ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageInfo.format        = VK_FORMAT_R32G32B32A32_SFLOAT;
        imageInfo.extent        = {m_width, m_height, 1};
        imageInfo.mipLevels     = 1;
        imageInfo.arrayLayers   = 1;
        imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_nrdComposedImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_nrdComposedImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_nrdComposedMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_nrdComposedImage, m_nrdComposedMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                       = m_nrdComposedImage;
        viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                      = VK_FORMAT_R32G32B32A32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_nrdComposedView) != VK_SUCCESS) return false;

        // Reset first-frame latch so the UNDEFINED→GENERAL barrier fires
        // correctly on the new image, even if createImages() is re-entered
        // for resize (future 4.E scope). Without this, a resized VkImage
        // starts in UNDEFINED but the latch says "already transitioned",
        // producing a validation error + potential garbage first frame.
        m_nrdComposeFirstFrame = true;
    }

    // ---- Sub-plan 4.E: NRD tonemapped output (RGBA8 UNORM) at binding 30 ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent        = {m_width, m_height, 1};
        imageInfo.mipLevels     = 1;
        imageInfo.arrayLayers   = 1;
        imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_nrdTonemappedImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_nrdTonemappedImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_nrdTonemappedMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_nrdTonemappedImage, m_nrdTonemappedMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                       = m_nrdTonemappedImage;
        viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                      = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_nrdTonemappedView) != VK_SUCCESS) return false;

        // Reset first-frame latch so UNDEFINED→GENERAL fires on new VkImage
        // after resize (same rationale as m_nrdComposeFirstFrame in 4.D T2).
        m_nrdTonemapFirstFrame = true;
    }

    // ---- Sub-plan 4.J: pre-DoF LDR (RGBA8 UNORM) at binding 32 ----
    // Composite shader's outLDR is wired here; DoF gather pass reads this +
    // depth AOV and writes the final RGBA8 (m_nrdTonemappedImage, binding 30).
    // Same dimensions as binding 30 — they share the full-res output extent.
    // Usage = STORAGE (compute write by composite, read by DoF) +
    // TRANSFER_SRC (defensive; downstream readback never targets pre-DoF but
    // keeping symmetric with binding 30 makes diff-mode comparisons trivial).
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent        = {m_width, m_height, 1};
        imageInfo.mipLevels     = 1;
        imageInfo.arrayLayers   = 1;
        imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_preDofLdrImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_preDofLdrImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_preDofLdrMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_preDofLdrImage, m_preDofLdrMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                       = m_preDofLdrImage;
        viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                      = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_preDofLdrView) != VK_SUCCESS) return false;

        // Reset first-frame latch on new VkImage (resize-safe, same idiom as
        // m_nrdTonemapFirstFrame / m_nrdComposeFirstFrame).
        m_preDofFirstFrame = true;
    }

    // ---- Sub-plan 4.G: bloom mip chain (RGBA16F, 3 levels) ----
    // Mip 0 = half-res, mip 1 = quarter-res, mip 2 = eighth-res.
    // Usage: STORAGE (for compute writes in extract/blur) + SAMPLED (for
    // bilinear upsampling in composite). Layout transitions happen per frame
    // in path_tracer_render.cpp.
    m_bloomMipWidth[0]  = (m_width  + 1) / 2;
    m_bloomMipHeight[0] = (m_height + 1) / 2;
    m_bloomMipWidth[1]  = (m_bloomMipWidth[0]  + 1) / 2;
    m_bloomMipHeight[1] = (m_bloomMipHeight[0] + 1) / 2;
    m_bloomMipWidth[2]  = (m_bloomMipWidth[1]  + 1) / 2;
    m_bloomMipHeight[2] = (m_bloomMipHeight[1] + 1) / 2;
    for (uint32_t mip = 0; mip < 3; ++mip) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageInfo.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
        imageInfo.extent        = {m_bloomMipWidth[mip], m_bloomMipHeight[mip], 1};
        imageInfo.mipLevels     = 1;
        imageInfo.arrayLayers   = 1;
        imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_bloomMipImages[mip]) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_bloomMipImages[mip], &memReqs);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_bloomMipMemory[mip]) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_bloomMipImages[mip], m_bloomMipMemory[mip], 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                       = m_bloomMipImages[mip];
        viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                      = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_bloomMipViews[mip]) != VK_SUCCESS) return false;

        m_bloomFirstFrame[mip] = true;
    }
    // Shared linear-clamp sampler for bilinear bloom upsampling in composite.
    if (m_bloomSampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.maxLod       = 1.0f;
        if (vkCreateSampler(m_device, &si, nullptr, &m_bloomSampler) != VK_SUCCESS) return false;
    }

#ifdef OHAO_DLSS_ENABLED
    // ---- Phase 5: DLSS-RR COLOR_OUT — denoised HDR-linear result (RGBA16F) ----
    // DLSS writes this (readWrite resource); the DLSS tonemap compute reads it as
    // a storage image and writes the RGBA8 beauty. STORAGE for DLSS UAV write +
    // tonemap read; SAMPLED/TRANSFER_SRC for flexibility/debug readback.
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType     = VK_IMAGE_TYPE_2D;
        imageInfo.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
        // OUTPUT/display resolution: DLSS-RR upscales the render-res guide buffers
        // into this full-res target, which the tonemap pass reads.
        imageInfo.extent        = {m_outW, m_outH, 1};
        imageInfo.mipLevels     = 1;
        imageInfo.arrayLayers   = 1;
        imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_dlssColorOutImage) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(m_device, m_dlssColorOutImage, &memReqs);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize  = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_dlssColorOutMemory) != VK_SUCCESS) return false;
        vkBindImageMemory(m_device, m_dlssColorOutImage, m_dlssColorOutMemory, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image                       = m_dlssColorOutImage;
        viewInfo.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format                      = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_dlssColorOutView) != VK_SUCCESS) return false;

        m_dlssColorOutFirstFrame = true;
    }
#endif  // OHAO_DLSS_ENABLED

    return true;
}

void PathTracer::destroyImages() {
#ifdef OHAO_DLSS_ENABLED
    if (m_dlssColorOutView)   { vkDestroyImageView(m_device, m_dlssColorOutView, nullptr);   m_dlssColorOutView   = VK_NULL_HANDLE; }
    if (m_dlssColorOutImage)  { vkDestroyImage(m_device, m_dlssColorOutImage, nullptr);      m_dlssColorOutImage  = VK_NULL_HANDLE; }
    if (m_dlssColorOutMemory) { vkFreeMemory(m_device, m_dlssColorOutMemory, nullptr);       m_dlssColorOutMemory = VK_NULL_HANDLE; }
    m_dlssColorOutFirstFrame = true;
#endif
    if (m_accumView) { vkDestroyImageView(m_device, m_accumView, nullptr); m_accumView = VK_NULL_HANDLE; }
    if (m_accumBuffer) { vkDestroyImage(m_device, m_accumBuffer, nullptr); m_accumBuffer = VK_NULL_HANDLE; }
    if (m_accumMemory) { vkFreeMemory(m_device, m_accumMemory, nullptr); m_accumMemory = VK_NULL_HANDLE; }

    if (m_outputView) { vkDestroyImageView(m_device, m_outputView, nullptr); m_outputView = VK_NULL_HANDLE; }
    if (m_outputImage) { vkDestroyImage(m_device, m_outputImage, nullptr); m_outputImage = VK_NULL_HANDLE; }
    if (m_outputMemory) { vkFreeMemory(m_device, m_outputMemory, nullptr); m_outputMemory = VK_NULL_HANDLE; }

    if (m_albedoAOVView) { vkDestroyImageView(m_device, m_albedoAOVView, nullptr); m_albedoAOVView = VK_NULL_HANDLE; }
    if (m_albedoAOV) { vkDestroyImage(m_device, m_albedoAOV, nullptr); m_albedoAOV = VK_NULL_HANDLE; }
    if (m_albedoAOVMemory) { vkFreeMemory(m_device, m_albedoAOVMemory, nullptr); m_albedoAOVMemory = VK_NULL_HANDLE; }

    if (m_normalAOVView) { vkDestroyImageView(m_device, m_normalAOVView, nullptr); m_normalAOVView = VK_NULL_HANDLE; }
    if (m_normalAOV) { vkDestroyImage(m_device, m_normalAOV, nullptr); m_normalAOV = VK_NULL_HANDLE; }
    if (m_normalAOVMemory) { vkFreeMemory(m_device, m_normalAOVMemory, nullptr); m_normalAOVMemory = VK_NULL_HANDLE; }

    if (m_motionVectorView)    { vkDestroyImageView(m_device, m_motionVectorView, nullptr); m_motionVectorView = VK_NULL_HANDLE; }
    if (m_motionVectorImage)   { vkDestroyImage(m_device, m_motionVectorImage, nullptr);    m_motionVectorImage = VK_NULL_HANDLE; }
    if (m_motionVectorMemory)  { vkFreeMemory(m_device, m_motionVectorMemory, nullptr);     m_motionVectorMemory = VK_NULL_HANDLE; }

    if (m_depthAOVView)      { vkDestroyImageView(m_device, m_depthAOVView, nullptr);   m_depthAOVView = VK_NULL_HANDLE; }
    if (m_depthAOVImage)     { vkDestroyImage(m_device, m_depthAOVImage, nullptr);      m_depthAOVImage = VK_NULL_HANDLE; }
    if (m_depthAOVMemory)    { vkFreeMemory(m_device, m_depthAOVMemory, nullptr);       m_depthAOVMemory = VK_NULL_HANDLE; }

    if (m_roughnessAOVView)  { vkDestroyImageView(m_device, m_roughnessAOVView, nullptr); m_roughnessAOVView = VK_NULL_HANDLE; }
    if (m_roughnessAOVImage) { vkDestroyImage(m_device, m_roughnessAOVImage, nullptr);    m_roughnessAOVImage = VK_NULL_HANDLE; }
    if (m_roughnessAOVMemory){ vkFreeMemory(m_device, m_roughnessAOVMemory, nullptr);     m_roughnessAOVMemory = VK_NULL_HANDLE; }

    if (m_diffuseRadianceView)    { vkDestroyImageView(m_device, m_diffuseRadianceView, nullptr);   m_diffuseRadianceView = VK_NULL_HANDLE; }
    if (m_diffuseRadianceImage)   { vkDestroyImage(m_device, m_diffuseRadianceImage, nullptr);      m_diffuseRadianceImage = VK_NULL_HANDLE; }
    if (m_diffuseRadianceMemory)  { vkFreeMemory(m_device, m_diffuseRadianceMemory, nullptr);       m_diffuseRadianceMemory = VK_NULL_HANDLE; }

    if (m_specularRadianceView)   { vkDestroyImageView(m_device, m_specularRadianceView, nullptr);  m_specularRadianceView = VK_NULL_HANDLE; }
    if (m_specularRadianceImage)  { vkDestroyImage(m_device, m_specularRadianceImage, nullptr);     m_specularRadianceImage = VK_NULL_HANDLE; }
    if (m_specularRadianceMemory) { vkFreeMemory(m_device, m_specularRadianceMemory, nullptr);      m_specularRadianceMemory = VK_NULL_HANDLE; }

    if (m_diffAlbedoView)    { vkDestroyImageView(m_device, m_diffAlbedoView, nullptr);   m_diffAlbedoView = VK_NULL_HANDLE; }
    if (m_diffAlbedoImage)   { vkDestroyImage(m_device, m_diffAlbedoImage, nullptr);      m_diffAlbedoImage = VK_NULL_HANDLE; }
    if (m_diffAlbedoMemory)  { vkFreeMemory(m_device, m_diffAlbedoMemory, nullptr);       m_diffAlbedoMemory = VK_NULL_HANDLE; }

    if (m_specColorView)     { vkDestroyImageView(m_device, m_specColorView, nullptr);   m_specColorView = VK_NULL_HANDLE; }
    if (m_specColorImage)    { vkDestroyImage(m_device, m_specColorImage, nullptr);      m_specColorImage = VK_NULL_HANDLE; }
    if (m_specColorMemory)   { vkFreeMemory(m_device, m_specColorMemory, nullptr);       m_specColorMemory = VK_NULL_HANDLE; }

    if (m_normalRoughnessView)   { vkDestroyImageView(m_device, m_normalRoughnessView, nullptr);  m_normalRoughnessView = VK_NULL_HANDLE; }
    if (m_normalRoughnessImage)  { vkDestroyImage(m_device, m_normalRoughnessImage, nullptr);     m_normalRoughnessImage = VK_NULL_HANDLE; }
    if (m_normalRoughnessMemory) { vkFreeMemory(m_device, m_normalRoughnessMemory, nullptr);      m_normalRoughnessMemory = VK_NULL_HANDLE; }

    if (m_outDiffRadianceView)    { vkDestroyImageView(m_device, m_outDiffRadianceView, nullptr);   m_outDiffRadianceView = VK_NULL_HANDLE; }
    if (m_outDiffRadianceImage)   { vkDestroyImage(m_device, m_outDiffRadianceImage, nullptr);      m_outDiffRadianceImage = VK_NULL_HANDLE; }
    if (m_outDiffRadianceMemory)  { vkFreeMemory(m_device, m_outDiffRadianceMemory, nullptr);       m_outDiffRadianceMemory = VK_NULL_HANDLE; }

    if (m_outSpecRadianceView)    { vkDestroyImageView(m_device, m_outSpecRadianceView, nullptr);   m_outSpecRadianceView = VK_NULL_HANDLE; }
    if (m_outSpecRadianceImage)   { vkDestroyImage(m_device, m_outSpecRadianceImage, nullptr);      m_outSpecRadianceImage = VK_NULL_HANDLE; }
    if (m_outSpecRadianceMemory)  { vkFreeMemory(m_device, m_outSpecRadianceMemory, nullptr);       m_outSpecRadianceMemory = VK_NULL_HANDLE; }

    if (m_nrdComposedView)   { vkDestroyImageView(m_device, m_nrdComposedView, nullptr);   m_nrdComposedView   = VK_NULL_HANDLE; }
    if (m_nrdComposedImage)  { vkDestroyImage(m_device, m_nrdComposedImage, nullptr);      m_nrdComposedImage  = VK_NULL_HANDLE; }
    if (m_nrdComposedMemory) { vkFreeMemory(m_device, m_nrdComposedMemory, nullptr);       m_nrdComposedMemory = VK_NULL_HANDLE; }

    if (m_nrdTonemappedView)   { vkDestroyImageView(m_device, m_nrdTonemappedView, nullptr);   m_nrdTonemappedView   = VK_NULL_HANDLE; }
    if (m_nrdTonemappedImage)  { vkDestroyImage(m_device, m_nrdTonemappedImage, nullptr);      m_nrdTonemappedImage  = VK_NULL_HANDLE; }
    if (m_nrdTonemappedMemory) { vkFreeMemory(m_device, m_nrdTonemappedMemory, nullptr);       m_nrdTonemappedMemory = VK_NULL_HANDLE; }

    // 4.J: pre-DoF LDR teardown.
    if (m_preDofLdrView)   { vkDestroyImageView(m_device, m_preDofLdrView, nullptr);   m_preDofLdrView   = VK_NULL_HANDLE; }
    if (m_preDofLdrImage)  { vkDestroyImage(m_device, m_preDofLdrImage, nullptr);      m_preDofLdrImage  = VK_NULL_HANDLE; }
    if (m_preDofLdrMemory) { vkFreeMemory(m_device, m_preDofLdrMemory, nullptr);       m_preDofLdrMemory = VK_NULL_HANDLE; }
    m_preDofFirstFrame = true;

    // 4.G: bloom mip chain teardown.
    for (uint32_t mip = 0; mip < 3; ++mip) {
        if (m_bloomMipViews[mip])  { vkDestroyImageView(m_device, m_bloomMipViews[mip], nullptr);   m_bloomMipViews[mip]  = VK_NULL_HANDLE; }
        if (m_bloomMipImages[mip]) { vkDestroyImage(m_device, m_bloomMipImages[mip], nullptr);      m_bloomMipImages[mip] = VK_NULL_HANDLE; }
        if (m_bloomMipMemory[mip]) { vkFreeMemory(m_device, m_bloomMipMemory[mip], nullptr);        m_bloomMipMemory[mip] = VK_NULL_HANDLE; }
        m_bloomMipWidth[mip]   = 0;
        m_bloomMipHeight[mip]  = 0;
        m_bloomFirstFrame[mip] = true;
    }
    if (m_bloomSampler) { vkDestroySampler(m_device, m_bloomSampler, nullptr); m_bloomSampler = VK_NULL_HANDLE; }

    for (uint32_t i = 0; i < 2; ++i) {
        if (m_surfaceHistoryViews[i]) { vkDestroyImageView(m_device, m_surfaceHistoryViews[i], nullptr); m_surfaceHistoryViews[i] = VK_NULL_HANDLE; }
        if (m_surfaceHistoryImages[i]) { vkDestroyImage(m_device, m_surfaceHistoryImages[i], nullptr); m_surfaceHistoryImages[i] = VK_NULL_HANDLE; }
        if (m_surfaceHistoryMemory[i]) { vkFreeMemory(m_device, m_surfaceHistoryMemory[i], nullptr); m_surfaceHistoryMemory[i] = VK_NULL_HANDLE; }
        m_surfaceHistoryInitialized[i] = false;

        if (m_shadingHistoryViews[i]) { vkDestroyImageView(m_device, m_shadingHistoryViews[i], nullptr); m_shadingHistoryViews[i] = VK_NULL_HANDLE; }
        if (m_shadingHistoryImages[i]) { vkDestroyImage(m_device, m_shadingHistoryImages[i], nullptr); m_shadingHistoryImages[i] = VK_NULL_HANDLE; }
        if (m_shadingHistoryMemory[i]) { vkFreeMemory(m_device, m_shadingHistoryMemory[i], nullptr); m_shadingHistoryMemory[i] = VK_NULL_HANDLE; }
        m_shadingHistoryInitialized[i] = false;
    }
    m_surfaceHistoryWriteIndex = 0;
    m_shadingHistoryWriteIndex = 0;

    // ReSTIR GI reservoir ping-pong
    for (uint32_t plane = 0; plane < 3; ++plane) {
        for (uint32_t i = 0; i < 2; ++i) {
            if (m_giReservoirViews[plane][i])  { vkDestroyImageView(m_device, m_giReservoirViews[plane][i], nullptr);  m_giReservoirViews[plane][i]  = VK_NULL_HANDLE; }
            if (m_giReservoirImages[plane][i]) { vkDestroyImage(m_device, m_giReservoirImages[plane][i], nullptr);     m_giReservoirImages[plane][i] = VK_NULL_HANDLE; }
            if (m_giReservoirMemory[plane][i]) { vkFreeMemory(m_device, m_giReservoirMemory[plane][i], nullptr);       m_giReservoirMemory[plane][i] = VK_NULL_HANDLE; }
        }
    }
    m_giReservoirInitialized[0] = false;
    m_giReservoirInitialized[1] = false;
    m_giReservoirWriteIndex = 0;
}

// ─── Material buffer ─────────────────────────────────────────────────

bool PathTracer::createMaterialBuffer() {
    // Pre-allocate material buffer for up to 256 instances (vec4 each)
    // HOST_VISIBLE so CPU can write albedo colors directly
    const uint32_t maxInstances = 256;
    m_materialData.resize(maxInstances, glm::vec4(0.8f, 0.8f, 0.8f, 1.0f));

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = maxInstances * sizeof(glm::vec4);
    bufInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &m_materialBuffer) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, m_materialBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_materialMemory) != VK_SUCCESS) return false;
    vkBindBufferMemory(m_device, m_materialBuffer, m_materialMemory, 0);

    // Write default albedos
    void* mapped;
    vkMapMemory(m_device, m_materialMemory, 0, maxInstances * sizeof(glm::vec4), 0, &mapped);
    memcpy(mapped, m_materialData.data(), maxInstances * sizeof(glm::vec4));
    vkUnmapMemory(m_device, m_materialMemory);

    return true;
}

void PathTracer::setMaterialAlbedos(const std::vector<glm::vec3>& albedos) {
    for (size_t i = 0; i < albedos.size() && i < m_materialData.size(); i++) {
        m_materialData[i] = glm::vec4(albedos[i], 1.0f);
    }
    void* mapped;
    VkDeviceSize size = m_materialData.size() * sizeof(glm::vec4);
    vkMapMemory(m_device, m_materialMemory, 0, size, 0, &mapped);
    memcpy(mapped, m_materialData.data(), size);
    vkUnmapMemory(m_device, m_materialMemory);
}

void PathTracer::setMaterialData(const std::vector<glm::vec4>& materials) {
    for (size_t i = 0; i < materials.size() && i < m_materialData.size(); i++) {
        m_materialData[i] = materials[i];
    }
    void* mapped;
    VkDeviceSize size = m_materialData.size() * sizeof(glm::vec4);
    vkMapMemory(m_device, m_materialMemory, 0, size, 0, &mapped);
    memcpy(mapped, m_materialData.data(), size);
    vkUnmapMemory(m_device, m_materialMemory);
}

} // namespace ohao
