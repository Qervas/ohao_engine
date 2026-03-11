#include "cloud_pass.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <array>
#include <algorithm>

namespace ohao {

// ---------------------------------------------------------------------------
// Hash utilities
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

// ---------------------------------------------------------------------------
// Worley noise (single octave, returns F1 distance normalized to [0,1])
// ---------------------------------------------------------------------------
static float worleyF1(float nx, float ny, float nz, int gridDiv, uint32_t seedOffset) {
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
        uint32_t seed = fidx * 3517u + 12345u + seedOffset;

        float fx = (static_cast<float>(ncx) + hashf(seed + 0u)) / static_cast<float>(gridDiv);
        float fy = (static_cast<float>(ncy) + hashf(seed + 1u)) / static_cast<float>(gridDiv);
        float fz = (static_cast<float>(ncz) + hashf(seed + 2u)) / static_cast<float>(gridDiv);

        float ddx = nx - fx; if (ddx >  0.5f) ddx -= 1.0f; if (ddx < -0.5f) ddx += 1.0f;
        float ddy = ny - fy; if (ddy >  0.5f) ddy -= 1.0f; if (ddy < -0.5f) ddy += 1.0f;
        float ddz = nz - fz; if (ddz >  0.5f) ddz -= 1.0f; if (ddz < -0.5f) ddz += 1.0f;

        float d = std::sqrt(ddx*ddx + ddy*ddy + ddz*ddz);
        if (d < minDist) minDist = d;
    }

    float norm = 1.3f / static_cast<float>(gridDiv);
    return std::min(minDist / norm, 1.0f);
}

// ---------------------------------------------------------------------------
// Perlin noise (3D, single octave)
// ---------------------------------------------------------------------------
static float perlinFade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

static float perlinGrad(uint32_t hash, float x, float y, float z) {
    uint32_t h = hash & 15;
    float u = h < 8 ? x : y;
    float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

static float perlinNoise(float x, float y, float z) {
    int xi = static_cast<int>(std::floor(x)) & 255;
    int yi = static_cast<int>(std::floor(y)) & 255;
    int zi = static_cast<int>(std::floor(z)) & 255;
    float xf = x - std::floor(x);
    float yf = y - std::floor(y);
    float zf = z - std::floor(z);
    float u = perlinFade(xf);
    float v = perlinFade(yf);
    float w = perlinFade(zf);

    auto p = [](int i) -> uint32_t { return hash_u32(static_cast<uint32_t>(i & 255)); };

    uint32_t aaa = p(p(p(xi) + yi) + zi);
    uint32_t aba = p(p(p(xi) + yi + 1) + zi);
    uint32_t aab = p(p(p(xi) + yi) + zi + 1);
    uint32_t abb = p(p(p(xi) + yi + 1) + zi + 1);
    uint32_t baa = p(p(p(xi + 1) + yi) + zi);
    uint32_t bba = p(p(p(xi + 1) + yi + 1) + zi);
    uint32_t bab = p(p(p(xi + 1) + yi) + zi + 1);
    uint32_t bbb = p(p(p(xi + 1) + yi + 1) + zi + 1);

    auto mix = [](float a, float b, float t) { return a + t * (b - a); };

    float x1 = mix(perlinGrad(aaa, xf, yf, zf),       perlinGrad(baa, xf-1, yf, zf),   u);
    float x2 = mix(perlinGrad(aba, xf, yf-1, zf),     perlinGrad(bba, xf-1, yf-1, zf), u);
    float y1 = mix(x1, x2, v);

    x1 = mix(perlinGrad(aab, xf, yf, zf-1),     perlinGrad(bab, xf-1, yf, zf-1),   u);
    x2 = mix(perlinGrad(abb, xf, yf-1, zf-1),   perlinGrad(bbb, xf-1, yf-1, zf-1), u);
    float y2 = mix(x1, x2, v);

    return (mix(y1, y2, w) + 1.0f) * 0.5f; // [0,1]
}

// ---------------------------------------------------------------------------
// Generate 3D Perlin-Worley RGBA8 noise (128^3)
// R = Perlin-Worley (Perlin * inverted Worley), smooth cloud base
// G = Worley F1 (4 cells) — medium detail
// B = Worley F1 (8 cells) — fine detail
// A = Worley F1 (16 cells) — erosion detail
// ---------------------------------------------------------------------------
std::vector<uint8_t> CloudPass::generatePerlinWorleyNoise(int size) {
    const int N = size;
    std::vector<uint8_t> data(static_cast<size_t>(N * N * N * 4));

    std::cout << "CloudPass: Generating Perlin-Worley RGBA8 noise (" << N << "^3)..." << std::endl;

    for (int z = 0; z < N; ++z)
    for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x) {
        float nx = static_cast<float>(x) / static_cast<float>(N);
        float ny = static_cast<float>(y) / static_cast<float>(N);
        float nz = static_cast<float>(z) / static_cast<float>(N);

        // Perlin noise — 4 octaves for smoother, less periodic base
        float perlin = perlinNoise(nx * 4.0f, ny * 4.0f, nz * 4.0f) * 0.5f
                     + perlinNoise(nx * 8.0f, ny * 8.0f, nz * 8.0f) * 0.25f
                     + perlinNoise(nx * 16.0f, ny * 16.0f, nz * 16.0f) * 0.125f
                     + perlinNoise(nx * 32.0f, ny * 32.0f, nz * 32.0f) * 0.0625f;
        perlin = std::clamp(perlin * 1.067f, 0.0f, 1.0f); // normalize sum

        // Worley at 6/12/24 cells — 6 cells breaks obvious 4-cell grid
        float w6  = worleyF1(nx, ny, nz, 6,  0);
        float w12 = worleyF1(nx, ny, nz, 12, 10000);
        float w24 = worleyF1(nx, ny, nz, 24, 20000);

        // Perlin-Worley R channel (Schneider, SIGGRAPH 2015):
        // remap(perlin, 0, 1, invWorley, 1) = invWorley + perlin * worley
        float invW6 = 1.0f - w6;
        float pw = std::clamp(invW6 + perlin * w6, 0.0f, 1.0f);

        size_t idx = static_cast<size_t>((z * N * N + y * N + x) * 4);
        data[idx + 0] = static_cast<uint8_t>(pw * 255.0f);
        data[idx + 1] = static_cast<uint8_t>(w6 * 255.0f);
        data[idx + 2] = static_cast<uint8_t>(w12 * 255.0f);
        data[idx + 3] = static_cast<uint8_t>(w24 * 255.0f);
    }

    return data;
}

// ---------------------------------------------------------------------------
// Generate 2D Weather Map (512² RGBA8)
// R = cloud coverage probability
// G = cloud type (0=stratus, 0.5=cumulus, 1.0=cumulonimbus)
// B = precipitation probability
// A = unused (1.0)
// ---------------------------------------------------------------------------
std::vector<uint8_t> CloudPass::generateWeatherMap(int size) {
    const int N = size;
    std::vector<uint8_t> data(static_cast<size_t>(N * N * 4));

    for (int y = 0; y < N; ++y)
    for (int x = 0; x < N; ++x) {
        float nx = static_cast<float>(x) / static_cast<float>(N);
        float ny = static_cast<float>(y) / static_cast<float>(N);

        // Large-scale coverage pattern (Perlin)
        float coverage = perlinNoise(nx * 4.0f, ny * 4.0f, 0.0f) * 0.5f
                       + perlinNoise(nx * 8.0f, ny * 8.0f, 0.5f) * 0.3f
                       + perlinNoise(nx * 16.0f, ny * 16.0f, 1.0f) * 0.2f;
        coverage = std::clamp(coverage, 0.0f, 1.0f);

        // Cloud type: varies across map (separate noise pattern)
        float type = perlinNoise(nx * 3.0f + 100.0f, ny * 3.0f + 100.0f, 2.0f);
        type = std::clamp(type, 0.0f, 1.0f);

        // Precipitation: correlated with coverage
        float precip = std::clamp(coverage * 1.5f - 0.3f, 0.0f, 1.0f);
        precip *= perlinNoise(nx * 6.0f + 200.0f, ny * 6.0f + 200.0f, 3.0f);
        precip = std::clamp(precip, 0.0f, 1.0f);

        size_t idx = static_cast<size_t>((y * N + x) * 4);
        data[idx + 0] = static_cast<uint8_t>(coverage * 255.0f);
        data[idx + 1] = static_cast<uint8_t>(type * 255.0f);
        data[idx + 2] = static_cast<uint8_t>(precip * 255.0f);
        data[idx + 3] = 255; // unused
    }

    return data;
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
    m_cloudWidth  = m_fullWidth  * 3 / 4;
    m_cloudHeight = m_fullHeight * 3 / 4;

    auto noiseData = generatePerlinWorleyNoise(128);
    if (!createNoiseTexture(noiseData)) {
        std::cerr << "CloudPass: noise texture creation failed" << std::endl;
        return false;
    }

    auto weatherData = generateWeatherMap(512);
    if (!createWeatherTexture(weatherData)) {
        std::cerr << "CloudPass: weather map creation failed" << std::endl;
        return false;
    }

    auto blueNoiseData = generateBlueNoise(128);
    if (!createBlueNoiseTexture(blueNoiseData)) {
        std::cerr << "CloudPass: blue noise texture creation failed" << std::endl;
        return false;
    }

    if (!createCloudOutput()) {
        std::cerr << "CloudPass: cloud output image creation failed" << std::endl;
        return false;
    }

    if (!createHistoryBuffers()) {
        std::cerr << "CloudPass: history buffer creation failed" << std::endl;
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

    // History sampler (LINEAR for smooth reprojection)
    VkSamplerCreateInfo hsi{};
    hsi.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    hsi.magFilter    = VK_FILTER_LINEAR;
    hsi.minFilter    = VK_FILTER_LINEAR;
    hsi.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    hsi.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    hsi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    hsi.compareEnable = VK_FALSE;
    hsi.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(m_device, &hsi, nullptr, &m_historySampler) != VK_SUCCESS) {
        return false;
    }

    std::cout << "CloudPass: OK (Perlin-Worley RGBA8 + Weather Map + Temporal)" << std::endl;
    return true;
}

void CloudPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);

    destroyCloudOutput();
    destroyHistoryBuffers();

    safeDestroy(m_pipeline);
    safeDestroy(m_pipelineLayout);
    safeDestroy(m_descriptorPool);
    safeDestroy(m_descriptorLayout);

    safeDestroy(m_depthSampler);
    safeDestroy(m_historySampler);

    // Noise resources
    safeDestroy(m_noiseSampler);
    safeDestroy(m_noiseView);
    safeDestroy(m_noiseImage);
    safeFree(m_noiseMemory);
    safeDestroy(m_noiseStagingBuffer);
    safeFree(m_noiseStagingMemory);

    // Weather map resources
    safeDestroy(m_weatherSampler);
    safeDestroy(m_weatherView);
    safeDestroy(m_weatherImage);
    safeFree(m_weatherMemory);
    safeDestroy(m_weatherStagingBuffer);
    safeFree(m_weatherStagingMemory);

    // Blue noise resources
    safeDestroy(m_blueNoiseSampler);
    safeDestroy(m_blueNoiseView);
    safeDestroy(m_blueNoiseImage);
    safeFree(m_blueNoiseMemory);
    safeDestroy(m_blueNoiseStagingBuffer);
    safeFree(m_blueNoiseStagingMemory);
}

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------

void CloudPass::execute(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (m_cloudOutput.image == VK_NULL_HANDLE) return;

    // --- One-time: upload noise texture ---
    if (!m_noiseUploaded) {
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

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask   = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount   = 1;
        region.imageExtent                   = {128, 128, 128};
        vkCmdCopyBufferToImage(cmd, m_noiseStagingBuffer, m_noiseImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        noiseBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        noiseBarrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        noiseBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        noiseBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &noiseBarrier);

        m_noiseUploaded = true;
    }

    // --- One-time: upload weather map ---
    if (!m_weatherUploaded) {
        VkImageMemoryBarrier weatherBarrier{};
        weatherBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        weatherBarrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        weatherBarrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        weatherBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        weatherBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        weatherBarrier.image               = m_weatherImage;
        weatherBarrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        weatherBarrier.srcAccessMask       = 0;
        weatherBarrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &weatherBarrier);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask   = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount   = 1;
        region.imageExtent                   = {512, 512, 1};
        vkCmdCopyBufferToImage(cmd, m_weatherStagingBuffer, m_weatherImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        weatherBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        weatherBarrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        weatherBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        weatherBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &weatherBarrier);

        m_weatherUploaded = true;
    }

    // --- One-time: upload blue noise texture ---
    if (!m_blueNoiseUploaded) {
        VkImageMemoryBarrier bnBarrier{};
        bnBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bnBarrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        bnBarrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bnBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bnBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bnBarrier.image               = m_blueNoiseImage;
        bnBarrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        bnBarrier.srcAccessMask       = 0;
        bnBarrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bnBarrier);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent                 = {128, 128, 1};
        vkCmdCopyBufferToImage(cmd, m_blueNoiseStagingBuffer, m_blueNoiseImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        bnBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bnBarrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bnBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bnBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &bnBarrier);

        m_blueNoiseUploaded = true;
    }

    // --- Transition history buffers: UNDEFINED → GENERAL (first time or after resize) ---
    for (int i = 0; i < 2; i++) {
        if (!m_historyReady[i] && m_historyBuffers[i].image != VK_NULL_HANDLE) {
            VkImageMemoryBarrier hb{};
            hb.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            hb.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            hb.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            hb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hb.image               = m_historyBuffers[i].image;
            hb.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            hb.srcAccessMask       = 0;
            hb.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &hb);
            m_historyReady[i] = true;
        }
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
        VkClearColorValue clearVal{};
        clearVal.float32[3] = 1.0f;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, m_cloudOutput.image, VK_IMAGE_LAYOUT_GENERAL,
                             &clearVal, 1, &range);
        srcStage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
        srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else {
        updateDescriptors();

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

        CloudParams params{};
        params.invViewProj       = m_invViewProj;
        params.sunDirection      = m_sunDir;
        params.time              = m_time;
        params.cameraPos         = m_cameraPos;
        params.cloudCoverage     = m_coverage;
        params.outputSize        = glm::vec2(static_cast<float>(m_cloudWidth),
                                             static_cast<float>(m_cloudHeight));
        params.cloudAltMin       = m_altMin;
        params.cloudAltMax       = m_altMax;
        params.cloudDensity      = m_density;
        params.cloudAbsorption   = m_absorption;
        params.cloudSpeed        = m_speed;
        params.frameIndex        = m_frameIndex;
        params.sunColor          = m_sunColor;
        params.sunIntensity      = m_sunIntensity;
        params.prevViewProj      = m_prevViewProj;
        params.lightningIntensity = m_lightningIntensity;
        params.lightningPosX     = m_lightningPosX;
        params.lightningPosZ     = m_lightningPosZ;
        params.pad               = 0.0f;

        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(CloudParams), &params);

        uint32_t groupX = (m_cloudWidth  + 7u) / 8u;
        uint32_t groupY = (m_cloudHeight + 7u) / 8u;
        vkCmdDispatch(cmd, groupX, groupY, 1);

        srcStage  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        srcAccess = VK_ACCESS_SHADER_WRITE_BIT;
    }

    // Copy cloud output to current history buffer for next frame's reprojection
    if (m_enabled && m_historyBuffers[m_currentHistory].image != VK_NULL_HANDLE) {
        // Barrier: cloud output → TRANSFER_SRC
        VkImageMemoryBarrier copySrc{};
        copySrc.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        copySrc.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        copySrc.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        copySrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copySrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copySrc.image               = m_cloudOutput.image;
        copySrc.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        copySrc.srcAccessMask       = srcAccess;
        copySrc.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &copySrc);

        VkImageCopy copyRegion{};
        copyRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copyRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copyRegion.extent = {m_cloudWidth, m_cloudHeight, 1};
        vkCmdCopyImage(cmd,
            m_cloudOutput.image, VK_IMAGE_LAYOUT_GENERAL,
            m_historyBuffers[m_currentHistory].image, VK_IMAGE_LAYOUT_GENERAL,
            1, &copyRegion);

        // Barrier: history write → next frame shader read
        VkImageMemoryBarrier histBarrier{};
        histBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        histBarrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        histBarrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        histBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        histBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        histBarrier.image               = m_historyBuffers[m_currentHistory].image;
        histBarrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        histBarrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        histBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &histBarrier);

        // Swap ping-pong
        m_currentHistory = 1 - m_currentHistory;

        // Barrier: cloud output → readable by sky pass
        VkImageMemoryBarrier readyBarrier{};
        readyBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        readyBarrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        readyBarrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        readyBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        readyBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        readyBarrier.image               = m_cloudOutput.image;
        readyBarrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        readyBarrier.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        readyBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &readyBarrier);
    } else {
        // No history — simple barrier for sky pass
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
}

void CloudPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_fullWidth && height == m_fullHeight) return;
    m_fullWidth  = width;
    m_fullHeight = height;
    m_cloudWidth  = std::max(width  * 3u / 4u, 1u);
    m_cloudHeight = std::max(height * 3u / 4u, 1u);

    vkDeviceWaitIdle(m_device);
    destroyCloudOutput();
    destroyHistoryBuffers();
    createCloudOutput();
    createHistoryBuffers();

    if (m_descriptorSet != VK_NULL_HANDLE) {
        updateDescriptors();
    }
    m_cloudOutputReady = false;
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

void CloudPass::setLightningData(float intensity, float posX, float posZ) {
    m_lightningIntensity = intensity;
    m_lightningPosX = posX;
    m_lightningPosZ = posZ;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool CloudPass::createNoiseTexture(const std::vector<uint8_t>& data) {
    const uint32_t SIZE        = 128;
    const VkDeviceSize bufSize = SIZE * SIZE * SIZE * 4; // RGBA8

    // --- Staging buffer ---
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

    // --- 3D device-local image (RGBA8_UNORM) ---
    VkImageCreateInfo imgInfo{};
    imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType     = VK_IMAGE_TYPE_3D;
    imgInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
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
    viewInfo.format   = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_noiseView) != VK_SUCCESS)
        return false;

    // --- Noise sampler (LINEAR, REPEAT) ---
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

bool CloudPass::createWeatherTexture(const std::vector<uint8_t>& data) {
    const uint32_t SIZE = 512;
    const VkDeviceSize bufSize = SIZE * SIZE * 4;

    // --- Staging buffer ---
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size        = bufSize;
    bufInfo.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &m_weatherStagingBuffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(m_device, m_weatherStagingBuffer, &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_weatherStagingMemory) != VK_SUCCESS)
        return false;
    vkBindBufferMemory(m_device, m_weatherStagingBuffer, m_weatherStagingMemory, 0);

    void* mapped;
    vkMapMemory(m_device, m_weatherStagingMemory, 0, bufSize, 0, &mapped);
    std::memcpy(mapped, data.data(), static_cast<size_t>(bufSize));
    vkUnmapMemory(m_device, m_weatherStagingMemory);

    // --- 2D device-local image (RGBA8_UNORM) ---
    VkImageCreateInfo imgInfo{};
    imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType     = VK_IMAGE_TYPE_2D;
    imgInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent        = {SIZE, SIZE, 1};
    imgInfo.mipLevels     = 1;
    imgInfo.arrayLayers   = 1;
    imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &imgInfo, nullptr, &m_weatherImage) != VK_SUCCESS)
        return false;

    vkGetImageMemoryRequirements(m_device, m_weatherImage, &memReq);
    VkMemoryAllocateInfo imgAlloc{};
    imgAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imgAlloc.allocationSize  = memReq.size;
    imgAlloc.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &imgAlloc, nullptr, &m_weatherMemory) != VK_SUCCESS)
        return false;
    vkBindImageMemory(m_device, m_weatherImage, m_weatherMemory, 0);

    // --- Image view ---
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image    = m_weatherImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format   = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_weatherView) != VK_SUCCESS)
        return false;

    // --- Sampler (LINEAR, REPEAT) ---
    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(m_device, &si, nullptr, &m_weatherSampler) != VK_SUCCESS)
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// Blue noise generation (128² R8, hash-based approximation)
// ---------------------------------------------------------------------------
std::vector<uint8_t> CloudPass::generateBlueNoise(int size) {
    std::vector<uint8_t> data(size * size);
    // R2 sequence (quasi-random, good spectral properties)
    const float g  = 1.32471795724f; // Plastic constant
    const float a1 = 1.0f / g;
    const float a2 = 1.0f / (g * g);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int idx = y * size + x;
            float n = static_cast<float>(idx);
            float val = std::fmod(0.5f + a1 * n, 1.0f);
            val = std::fmod(val + a2 * n, 1.0f);
            data[idx] = static_cast<uint8_t>(val * 255.0f);
        }
    }
    return data;
}

bool CloudPass::createBlueNoiseTexture(const std::vector<uint8_t>& data) {
    const uint32_t SIZE = 128;
    const VkDeviceSize bufSize = SIZE * SIZE;

    // Staging buffer
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size  = bufSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &m_blueNoiseStagingBuffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, m_blueNoiseStagingBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_blueNoiseStagingMemory) != VK_SUCCESS)
        return false;

    vkBindBufferMemory(m_device, m_blueNoiseStagingBuffer, m_blueNoiseStagingMemory, 0);
    void* mapped;
    vkMapMemory(m_device, m_blueNoiseStagingMemory, 0, bufSize, 0, &mapped);
    std::memcpy(mapped, data.data(), bufSize);
    vkUnmapMemory(m_device, m_blueNoiseStagingMemory);

    // GPU image (2D, R8_UNORM)
    VkImageCreateInfo imgInfo{};
    imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType     = VK_IMAGE_TYPE_2D;
    imgInfo.format        = VK_FORMAT_R8_UNORM;
    imgInfo.extent        = {SIZE, SIZE, 1};
    imgInfo.mipLevels     = 1;
    imgInfo.arrayLayers   = 1;
    imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &imgInfo, nullptr, &m_blueNoiseImage) != VK_SUCCESS)
        return false;

    vkGetImageMemoryRequirements(m_device, m_blueNoiseImage, &memReqs);
    allocInfo.allocationSize  = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_blueNoiseMemory) != VK_SUCCESS)
        return false;
    vkBindImageMemory(m_device, m_blueNoiseImage, m_blueNoiseMemory, 0);

    // View
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                           = m_blueNoiseImage;
    viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                          = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;
    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_blueNoiseView) != VK_SUCCESS)
        return false;

    // Sampler (REPEAT for tiling)
    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_NEAREST;
    si.minFilter    = VK_FILTER_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (vkCreateSampler(m_device, &si, nullptr, &m_blueNoiseSampler) != VK_SUCCESS)
        return false;

    return true;
}

bool CloudPass::createHistoryBuffers() {
    for (int i = 0; i < 2; i++) {
        m_historyBuffers[i] = createRenderTarget(
            VK_FORMAT_R16G16B16A16_SFLOAT,
            m_cloudWidth, m_cloudHeight,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (m_historyBuffers[i].image == VK_NULL_HANDLE) return false;
        m_historyReady[i] = false;
    }
    m_currentHistory = 0;
    return true;
}

void CloudPass::destroyHistoryBuffers() {
    for (int i = 0; i < 2; i++) {
        m_historyBuffers[i].destroy(m_device);
        m_historyReady[i] = false;
    }
}

bool CloudPass::createCloudOutput() {
    m_cloudOutput = createRenderTarget(
        VK_FORMAT_R16G16B16A16_SFLOAT,
        m_cloudWidth, m_cloudHeight,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    return m_cloudOutput.image != VK_NULL_HANDLE;
}

void CloudPass::destroyCloudOutput() {
    m_cloudOutput.destroy(m_device);
    m_cloudOutputReady = false;
}

bool CloudPass::createDescriptors() {
    // Binding 0: storage image (cloud output, writeonly)
    // Binding 1: combined sampler 3D (Perlin-Worley noise RGBA8)
    // Binding 2: combined sampler 2D (depth buffer)
    // Binding 3: combined sampler 2D (weather map)
    // Binding 4: combined sampler 2D (blue noise 128² R8)
    // Binding 5: combined sampler 2D (history buffer — previous frame)
    std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
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

    bindings[3].binding         = 3;
    bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[4].binding         = 4;
    bindings[4].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[5].binding         = 5;
    bindings[5].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorLayout) != VK_SUCCESS)
        return false;

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,         1};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo dsAllocInfo{};
    dsAllocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAllocInfo.descriptorPool     = m_descriptorPool;
    dsAllocInfo.descriptorSetCount = 1;
    dsAllocInfo.pSetLayouts        = &m_descriptorLayout;
    if (vkAllocateDescriptorSets(m_device, &dsAllocInfo, &m_descriptorSet) != VK_SUCCESS)
        return false;

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

    // Binding 1: 3D noise texture (RGBA8)
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

    // Binding 3: weather map
    VkDescriptorImageInfo weatherInfo{};
    weatherInfo.sampler     = m_weatherSampler;
    weatherInfo.imageView   = m_weatherView;
    weatherInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (m_weatherView != VK_NULL_HANDLE && m_weatherSampler != VK_NULL_HANDLE) {
        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = m_descriptorSet;
        w.dstBinding      = 3;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo      = &weatherInfo;
        writes.push_back(w);
    }

    // Binding 4: blue noise texture
    VkDescriptorImageInfo blueNoiseInfo{};
    blueNoiseInfo.sampler     = m_blueNoiseSampler;
    blueNoiseInfo.imageView   = m_blueNoiseView;
    blueNoiseInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (m_blueNoiseView != VK_NULL_HANDLE && m_blueNoiseSampler != VK_NULL_HANDLE) {
        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = m_descriptorSet;
        w.dstBinding      = 4;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo      = &blueNoiseInfo;
        writes.push_back(w);
    }

    // Binding 5: history buffer (read from previous frame's output)
    uint32_t readIdx = 1 - m_currentHistory;
    VkDescriptorImageInfo historyInfo{};
    historyInfo.sampler     = m_historySampler;
    historyInfo.imageView   = m_historyBuffers[readIdx].view;
    historyInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (m_historyBuffers[readIdx].view != VK_NULL_HANDLE && m_historySampler != VK_NULL_HANDLE) {
        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = m_descriptorSet;
        w.dstBinding      = 5;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo      = &historyInfo;
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

bool CloudPass::reloadShader(const std::string& spvPath) {
    return reloadComputeShader(spvPath, m_descriptorLayout, sizeof(CloudParams),
                               m_pipeline, m_pipelineLayout);
}

} // namespace ohao
