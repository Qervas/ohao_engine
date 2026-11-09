#include "render/deferred/env_prefilter.hpp"
#include "render/shader_bindings/ibl_env_prefilter.hpp"

#include "core/log.hpp"
#include "gpu/shared_layout.hpp"
#include "render/rt/rt_compute.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace ohao::render {

namespace shared = gpu::shared;

namespace {

constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t kPrefilterSamples = 256;
constexpr uint32_t kIrradianceSamples = 512;
constexpr uint32_t kMaxSets = 64;

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
             VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Compute writes of the last dispatch visible to the next one's reads and writes.
void computeToCompute(VkCommandBuffer cmd) {
    VkMemoryBarrier m{};
    m.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    m.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    m.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &m,
                         0, nullptr, 0, nullptr);
}

} // namespace

EnvPrefilter::~EnvPrefilter() { cleanup(); }

bool EnvPrefilter::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    const VkDescriptorSetLayoutBinding bindings[] = {
        shader::layoutBinding(shader::ibl_env_prefilter::inImage),
        shader::layoutBinding(shader::ibl_env_prefilter::chain),
        shader::layoutBinding(shader::ibl_env_prefilter::chainSampler),
        shader::layoutBinding(shader::ibl_env_prefilter::outImage),
    };
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 4;
    li.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &li, nullptr, &m_setLayout) != VK_SUCCESS) return false;

    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * kMaxSets},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kMaxSets},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxSets},
    };
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = kMaxSets;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device, &si, nullptr, &m_sampler) != VK_SUCCESS) return false;

    constexpr uint32_t push = sizeof(shared::EnvPrefilterPush);
    return rtc::createComputePipeline(device, "ibl_env_prefilter.slang.copySource.spv", "copySource", m_setLayout,
                                      push, m_layouts[0], m_copy) &&
           rtc::createComputePipeline(device, "ibl_env_prefilter.slang.downsample.spv", "downsample", m_setLayout,
                                      push, m_layouts[1], m_downsample) &&
           rtc::createComputePipeline(device, "ibl_env_prefilter.slang.prefilter.spv", "prefilter", m_setLayout,
                                      push, m_layouts[2], m_prefilter) &&
           rtc::createComputePipeline(device, "ibl_env_prefilter.slang.irradiance.spv", "irradiance", m_setLayout,
                                      push, m_layouts[3], m_irradianceSum);
}

void EnvPrefilter::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    destroyImages();
    for (VkPipeline p : {m_copy, m_downsample, m_prefilter, m_irradianceSum})
        if (p) vkDestroyPipeline(m_device, p, nullptr);
    m_copy = m_downsample = m_prefilter = m_irradianceSum = VK_NULL_HANDLE;
    for (VkPipelineLayout& l : m_layouts) {
        if (l) vkDestroyPipelineLayout(m_device, l, nullptr);
        l = VK_NULL_HANDLE;
    }
    if (m_sampler) vkDestroySampler(m_device, m_sampler, nullptr);
    if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    m_sampler = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_device = VK_NULL_HANDLE;
}

bool EnvPrefilter::createMipImage(MipImage& img, uint32_t width, uint32_t height, uint32_t mips) {
    img.width = std::max(width, 1u);
    img.height = std::max(height, 1u);
    img.mips = mips;
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = kFormat;
    ii.extent = {img.width, img.height, 1};
    ii.mipLevels = mips;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &ii, nullptr, &img.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(m_device, img.image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = rtc::findMemoryType(m_physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(m_device, &ai, nullptr, &img.memory) != VK_SUCCESS)
        return false;
    vkBindImageMemory(m_device, img.image, img.memory, 0);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = kFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    if (vkCreateImageView(m_device, &vi, nullptr, &img.full) != VK_SUCCESS) return false;
    img.levels.assign(mips, VK_NULL_HANDLE);
    for (uint32_t m = 0; m < mips; ++m) {
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1};
        if (vkCreateImageView(m_device, &vi, nullptr, &img.levels[m]) != VK_SUCCESS) return false;
    }
    return true;
}

void EnvPrefilter::destroyMipImage(MipImage& img) {
    for (VkImageView v : img.levels)
        if (v) vkDestroyImageView(m_device, v, nullptr);
    if (img.full) vkDestroyImageView(m_device, img.full, nullptr);
    if (img.image) vkDestroyImage(m_device, img.image, nullptr);
    if (img.memory) vkFreeMemory(m_device, img.memory, nullptr);
    img = MipImage{};
}

void EnvPrefilter::destroyImages() {
    destroyMipImage(m_chain);
    destroyMipImage(m_prefiltered);
    destroyMipImage(m_irradiance);
}

VkDescriptorSet EnvPrefilter::allocateSet(VkImageView in, VkImageLayout inLayout, VkImageView chain, VkImageView out) {
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_device, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    // The build's images stay in GENERAL while it runs; a binding the entry point does not
    // use still gets a valid view.
    const VkDescriptorImageInfo inInfo{VK_NULL_HANDLE, in, inLayout};
    const VkDescriptorImageInfo chainInfo{VK_NULL_HANDLE, chain, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo samplerInfo{m_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    const VkDescriptorImageInfo outInfo{VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[4] = {
        shader::write(shader::ibl_env_prefilter::inImage, set),
        shader::write(shader::ibl_env_prefilter::chain, set),
        shader::write(shader::ibl_env_prefilter::chainSampler, set),
        shader::write(shader::ibl_env_prefilter::outImage, set),
    };
    w[0].pImageInfo = &inInfo;
    w[1].pImageInfo = &chainInfo;
    w[2].pImageInfo = &samplerInfo;
    w[3].pImageInfo = &outInfo;
    vkUpdateDescriptorSets(m_device, 4, w, 0, nullptr);
    return set;
}

void EnvPrefilter::dispatch(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet set, uint32_t w, uint32_t h,
                            uint32_t samples, float roughness) {
    const VkPipelineLayout layout = pipeline == m_copy         ? m_layouts[0]
                                    : pipeline == m_downsample ? m_layouts[1]
                                    : pipeline == m_prefilter  ? m_layouts[2]
                                                               : m_layouts[3];
    shared::EnvPrefilterPush push{};
    push.size = {w, h, samples, m_chain.mips};
    push.params = {roughness, static_cast<float>(m_chain.width), static_cast<float>(m_chain.height), 0.0f};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
}

bool EnvPrefilter::build(VkCommandBuffer cmd, VkImageView source, uint32_t width, uint32_t height) {
    if (m_copy == VK_NULL_HANDLE || source == VK_NULL_HANDLE || width == 0 || height == 0) return false;
    destroyImages();
    vkResetDescriptorPool(m_device, m_pool, 0);
    m_sourceWidth = width;
    m_sourceHeight = height;

    const uint32_t chainMips = static_cast<uint32_t>(std::bit_width(std::max(width, height)));
    const uint32_t levels = std::min(shared::kEnvPrefilterLevels, chainMips);
    if (!createMipImage(m_chain, width, height, chainMips) || !createMipImage(m_prefiltered, width, height, levels) ||
        !createMipImage(m_irradiance, shared::kEnvIrradianceWidth, shared::kEnvIrradianceWidth / 2, 1)) {
        OHAO_LOG(Error, render, "EnvPrefilter: image creation failed ({}x{})", width, height);
        destroyImages();
        return false;
    }

    for (VkImage img : {m_chain.image, m_prefiltered.image, m_irradiance.image})
        barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // The chain: the environment, then its 2x2 means.
    const auto levelSize = [](uint32_t base, uint32_t m) { return std::max(base >> m, 1u); };
    dispatch(cmd, m_copy, allocateSet(source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, m_chain.full, m_chain.levels[0]), width, height, 0, 0.0f);
    for (uint32_t m = 1; m < chainMips; ++m) {
        computeToCompute(cmd);
        dispatch(cmd, m_downsample, allocateSet(m_chain.levels[m - 1], VK_IMAGE_LAYOUT_GENERAL, m_chain.full, m_chain.levels[m]),
                 levelSize(width, m), levelSize(height, m), 0, 0.0f);
    }
    // Prefiltered level 0 is the environment (roughness 0); the others integrate GGX lobes.
    dispatch(cmd, m_copy, allocateSet(source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, m_chain.full, m_prefiltered.levels[0]), width, height, 0, 0.0f);
    computeToCompute(cmd);
    for (uint32_t m = 1; m < levels; ++m) {
        const float roughness = static_cast<float>(m) / static_cast<float>(shared::kEnvPrefilterLevels - 1);
        dispatch(cmd, m_prefilter, allocateSet(m_chain.levels[0], VK_IMAGE_LAYOUT_GENERAL, m_chain.full, m_prefiltered.levels[m]),
                 levelSize(width, m), levelSize(height, m), kPrefilterSamples, roughness);
    }
    dispatch(cmd, m_irradianceSum, allocateSet(m_chain.levels[0], VK_IMAGE_LAYOUT_GENERAL, m_chain.full, m_irradiance.levels[0]),
             m_irradiance.width, m_irradiance.height, kIrradianceSamples, 1.0f);

    for (VkImage img : {m_prefiltered.image, m_irradiance.image})
        barrier(cmd, img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    OHAO_LOG(Info, render, "EnvPrefilter: {}x{} environment, {} GGX levels, {}x{} irradiance", width, height, levels,
             m_irradiance.width, m_irradiance.height);
    return true;
}

} // namespace ohao::render
