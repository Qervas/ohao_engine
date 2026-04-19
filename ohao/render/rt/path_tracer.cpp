#include "path_tracer.hpp"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <iostream>

namespace ohao {

namespace {
constexpr uint32_t kPTFlagEnableAOVs = 1u << 0;
constexpr uint32_t kPTFlagEnableInternalDenoise = 1u << 1;
constexpr uint32_t kPTFlagEnableFireflyClamp = 1u << 2;
}

PathTracer::~PathTracer() {
    destroy();
}

// ─── Function pointer loading ─────────────────────────────────────────

bool PathTracer::loadFunctionPointers() {
    vkCreateRayTracingPipelinesKHR =
        (PFN_vkCreateRayTracingPipelinesKHR)vkGetDeviceProcAddr(m_device, "vkCreateRayTracingPipelinesKHR");
    vkGetRayTracingShaderGroupHandlesKHR =
        (PFN_vkGetRayTracingShaderGroupHandlesKHR)vkGetDeviceProcAddr(m_device, "vkGetRayTracingShaderGroupHandlesKHR");
    vkCmdTraceRaysKHR =
        (PFN_vkCmdTraceRaysKHR)vkGetDeviceProcAddr(m_device, "vkCmdTraceRaysKHR");
    vkGetBufferDeviceAddressFn =
        (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(m_device, "vkGetBufferDeviceAddress");

    return vkCreateRayTracingPipelinesKHR && vkGetRayTracingShaderGroupHandlesKHR &&
           vkCmdTraceRaysKHR && vkGetBufferDeviceAddressFn;
}

// ─── Helpers ──────────────────────────────────────────────────────────

uint32_t PathTracer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return UINT32_MAX;
}

VkDeviceAddress PathTracer::getBufferDeviceAddress(VkBuffer buffer) {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddressFn(m_device, &info);
}

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

// ─── Initialization ──────────────────────────────────────────────────

bool PathTracer::init(VkDevice device, VkPhysicalDevice physicalDevice,
                       uint32_t width, uint32_t height) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_width = width;
    m_height = height;
    m_sampleIndex = 0;
    m_historyFrameCount = 0;

    if (!loadFunctionPointers()) {
        std::cerr << "[PathTracer] Failed to load RT function pointers" << std::endl;
        return false;
    }

    if (!createImages()) {
        std::cerr << "[PathTracer] Failed to create output images" << std::endl;
        return false;
    }

    if (!createMaterialBuffer()) {
        std::cerr << "[PathTracer] Failed to create material buffer" << std::endl;
        return false;
    }

    if (!createDescriptorResources()) {
        std::cerr << "[PathTracer] Failed to create descriptor resources" << std::endl;
        return false;
    }

    if (!createRTPipeline()) {
        std::cerr << "[PathTracer] Failed to create RT pipeline" << std::endl;
        return false;
    }

    if (!createShaderBindingTable()) {
        std::cerr << "[PathTracer] Failed to create SBT" << std::endl;
        return false;
    }

    std::cout << "[PathTracer] Initialized (" << width << "x" << height << ")" << std::endl;
    return true;
}

// ─── Image creation ──────────────────────────────────────────────────

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
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT;
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
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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

    // ---- Feature 3.B: Roughness AOV (R8 UNORM) ----
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8_UNORM;
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
        viewInfo.format = VK_FORMAT_R8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_roughnessAOVView) != VK_SUCCESS) return false;
    }

    return true;
}

void PathTracer::destroyImages() {
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

// ─── Descriptor resources ────────────────────────────────────────────

bool PathTracer::createDescriptorResources() {
    // Layout: RT resources + realtime history validation images
    //   0: TLAS (acceleration structure)           — RAYGEN
    //   1: Accumulation buffer (storage image)     — RAYGEN   (RGBA32F)
    //   2: Output image (storage image)            — RAYGEN   (RGBA8)
    //   3: Material buffer SSBO                    — RAYGEN + CLOSEST_HIT
    //   4: Normal buffer SSBO (vec4 per vertex)    — CLOSEST_HIT
    //   5: Index buffer SSBO (uint per index)      — CLOSEST_HIT
    //   6: Albedo AOV (storage image)              — RAYGEN   (RGBA32F)
    //   7: Normal AOV (storage image)              — RAYGEN   (RGBA32F)
    VkDescriptorSetLayoutBinding bindings[22] = {};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    bindings[7].binding = 7;
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // 8: UV buffer SSBO
    bindings[8].binding = 8;
    bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[8].descriptorCount = 1;
    bindings[8].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    // 9: Material ID buffer (per-triangle)
    bindings[9].binding = 9;
    bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[9].descriptorCount = 1;
    bindings[9].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    // 10: Material color buffer (per-material)
    bindings[10].binding = 10;
    bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[10].descriptorCount = 1;
    bindings[10].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    // 11: Light buffer (SSBO) — accessed by raygen + miss
    bindings[11].binding = 11;
    bindings[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[11].descriptorCount = 1;
    bindings[11].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;

    // 12: Bindless textures — accessed by closest-hit + miss (env map)
    bindings[12].binding = 12;
    bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[12].descriptorCount = m_maxBindlessTextures;
    bindings[12].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    // 13: Previous-frame surface history image
    bindings[13].binding = 13;
    bindings[13].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[13].descriptorCount = 1;
    bindings[13].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // 14: Current-frame surface history image
    bindings[14].binding = 14;
    bindings[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[14].descriptorCount = 1;
    bindings[14].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // 15: Previous-frame shading history image
    bindings[15].binding = 15;
    bindings[15].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[15].descriptorCount = 1;
    bindings[15].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // 16: Current-frame shading history image
    bindings[16].binding = 16;
    bindings[16].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[16].descriptorCount = 1;
    bindings[16].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // 17: Env marginal CDF (storage buffer) — accessed by raygen + miss
    bindings[17].binding = 17;
    bindings[17].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[17].descriptorCount = 1;
    bindings[17].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;

    // 18: Env conditional CDF (storage buffer) — accessed by raygen + miss
    bindings[18].binding = 18;
    bindings[18].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[18].descriptorCount = 1;
    bindings[18].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;

    // 19: Motion vector AOV (RG16F storage image) — Sub-plan 3.A
    bindings[19].binding = 19;
    bindings[19].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[19].descriptorCount = 1;
    bindings[19].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Binding 20: depth AOV (R32F storage image) — Sub-plan 3.B
    bindings[20].binding         = 20;
    bindings[20].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[20].descriptorCount = 1;
    bindings[20].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Binding 21: roughness AOV (R8 UNORM storage image) — Sub-plan 3.B
    bindings[21].binding         = 21;
    bindings[21].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[21].descriptorCount = 1;
    bindings[21].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    // Enable bindless: variable count on the LAST binding only
    VkDescriptorBindingFlags bindingFlags[22] = {};
    bindingFlags[12] = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT
                     | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
                     | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount = 22;
    flagsInfo.pBindingFlags = bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext = &flagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 22;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
        return false;

    // Pool — allocate enough for bindless textures
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 11},  // +1 MV (3.A), +2 depth/roughness (3.B)
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 9},  // +2 for env CDF marginal + conditional
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_maxBindlessTextures},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        return false;

    // Allocate set with variable descriptor count for bindless textures
    uint32_t variableCount = m_maxBindlessTextures;
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableInfo{};
    variableInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    variableInfo.descriptorSetCount = 1;
    variableInfo.pDescriptorCounts = &variableCount;

    VkDescriptorSetAllocateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.pNext = &variableInfo;
    setInfo.descriptorPool = m_descriptorPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &m_descriptorSetLayout;

    return vkAllocateDescriptorSets(m_device, &setInfo, &m_descriptorSet) == VK_SUCCESS;
}

// ─── RT Pipeline ─────────────────────────────────────────────────────

bool PathTracer::createRTPipeline() {
    // Load shader SPVs
    auto rgenCode = readFile(m_shaderSet.raygenSpv);
    auto rmissCode = readFile(m_shaderSet.missSpv);
    auto rchitCode = readFile(m_shaderSet.closestHitSpv);
    auto rahitCode = readFile(m_shaderSet.anyHitSpv);

    if (rgenCode.empty() || rmissCode.empty() || rchitCode.empty()) {
        std::cerr << "[PathTracer] Failed to load RT shader SPVs" << std::endl;
        return false;
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

    // Shader stages: 0=raygen, 1=miss, 2=closest-hit, 3=any-hit (optional)
    uint32_t stageCount = hasAnyHit ? 4 : 3;
    VkPipelineShaderStageCreateInfo stages[4] = {};

    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[0].module = rgenModule;
    stages[0].pName = "main";

    // Specialization constant: sampler type baked into the raygen SPIR-V
    // (see shaders/includes/rt/sampler_api.glsl, layout(constant_id=0)).
    VkSpecializationMapEntry samplerEntry{};
    samplerEntry.constantID = kSamplerSpecConstantId;
    samplerEntry.offset = 0;
    samplerEntry.size = sizeof(uint32_t);

    uint32_t samplerTypeVal = static_cast<uint32_t>(m_renderSettings.samplerType);

    VkSpecializationInfo samplerSpecInfo{};
    samplerSpecInfo.mapEntryCount = 1;
    samplerSpecInfo.pMapEntries = &samplerEntry;
    samplerSpecInfo.dataSize = sizeof(uint32_t);
    samplerSpecInfo.pData = &samplerTypeVal;

    stages[0].pSpecializationInfo = &samplerSpecInfo;

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[1].module = rmissModule;
    stages[1].pName = "main";

    stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[2].module = rchitModule;
    stages[2].pName = "main";

    if (hasAnyHit) {
        stages[3].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[3].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
        stages[3].module = rahitModule;
        stages[3].pName = "main";
    }

    // Shader groups: 3 groups (any-hit is part of the hit group, not a separate group)
    VkRayTracingShaderGroupCreateInfoKHR groups[3] = {};

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
        std::cerr << "[PathTracer] Failed to create pipeline layout" << std::endl;
        vkDestroyShaderModule(m_device, rgenModule, nullptr);
        vkDestroyShaderModule(m_device, rmissModule, nullptr);
        vkDestroyShaderModule(m_device, rchitModule, nullptr);
        if (rahitModule) vkDestroyShaderModule(m_device, rahitModule, nullptr);
        return false;
    }

    // Create RT pipeline
    VkRayTracingPipelineCreateInfoKHR pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipelineInfo.stageCount = stageCount;
    pipelineInfo.pStages = stages;
    pipelineInfo.groupCount = 3;
    pipelineInfo.pGroups = groups;
    pipelineInfo.maxPipelineRayRecursionDepth = 2;  // path tracing needs bounce recursion
    pipelineInfo.layout = m_pipelineLayout;

    VkResult result = vkCreateRayTracingPipelinesKHR(
        m_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_rtPipeline);

    // Cleanup shader modules (no longer needed after pipeline creation)
    vkDestroyShaderModule(m_device, rgenModule, nullptr);
    vkDestroyShaderModule(m_device, rmissModule, nullptr);
    vkDestroyShaderModule(m_device, rchitModule, nullptr);
    if (rahitModule) vkDestroyShaderModule(m_device, rahitModule, nullptr);

    if (result != VK_SUCCESS) {
        std::cerr << "[PathTracer] Failed to create RT pipeline (err=" << result << ")" << std::endl;
        return false;
    }

    std::cout << "[PathTracer] RT pipeline created" << std::endl;
    return true;
}

// ─── Shader Binding Table ────────────────────────────────────────────

bool PathTracer::createShaderBindingTable() {
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

    // 3 groups: rgen, miss, closest-hit
    uint32_t groupCount = 3;

    // Get shader group handles from the pipeline
    std::vector<uint8_t> handles(groupCount * handleSize);
    if (vkGetRayTracingShaderGroupHandlesKHR(m_device, m_rtPipeline, 0, groupCount,
                                              handles.size(), handles.data()) != VK_SUCCESS) {
        std::cerr << "[PathTracer] Failed to get shader group handles" << std::endl;
        return false;
    }

    // Each region must be baseAlignment-aligned
    uint32_t rgenSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t missSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t hitSize = (handleSizeAligned + baseAlignment - 1) & ~(baseAlignment - 1);
    uint32_t totalSize = rgenSize + missSize + hitSize;

    // Create SBT buffer
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = totalSize;
    bufInfo.usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &m_sbtBuffer) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, m_sbtBuffer, &memReqs);

    VkMemoryAllocateFlagsInfo allocFlags{};
    allocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    allocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &allocFlags;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_sbtMemory) != VK_SUCCESS) return false;
    vkBindBufferMemory(m_device, m_sbtBuffer, m_sbtMemory, 0);

    // Write handles into SBT buffer
    void* mapped;
    vkMapMemory(m_device, m_sbtMemory, 0, totalSize, 0, &mapped);
    memset(mapped, 0, totalSize);

    uint8_t* dst = static_cast<uint8_t*>(mapped);
    // Rgen at offset 0
    memcpy(dst, handles.data() + 0 * handleSize, handleSize);
    // Miss at offset rgenSize
    memcpy(dst + rgenSize, handles.data() + 1 * handleSize, handleSize);
    // Closest-hit at offset rgenSize + missSize
    memcpy(dst + rgenSize + missSize, handles.data() + 2 * handleSize, handleSize);

    vkUnmapMemory(m_device, m_sbtMemory);

    // Set up strided device address regions
    VkDeviceAddress sbtAddr = getBufferDeviceAddress(m_sbtBuffer);

    m_rgenRegion.deviceAddress = sbtAddr;
    m_rgenRegion.stride = handleSizeAligned;
    m_rgenRegion.size = rgenSize;

    m_missRegion.deviceAddress = sbtAddr + rgenSize;
    m_missRegion.stride = handleSizeAligned;
    m_missRegion.size = missSize;

    m_hitRegion.deviceAddress = sbtAddr + rgenSize + missSize;
    m_hitRegion.stride = handleSizeAligned;
    m_hitRegion.size = hitSize;

    m_callRegion = {};  // not used

    std::cout << "[PathTracer] SBT created (handleSize=" << handleSize
              << ", aligned=" << handleSizeAligned << ")" << std::endl;
    return true;
}

// ─── Render ──────────────────────────────────────────────────────────

void PathTracer::render(VkCommandBuffer cmd, RTAccelerationStructure* accel,
                         const glm::mat4& view, const glm::mat4& proj,
                         const glm::vec3& lightPos, float lightIntensity,
                         const glm::vec3& lightColor, float lightRadius) {
    if (!m_rtPipeline || !accel || !accel->getTLAS()) return;

    // --- Update descriptor set with current frame's data ---

    // Binding 0: TLAS
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    VkAccelerationStructureKHR tlas = accel->getTLAS();
    asWrite.pAccelerationStructures = &tlas;

    // Binding 1: Accumulation buffer (storage image, RGBA32F)
    VkDescriptorImageInfo accumInfo{};
    accumInfo.imageView = m_accumView;
    accumInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Binding 2: Output image (storage image, RGBA8)
    VkDescriptorImageInfo outputInfo{};
    outputInfo.imageView = m_outputView;
    outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Binding 3: Material buffer SSBO
    VkDescriptorBufferInfo materialInfo{};
    materialInfo.buffer = m_materialBuffer;
    materialInfo.offset = 0;
    materialInfo.range = m_materialData.size() * sizeof(glm::vec4);

    // Binding 4: Normal buffer SSBO
    VkDescriptorBufferInfo normalBufInfo{};
    normalBufInfo.buffer = m_normalBuffer != VK_NULL_HANDLE ? m_normalBuffer : m_materialBuffer;
    normalBufInfo.offset = 0;
    normalBufInfo.range = VK_WHOLE_SIZE;

    // Binding 5: Index buffer SSBO
    VkDescriptorBufferInfo indexBufInfo{};
    indexBufInfo.buffer = m_indexBuffer != VK_NULL_HANDLE ? m_indexBuffer : m_materialBuffer;
    indexBufInfo.offset = 0;
    indexBufInfo.range = VK_WHOLE_SIZE;

    // Binding 6: Albedo AOV (storage image, RGBA32F)
    VkDescriptorImageInfo albedoAOVInfo{};
    albedoAOVInfo.imageView = m_albedoAOVView;
    albedoAOVInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Binding 7: Normal AOV (storage image, RGBA32F)
    VkDescriptorImageInfo normalAOVInfo{};
    normalAOVInfo.imageView = m_normalAOVView;
    normalAOVInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    const uint32_t prevSurfaceHistoryIndex = 1u - m_surfaceHistoryWriteIndex;
    VkDescriptorImageInfo prevSurfaceHistoryInfo{};
    prevSurfaceHistoryInfo.imageView = m_surfaceHistoryViews[prevSurfaceHistoryIndex];
    prevSurfaceHistoryInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo currSurfaceHistoryInfo{};
    currSurfaceHistoryInfo.imageView = m_surfaceHistoryViews[m_surfaceHistoryWriteIndex];
    currSurfaceHistoryInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    const uint32_t prevShadingHistoryIndex = 1u - m_shadingHistoryWriteIndex;
    VkDescriptorImageInfo prevShadingHistoryInfo{};
    prevShadingHistoryInfo.imageView = m_shadingHistoryViews[prevShadingHistoryIndex];
    prevShadingHistoryInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo currShadingHistoryInfo{};
    currShadingHistoryInfo.imageView = m_shadingHistoryViews[m_shadingHistoryWriteIndex];
    currShadingHistoryInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[22] = {};

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[0].pNext = &asWrite;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &accumInfo;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_descriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &outputInfo;

    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = m_descriptorSet;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &materialInfo;

    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = m_descriptorSet;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].pBufferInfo = &normalBufInfo;

    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = m_descriptorSet;
    writes[5].dstBinding = 5;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[5].pBufferInfo = &indexBufInfo;

    writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = m_descriptorSet;
    writes[6].dstBinding = 6;
    writes[6].descriptorCount = 1;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[6].pImageInfo = &albedoAOVInfo;

    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = m_descriptorSet;
    writes[7].dstBinding = 7;
    writes[7].descriptorCount = 1;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[7].pImageInfo = &normalAOVInfo;

    // Binding 8: UV buffer
    VkDescriptorBufferInfo uvBufInfo{};
    uvBufInfo.buffer = m_uvBuffer != VK_NULL_HANDLE ? m_uvBuffer : m_materialBuffer;
    uvBufInfo.offset = 0;
    uvBufInfo.range = VK_WHOLE_SIZE;

    writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[8].dstSet = m_descriptorSet;
    writes[8].dstBinding = 8;
    writes[8].descriptorCount = 1;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[8].pBufferInfo = &uvBufInfo;

    // Binding 9: Material ID buffer
    VkDescriptorBufferInfo matIDBufInfo{};
    matIDBufInfo.buffer = m_matIDBuffer != VK_NULL_HANDLE ? m_matIDBuffer : m_materialBuffer;
    matIDBufInfo.offset = 0;
    matIDBufInfo.range = VK_WHOLE_SIZE;

    writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[9].dstSet = m_descriptorSet;
    writes[9].dstBinding = 9;
    writes[9].descriptorCount = 1;
    writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[9].pBufferInfo = &matIDBufInfo;

    // Binding 10: Material color buffer
    VkDescriptorBufferInfo matColorBufInfo{};
    matColorBufInfo.buffer = m_matColorBuffer != VK_NULL_HANDLE ? m_matColorBuffer : m_materialBuffer;
    matColorBufInfo.offset = 0;
    matColorBufInfo.range = VK_WHOLE_SIZE;

    writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[10].dstSet = m_descriptorSet;
    writes[10].dstBinding = 10;
    writes[10].descriptorCount = 1;
    writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[10].pBufferInfo = &matColorBufInfo;

    // Binding 11: Light buffer (SSBO)
    uint32_t writeCount = 11;
    VkDescriptorBufferInfo lightBufInfo{};
    if (m_lightBuffer != VK_NULL_HANDLE) {
        lightBufInfo.buffer = m_lightBuffer;
        lightBufInfo.offset = 0;
        lightBufInfo.range = VK_WHOLE_SIZE;
        writes[11].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[11].dstSet = m_descriptorSet;
        writes[11].dstBinding = 11;
        writes[11].descriptorCount = 1;
        writes[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[11].pBufferInfo = &lightBufInfo;
        writeCount = 12;
    }

    // Binding 12: Bindless textures (MUST be last — variable count)
    std::vector<VkDescriptorImageInfo> texInfos;
    if (!m_bindlessImageViews.empty()) {
        texInfos.resize(m_bindlessTextureCount);
        for (uint32_t i = 0; i < m_bindlessTextureCount; i++) {
            texInfos[i].imageView = m_bindlessImageViews[i];
            texInfos[i].sampler = (i < m_bindlessSamplers.size()) ? m_bindlessSamplers[i] : m_defaultSampler;
            texInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[writeCount].dstSet = m_descriptorSet;
        writes[writeCount].dstBinding = 12;
        writes[writeCount].descriptorCount = m_bindlessTextureCount;
        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[writeCount].pImageInfo = texInfos.data();
        writeCount++;
    }

    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = m_descriptorSet;
    writes[writeCount].dstBinding = 13;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo = &prevSurfaceHistoryInfo;
    writeCount++;

    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = m_descriptorSet;
    writes[writeCount].dstBinding = 14;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo = &currSurfaceHistoryInfo;
    writeCount++;

    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = m_descriptorSet;
    writes[writeCount].dstBinding = 15;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo = &prevShadingHistoryInfo;
    writeCount++;

    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = m_descriptorSet;
    writes[writeCount].dstBinding = 16;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo = &currShadingHistoryInfo;
    writeCount++;

    // Binding 17: env marginal CDF (only if set — dummy buffers from light_upload guarantee this)
    VkDescriptorBufferInfo envMargInfo{};
    if (m_envMarginalCDFBuffer != VK_NULL_HANDLE) {
        envMargInfo.buffer = m_envMarginalCDFBuffer;
        envMargInfo.offset = 0;
        envMargInfo.range = VK_WHOLE_SIZE;
        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[writeCount].dstSet = m_descriptorSet;
        writes[writeCount].dstBinding = 17;
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[writeCount].pBufferInfo = &envMargInfo;
        writeCount++;
    }

    // Binding 18: env conditional CDF (only if set)
    VkDescriptorBufferInfo envCondInfo{};
    if (m_envConditionalCDFBuffer != VK_NULL_HANDLE) {
        envCondInfo.buffer = m_envConditionalCDFBuffer;
        envCondInfo.offset = 0;
        envCondInfo.range = VK_WHOLE_SIZE;
        writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[writeCount].dstSet = m_descriptorSet;
        writes[writeCount].dstBinding = 18;
        writes[writeCount].descriptorCount = 1;
        writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[writeCount].pBufferInfo = &envCondInfo;
        writeCount++;
    }

    // Binding 19: motion vector AOV
    VkDescriptorImageInfo motionVectorInfo{};
    motionVectorInfo.imageView = m_motionVectorView;
    motionVectorInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet = m_descriptorSet;
    writes[writeCount].dstBinding = 19;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo = &motionVectorInfo;
    writeCount++;

    // Binding 20: depth AOV — Sub-plan 3.B
    VkDescriptorImageInfo depthInfo{};
    depthInfo.imageView   = m_depthAOVView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = m_descriptorSet;
    writes[writeCount].dstBinding      = 20;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &depthInfo;
    writeCount++;

    // Binding 21: roughness AOV — Sub-plan 3.B
    VkDescriptorImageInfo roughInfo{};
    roughInfo.imageView   = m_roughnessAOVView;
    roughInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[writeCount].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[writeCount].dstSet          = m_descriptorSet;
    writes[writeCount].dstBinding      = 21;
    writes[writeCount].descriptorCount = 1;
    writes[writeCount].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[writeCount].pImageInfo      = &roughInfo;
    writeCount++;

    vkUpdateDescriptorSets(m_device, writeCount, writes, 0, nullptr);

    // --- Transition accumulation buffer to GENERAL ---
    // On first accumulated frame, transition from UNDEFINED to clear it;
    // on subsequent frames, keep GENERAL (already there from last trace).
    {
        VkImageMemoryBarrier accumBarrier{};
        accumBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        accumBarrier.srcAccessMask = (m_historyFrameCount == 0) ? 0 : VK_ACCESS_SHADER_WRITE_BIT;
        accumBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        accumBarrier.oldLayout = (m_historyFrameCount == 0) ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        accumBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        accumBarrier.image = m_accumBuffer;
        accumBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            (m_historyFrameCount == 0) ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 1, &accumBarrier);
    }

    // --- Transition output image to GENERAL for storage write ---
    {
        VkImageMemoryBarrier outputBarrier{};
        outputBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        outputBarrier.srcAccessMask = 0;
        outputBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        outputBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        outputBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        outputBarrier.image = m_outputImage;
        outputBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 1, &outputBarrier);
    }

    // --- Transition AOV images to GENERAL for storage write ---
    if (m_renderSettings.enableAuxiliaryAOVs) {
        VkImageMemoryBarrier aovBarriers[5] = {};

        aovBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[0].srcAccessMask = 0;
        aovBarriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[0].image = m_albedoAOV;
        aovBarriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        aovBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[1].srcAccessMask = 0;
        aovBarriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[1].image = m_normalAOV;
        aovBarriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // Sub-plan 3.A: motion vector AOV needs same UNDEFINED→GENERAL transition
        // so raygen can imageStore to it. Safe to transition every frame — the
        // shader overwrites the entire image each pass.
        aovBarriers[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[2].srcAccessMask = 0;
        aovBarriers[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[2].image = m_motionVectorImage;
        aovBarriers[2].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // Sub-plan 3.B: depth AOV barrier
        aovBarriers[3].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[3].srcAccessMask = 0;
        aovBarriers[3].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[3].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[3].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[3].image = m_depthAOVImage;
        aovBarriers[3].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // Sub-plan 3.B: roughness AOV barrier
        aovBarriers[4].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[4].srcAccessMask = 0;
        aovBarriers[4].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[4].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        aovBarriers[4].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[4].image = m_roughnessAOVImage;
        aovBarriers[4].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 5, aovBarriers);
    }

    // --- Transition surface history images to GENERAL ---
    {
        const uint32_t prevSurfaceHistoryIndex = 1u - m_surfaceHistoryWriteIndex;
        VkImageMemoryBarrier historyBarriers[2] = {};

        historyBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        historyBarriers[0].srcAccessMask = m_surfaceHistoryInitialized[prevSurfaceHistoryIndex] ? VK_ACCESS_SHADER_WRITE_BIT : 0;
        historyBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        historyBarriers[0].oldLayout = m_surfaceHistoryInitialized[prevSurfaceHistoryIndex] ? VK_IMAGE_LAYOUT_GENERAL
                                                                                            : VK_IMAGE_LAYOUT_UNDEFINED;
        historyBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        historyBarriers[0].image = m_surfaceHistoryImages[prevSurfaceHistoryIndex];
        historyBarriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        historyBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        historyBarriers[1].srcAccessMask = m_surfaceHistoryInitialized[m_surfaceHistoryWriteIndex] ? VK_ACCESS_SHADER_WRITE_BIT : 0;
        historyBarriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        historyBarriers[1].oldLayout = m_surfaceHistoryInitialized[m_surfaceHistoryWriteIndex] ? VK_IMAGE_LAYOUT_GENERAL
                                                                                                : VK_IMAGE_LAYOUT_UNDEFINED;
        historyBarriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        historyBarriers[1].image = m_surfaceHistoryImages[m_surfaceHistoryWriteIndex];
        historyBarriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 2, historyBarriers);
    }

    {
        const uint32_t prevShadingHistoryIndex = 1u - m_shadingHistoryWriteIndex;
        VkImageMemoryBarrier historyBarriers[2] = {};

        historyBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        historyBarriers[0].srcAccessMask = m_shadingHistoryInitialized[prevShadingHistoryIndex] ? VK_ACCESS_SHADER_WRITE_BIT : 0;
        historyBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        historyBarriers[0].oldLayout = m_shadingHistoryInitialized[prevShadingHistoryIndex] ? VK_IMAGE_LAYOUT_GENERAL
                                                                                            : VK_IMAGE_LAYOUT_UNDEFINED;
        historyBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        historyBarriers[0].image = m_shadingHistoryImages[prevShadingHistoryIndex];
        historyBarriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        historyBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        historyBarriers[1].srcAccessMask = m_shadingHistoryInitialized[m_shadingHistoryWriteIndex] ? VK_ACCESS_SHADER_WRITE_BIT : 0;
        historyBarriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        historyBarriers[1].oldLayout = m_shadingHistoryInitialized[m_shadingHistoryWriteIndex] ? VK_IMAGE_LAYOUT_GENERAL
                                                                                                : VK_IMAGE_LAYOUT_UNDEFINED;
        historyBarriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        historyBarriers[1].image = m_shadingHistoryImages[m_shadingHistoryWriteIndex];
        historyBarriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0, 0, nullptr, 0, nullptr, 2, historyBarriers);
    }

    // --- Bind pipeline and descriptors ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, m_rtPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    // --- Push constants ---
    PTPushConstants pc{};
    pc.invView = glm::inverse(view);
    pc.invProj = glm::inverse(proj);
    pc.prevViewProj = m_prevViewProj;
    pc.params = glm::uvec4(m_width, m_height, m_sampleIndex, m_maxBounces);
    pc.control = glm::uvec4(0u);
    if (m_renderSettings.enableAuxiliaryAOVs) pc.control.x |= kPTFlagEnableAOVs;
    if (m_renderSettings.enableInternalDenoise) pc.control.x |= kPTFlagEnableInternalDenoise;
    if (m_renderSettings.enableFireflyClamp) pc.control.x |= kPTFlagEnableFireflyClamp;
    pc.control.y = m_historyFrameCount;
    pc.control.z = m_viewChangedThisFrame ? 1u : 0u;
    pc.control.w = m_envCDFWidth;
    pc.tuning = glm::vec4(m_renderSettings.fireflyClampLuminance, float(m_envCDFHeight), m_envCDFIntegral, 0.0f);

    // Store current viewProj for next frame's reprojection
    m_prevViewProj = proj * view;

    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                       0, sizeof(PTPushConstants), &pc);

    // --- Trace rays! ---
    vkCmdTraceRaysKHR(cmd, &m_rgenRegion, &m_missRegion, &m_hitRegion, &m_callRegion,
                      m_width, m_height, 1);

    // --- Transition output image to TRANSFER_SRC_OPTIMAL for readback/blit ---
    {
        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer.image = m_outputImage;
        toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    }

    // --- Transition AOV images to TRANSFER_SRC_OPTIMAL for denoiser readback ---
    if (m_renderSettings.enableAuxiliaryAOVs) {
        VkImageMemoryBarrier aovBarriers[2] = {};

        aovBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        aovBarriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        aovBarriers[0].image = m_albedoAOV;
        aovBarriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        aovBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        aovBarriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aovBarriers[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        aovBarriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        aovBarriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        aovBarriers[1].image = m_normalAOV;
        aovBarriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 2, aovBarriers);
    }

    // Advance sample sequence and track accumulation history separately.
    m_sampleIndex++;
    m_historyFrameCount++;
    m_viewChangedThisFrame = false;
    m_surfaceHistoryInitialized[m_surfaceHistoryWriteIndex] = true;
    m_surfaceHistoryWriteIndex = 1u - m_surfaceHistoryWriteIndex;
    m_shadingHistoryInitialized[m_shadingHistoryWriteIndex] = true;
    m_shadingHistoryWriteIndex = 1u - m_shadingHistoryWriteIndex;
}

// ─── Accumulation reset ──────────────────────────────────────────────

void PathTracer::resetAccumulation() {
    m_sampleIndex = 0;
    m_historyFrameCount = 0;
    m_viewChangedThisFrame = false;
    m_surfaceHistoryWriteIndex = 0;
    m_surfaceHistoryInitialized[0] = false;
    m_surfaceHistoryInitialized[1] = false;
    m_shadingHistoryWriteIndex = 0;
    m_shadingHistoryInitialized[0] = false;
    m_shadingHistoryInitialized[1] = false;
}

// ─── Resize ──────────────────────────────────────────────────────────

void PathTracer::resize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;

    // Recreate both images at new resolution
    destroyImages();
    createImages();

    // Reset accumulation since the buffer dimensions changed
    m_sampleIndex = 0;
    m_historyFrameCount = 0;
    m_viewChangedThisFrame = false;
    m_surfaceHistoryWriteIndex = 0;
    m_surfaceHistoryInitialized[0] = false;
    m_surfaceHistoryInitialized[1] = false;
    m_shadingHistoryWriteIndex = 0;
    m_shadingHistoryInitialized[0] = false;
    m_shadingHistoryInitialized[1] = false;
}

// ─── Cleanup ─────────────────────────────────────────────────────────

void PathTracer::destroy() {
    if (!m_device) return;
    vkDeviceWaitIdle(m_device);

    // CDF buffers are owned by VulkanRenderer, not by the path tracer.
    // Clear the cached handles so later destroy calls or reuse don't touch freed memory.
    m_envMarginalCDFBuffer = VK_NULL_HANDLE;
    m_envConditionalCDFBuffer = VK_NULL_HANDLE;
    m_envCDFWidth = 0;
    m_envCDFHeight = 0;
    m_envCDFIntegral = 0.0f;

    // SBT
    if (m_sbtBuffer) { vkDestroyBuffer(m_device, m_sbtBuffer, nullptr); m_sbtBuffer = VK_NULL_HANDLE; }
    if (m_sbtMemory) { vkFreeMemory(m_device, m_sbtMemory, nullptr); m_sbtMemory = VK_NULL_HANDLE; }

    // Pipeline
    if (m_rtPipeline) { vkDestroyPipeline(m_device, m_rtPipeline, nullptr); m_rtPipeline = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }

    // Descriptors
    if (m_descriptorPool) { vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr); m_descriptorPool = VK_NULL_HANDLE; }
    if (m_descriptorSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr); m_descriptorSetLayout = VK_NULL_HANDLE; }
    m_descriptorSet = VK_NULL_HANDLE;  // freed with pool

    // Material buffer
    if (m_materialBuffer) { vkDestroyBuffer(m_device, m_materialBuffer, nullptr); m_materialBuffer = VK_NULL_HANDLE; }
    if (m_materialMemory) { vkFreeMemory(m_device, m_materialMemory, nullptr); m_materialMemory = VK_NULL_HANDLE; }

    // Images
    destroyImages();
}

} // namespace ohao
