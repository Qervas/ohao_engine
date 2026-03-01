#include "cloud_pass.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <array>
#include <algorithm>

namespace ohao {

// ---------------------------------------------------------------------------
// Noise generation
// ---------------------------------------------------------------------------

static uint32_t hash_u32(uint32_t x) {
    x = ((x >> 16) ^ x) * 0x45d9f3bu;
    x = ((x >> 16) ^ x) * 0x45d9f3bu;
    x =  (x >> 16) ^ x;
    return x;
}

static float hashf(uint32_t seed) {
    return static_cast<float>(hash_u32(seed)) / static_cast<float>(0xFFFFFFFFu);
}

// Generate 3D Worley (cellular) noise.
// size: texture dimension (size^3 voxels, must be power of two)
// gridDiv: number of cells per axis (feature point grid)
// Returns F1 distance field normalized to [0,1], stored as uint8.
// Value 0 = near feature point (cloud edge), 255 = far (cloud bulk interior).
std::vector<uint8_t> CloudPass::generateWorleyNoise(int size, int gridDiv) {
    struct FeaturePoint { float x, y, z; };

    // Pre-generate one feature point per cell
    const int numCells = gridDiv * gridDiv * gridDiv;
    std::vector<FeaturePoint> feats(static_cast<size_t>(numCells));
    for (int cz = 0; cz < gridDiv; ++cz)
    for (int cy = 0; cy < gridDiv; ++cy)
    for (int cx = 0; cx < gridDiv; ++cx) {
        uint32_t idx  = static_cast<uint32_t>(cz * gridDiv * gridDiv + cy * gridDiv + cx);
        uint32_t seed = idx * 3517u + 12345u;
        feats[idx] = {
            (static_cast<float>(cx) + hashf(seed + 0u)) / static_cast<float>(gridDiv),
            (static_cast<float>(cy) + hashf(seed + 1u)) / static_cast<float>(gridDiv),
            (static_cast<float>(cz) + hashf(seed + 2u)) / static_cast<float>(gridDiv)
        };
    }

    // Normalisation: max F1 distance ≈ 0.5 * (sqrt(3)/gridDiv)
    // Use slightly larger value so the noise spans full [0,1]
    const float norm = 1.3f / static_cast<float>(gridDiv);

    const int N = size;
    std::vector<uint8_t> noise(static_cast<size_t>(N * N * N));

    for (int z = 0; z < N; ++z)
    for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x) {
        float nx = static_cast<float>(x) / static_cast<float>(N);
        float ny = static_cast<float>(y) / static_cast<float>(N);
        float nz = static_cast<float>(z) / static_cast<float>(N);

        int cx = static_cast<int>(nx * gridDiv);
        int cy = static_cast<int>(ny * gridDiv);
        int cz = static_cast<int>(nz * gridDiv);

        float minDist = 1e9f;

        for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            int ncx = (cx + dx + gridDiv) % gridDiv;
            int ncy = (cy + dy + gridDiv) % gridDiv;
            int ncz = (cz + dz + gridDiv) % gridDiv;
            uint32_t fidx = static_cast<uint32_t>(ncz * gridDiv * gridDiv + ncy * gridDiv + ncx);

            float fx = feats[fidx].x;
            float fy = feats[fidx].y;
            float fz = feats[fidx].z;

            // Wrap-aware distance in normalised [0,1] space
            float ddx = nx - fx; if (ddx >  0.5f) ddx -= 1.0f; if (ddx < -0.5f) ddx += 1.0f;
            float ddy = ny - fy; if (ddy >  0.5f) ddy -= 1.0f; if (ddy < -0.5f) ddy += 1.0f;
            float ddz = nz - fz; if (ddz >  0.5f) ddz -= 1.0f; if (ddz < -0.5f) ddz += 1.0f;

            float d = std::sqrt(ddx*ddx + ddy*ddy + ddz*ddz);
            if (d < minDist) minDist = d;
        }

        float val = std::min(minDist / norm, 1.0f);
        noise[static_cast<size_t>(z * N * N + y * N + x)] =
            static_cast<uint8_t>(val * 255.0f);
    }

    return noise;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

CloudPass::~CloudPass() {
    cleanup();
}

bool CloudPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device         = device;
    m_physicalDevice = physicalDevice;
    m_fullWidth  = 1920;
    m_fullHeight = 1080;
    m_cloudWidth  = m_fullWidth  / 2;
    m_cloudHeight = m_fullHeight / 2;

    std::cout << "CloudPass: Generating Worley noise (128^3)..." << std::endl;
    auto noiseData = generateWorleyNoise(128, 8);
    if (!createNoiseTexture(noiseData)) {
        std::cerr << "CloudPass: noise texture creation failed" << std::endl;
        return false;
    }

    if (!createCloudOutput()) {
        std::cerr << "CloudPass: cloud output image creation failed" << std::endl;
        return false;
    }

    if (!createDescriptors()) {
        std::cerr << "CloudPass: descriptor creation failed" << std::endl;
        return false;
    }

    if (!createPipelineResources()) {
        std::cerr << "CloudPass: pipeline creation failed" << std::endl;
        return false;
    }

    // Depth sampler (NEAREST, no compare)
    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_NEAREST;
    si.minFilter    = VK_FILTER_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.compareEnable = VK_FALSE;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(m_device, &si, nullptr, &m_depthSampler) != VK_SUCCESS) {
        return false;
    }

    std::cout << "CloudPass: OK" << std::endl;
    return true;
}

void CloudPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);

    destroyCloudOutput();

    safeDestroy(m_pipeline);
    safeDestroy(m_pipelineLayout);
    safeDestroy(m_descriptorPool);
    safeDestroy(m_descriptorLayout);

    safeDestroy(m_depthSampler);

    // Noise resources
    safeDestroy(m_noiseSampler);
    safeDestroy(m_noiseView);
    safeDestroy(m_noiseImage);
    safeFree(m_noiseMemory);
    safeDestroy(m_noiseStagingBuffer);
    safeFree(m_noiseStagingMemory);
}

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

void CloudPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (m_cloudOutput.image == VK_NULL_HANDLE) return;

    // --- One-time: upload noise texture + transition cloud output to GENERAL ---
    if (!m_noiseUploaded) {
        // Transition 3D noise: UNDEFINED → TRANSFER_DST
        VkImageMemoryBarrier noiseBarrier{};
        noiseBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        noiseBarrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        noiseBarrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        noiseBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        noiseBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        noiseBarrier.image               = m_noiseImage;
        noiseBarrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        noiseBarrier.srcAccessMask       = 0;
        noiseBarrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &noiseBarrier);

        // Copy staging buffer → 3D noise image
        VkBufferImageCopy region{};
        region.bufferOffset                  = 0;
        region.bufferRowLength               = 0;
        region.bufferImageHeight             = 0;
        region.imageSubresource.aspectMask   = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel     = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount   = 1;
        region.imageOffset                   = {0, 0, 0};
        region.imageExtent                   = {128, 128, 128};
        vkCmdCopyBufferToImage(cmd, m_noiseStagingBuffer, m_noiseImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        // Transition noise: TRANSFER_DST → SHADER_READ_ONLY
        noiseBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        noiseBarrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        noiseBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        noiseBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &noiseBarrier);

        m_noiseUploaded = true;
    }

    // --- Transition cloud output: UNDEFINED → GENERAL (first time or after resize) ---
    if (!m_cloudOutputReady) {
        VkImageMemoryBarrier cloudBarrier{};
        cloudBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        cloudBarrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        cloudBarrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        cloudBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cloudBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cloudBarrier.image               = m_cloudOutput.image;
        cloudBarrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        cloudBarrier.srcAccessMask       = 0;
        cloudBarrier.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &cloudBarrier);

        m_cloudOutputReady = true;
    }

    VkPipelineStageFlags srcStage;
    VkAccessFlags        srcAccess;

    if (!m_enabled) {
        // Clear cloud buffer to (0,0,0,1) = fully transparent
        VkClearColorValue clearVal{};
        clearVal.float32[0] = 0.0f;
        clearVal.float32[1] = 0.0f;
        clearVal.float32[2] = 0.0f;
        clearVal.float32[3] = 1.0f;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, m_cloudOutput.image, VK_IMAGE_LAYOUT_GENERAL,
                             &clearVal, 1, &range);
        srcStage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
        srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else {
        // Update descriptor set with current depth view
        updateDescriptors();

        // Dispatch compute
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

        CloudParams params{};
        params.invViewProj     = m_invViewProj;
        params.sunDirection    = m_sunDir;
        params.time            = m_time;
        params.cameraPos       = m_cameraPos;
        params.cloudCoverage   = m_coverage;
        params.outputSize      = glm::vec2(static_cast<float>(m_cloudWidth),
                                           static_cast<float>(m_cloudHeight));
        params.cloudAltMin     = m_altMin;
        params.cloudAltMax     = m_altMax;
        params.cloudDensity    = m_density;
        params.cloudAbsorption = m_absorption;
        params.cloudSpeed      = m_speed;
        params.pad             = 0.0f;
        params.sunColor        = m_sunColor;
        params.sunIntensity    = m_sunIntensity;

        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(CloudParams), &params);

        // Dispatch: 8x8 thread groups
        uint32_t groupX = (m_cloudWidth  + 7u) / 8u;
        uint32_t groupY = (m_cloudHeight + 7u) / 8u;
        vkCmdDispatch(cmd, groupX, groupY, 1);

        srcStage  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        srcAccess = VK_ACCESS_SHADER_WRITE_BIT;
    }

    // Barrier: make cloud write visible to sky fragment shader
    // Layout stays GENERAL — valid for sampling via COMBINED_IMAGE_SAMPLER.
    VkImageMemoryBarrier readyBarrier{};
    readyBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    readyBarrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    readyBarrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    readyBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readyBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readyBarrier.image               = m_cloudOutput.image;
    readyBarrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    readyBarrier.srcAccessMask       = srcAccess;
    readyBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        srcStage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &readyBarrier);
}

void CloudPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_fullWidth && height == m_fullHeight) return;
    m_fullWidth  = width;
    m_fullHeight = height;
    m_cloudWidth  = std::max(width  / 2u, 1u);
    m_cloudHeight = std::max(height / 2u, 1u);

    vkDeviceWaitIdle(m_device);
    destroyCloudOutput();
    createCloudOutput();

    // Descriptors reference the new cloud image view — update them
    if (m_descriptorSet != VK_NULL_HANDLE) {
        updateDescriptors();
    }
    m_cloudOutputReady = false;  // needs UNDEFINED→GENERAL transition on next frame
}

// ---------------------------------------------------------------------------
// Setters
// ---------------------------------------------------------------------------

void CloudPass::setDepthBuffer(VkImageView depth) {
    m_depthView = depth;
}

void CloudPass::setSunData(const glm::vec3& direction, const glm::vec3& color, float intensity) {
    m_sunDir       = direction;
    m_sunColor     = color;
    m_sunIntensity = intensity;
}

void CloudPass::setCameraData(const glm::mat4& invViewProj, const glm::vec3& cameraPos) {
    m_invViewProj = invViewProj;
    m_cameraPos   = cameraPos;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool CloudPass::createNoiseTexture(const std::vector<uint8_t>& data) {
    const uint32_t SIZE        = 128;
    const VkDeviceSize bufSize = SIZE * SIZE * SIZE;

    // --- Staging buffer (HOST_VISIBLE | HOST_COHERENT) ---
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size  = bufSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &m_noiseStagingBuffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(m_device, m_noiseStagingBuffer, &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_noiseStagingMemory) != VK_SUCCESS)
        return false;
    vkBindBufferMemory(m_device, m_noiseStagingBuffer, m_noiseStagingMemory, 0);

    void* mapped;
    vkMapMemory(m_device, m_noiseStagingMemory, 0, bufSize, 0, &mapped);
    std::memcpy(mapped, data.data(), static_cast<size_t>(bufSize));
    vkUnmapMemory(m_device, m_noiseStagingMemory);

    // --- 3D device-local image ---
    VkImageCreateInfo imgInfo{};
    imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType     = VK_IMAGE_TYPE_3D;
    imgInfo.format        = VK_FORMAT_R8_UNORM;
    imgInfo.extent        = {SIZE, SIZE, SIZE};
    imgInfo.mipLevels     = 1;
    imgInfo.arrayLayers   = 1;
    imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &imgInfo, nullptr, &m_noiseImage) != VK_SUCCESS)
        return false;

    vkGetImageMemoryRequirements(m_device, m_noiseImage, &memReq);
    VkMemoryAllocateInfo noiseAlloc{};
    noiseAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    noiseAlloc.allocationSize = memReq.size;
    noiseAlloc.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &noiseAlloc, nullptr, &m_noiseMemory) != VK_SUCCESS)
        return false;
    vkBindImageMemory(m_device, m_noiseImage, m_noiseMemory, 0);

    // --- 3D image view ---
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image    = m_noiseImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format   = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_noiseView) != VK_SUCCESS)
        return false;

    // --- Noise sampler (LINEAR, REPEAT — tiling 3D texture) ---
    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (vkCreateSampler(m_device, &si, nullptr, &m_noiseSampler) != VK_SUCCESS)
        return false;

    return true;
}

bool CloudPass::createCloudOutput() {
    m_cloudOutput = createRenderTarget(
        VK_FORMAT_R16G16B16A16_SFLOAT,
        m_cloudWidth, m_cloudHeight,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT);  // transfer dst for vkCmdClearColorImage
    return m_cloudOutput.image != VK_NULL_HANDLE;
}

void CloudPass::destroyCloudOutput() {
    m_cloudOutput.destroy(m_device);
    m_cloudOutputReady = false;
}

bool CloudPass::createDescriptors() {
    // Binding 0: storage image (cloud output, writeonly)
    // Binding 1: combined sampler 3D (Worley noise)
    // Binding 2: combined sampler 2D (depth buffer)
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorLayout) != VK_SUCCESS)
        return false;

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,         1};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descriptorLayout;
    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet) != VK_SUCCESS)
        return false;

    // Write static bindings immediately (noise texture doesn't change)
    updateDescriptors();
    return true;
}

void CloudPass::updateDescriptors() {
    if (m_descriptorSet == VK_NULL_HANDLE) return;

    std::vector<VkWriteDescriptorSet> writes;

    // Binding 0: cloud output storage image
    VkDescriptorImageInfo storageInfo{};
    storageInfo.imageView   = m_cloudOutput.view;
    storageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (m_cloudOutput.view != VK_NULL_HANDLE) {
        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = m_descriptorSet;
        w.dstBinding      = 0;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.descriptorCount = 1;
        w.pImageInfo      = &storageInfo;
        writes.push_back(w);
    }

    // Binding 1: 3D noise texture
    VkDescriptorImageInfo noiseInfo{};
    noiseInfo.sampler     = m_noiseSampler;
    noiseInfo.imageView   = m_noiseView;
    noiseInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (m_noiseView != VK_NULL_HANDLE && m_noiseSampler != VK_NULL_HANDLE) {
        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = m_descriptorSet;
        w.dstBinding      = 1;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo      = &noiseInfo;
        writes.push_back(w);
    }

    // Binding 2: depth buffer
    VkDescriptorImageInfo depthInfo{};
    depthInfo.sampler     = m_depthSampler;
    depthInfo.imageView   = m_depthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    if (m_depthView != VK_NULL_HANDLE && m_depthSampler != VK_NULL_HANDLE) {
        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = m_descriptorSet;
        w.dstBinding      = 2;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo      = &depthInfo;
        writes.push_back(w);
    }

    if (!writes.empty()) {
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

bool CloudPass::createPipelineResources() {
    return createComputePipeline(
        "compute_cloud.comp.spv",
        m_descriptorLayout,
        static_cast<uint32_t>(sizeof(CloudParams)),
        m_pipeline, m_pipelineLayout);
}

} // namespace ohao
