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

} // namespace

EnvPrefilter::~EnvPrefilter() { cleanup(); }

bool EnvPrefilter::initialize(VkDevice device, GpuAllocator& allocator) {
    m_device = device;
    m_allocator = &allocator;

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
    retireImages();
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

void EnvPrefilter::retireImages() {
    for (gpu::Image* img : {&m_chain, &m_prefiltered, &m_irradiance}) m_allocator->retire(*img);
}

VkDescriptorSet EnvPrefilter::allocateSet(VkImageView in, VkImageLayout inLayout, VkImageView out) {
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_device, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    // The chain stays in GENERAL while the build runs; a binding the entry point does not use still gets a
    // valid view.
    const VkDescriptorImageInfo inInfo{VK_NULL_HANDLE, in, inLayout};
    const VkDescriptorImageInfo chainInfo{VK_NULL_HANDLE, m_chain.view, VK_IMAGE_LAYOUT_GENERAL};
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

void EnvPrefilter::dispatch(const Frame& frame, VkPipeline pipeline, gpu::Image& in, uint32_t inMip, bool readsChain,
                            gpu::Image& out, uint32_t outMip, uint32_t samples, float roughness) {
    const VkPipelineLayout layout = pipeline == m_copy         ? m_layouts[0]
                                    : pipeline == m_downsample ? m_layouts[1]
                                    : pipeline == m_prefilter  ? m_layouts[2]
                                                               : m_layouts[3];
    const bool fromChain = &in == &m_chain;
    const VkDescriptorSet set =
        allocateSet(in.subView(inMip), fromChain ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    out.subView(outMip));

    Pass pass(frame, nullptr, Pass::Compute);
    if (readsChain) pass.sampledGeneral(m_chain);
    else if (fromChain) pass.sampledGeneral(in, {inMip, 0});
    else pass.sampled(in);
    pass.storageWrite(out, Old::Discard, {outMip, 0});
    pass.begin();

    const VkExtent2D size = out.extent(outMip);
    shared::EnvPrefilterPush push{};
    push.size = {size.width, size.height, samples, m_chain.desc.mips};
    push.params = {roughness, static_cast<float>(m_chain.desc.width), static_cast<float>(m_chain.desc.height), 0.0f};
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(frame.cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(frame.cmd, (size.width + 7) / 8, (size.height + 7) / 8, 1);
}

bool EnvPrefilter::build(const Frame& frame, gpu::Image& source) {
    const uint32_t width = source.desc.width, height = source.desc.height;
    if (m_copy == VK_NULL_HANDLE || !source) return false;
    retireImages();
    vkResetDescriptorPool(m_device, m_pool, 0);

    const uint32_t chainMips = static_cast<uint32_t>(std::bit_width(std::max(width, height)));
    const uint32_t levels = std::min(shared::kEnvPrefilterLevels, chainMips);
    constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    m_chain = m_allocator->createImage({"env.chain", kFormat, width, height, kUsage, chainMips});
    m_prefiltered = m_allocator->createImage({"env.prefiltered", kFormat, width, height, kUsage, levels});
    m_irradiance = m_allocator->createImage(
        {"env.irradiance", kFormat, shared::kEnvIrradianceWidth, shared::kEnvIrradianceWidth / 2, kUsage});
    if (!m_chain || !m_prefiltered || !m_irradiance) {
        OHAO_LOG(Error, render, "EnvPrefilter: image creation failed ({}x{})", width, height);
        retireImages();
        return false;
    }

    // The chain: the environment, then its 2x2 means.
    dispatch(frame, m_copy, source, 0, false, m_chain, 0, 0, 0.0f);
    for (uint32_t m = 1; m < chainMips; ++m) dispatch(frame, m_downsample, m_chain, m - 1, false, m_chain, m, 0, 0.0f);
    // Prefiltered level 0 is the environment (roughness 0); the others integrate GGX lobes.
    dispatch(frame, m_copy, source, 0, false, m_prefiltered, 0, 0, 0.0f);
    for (uint32_t m = 1; m < levels; ++m) {
        const float roughness = static_cast<float>(m) / static_cast<float>(shared::kEnvPrefilterLevels - 1);
        dispatch(frame, m_prefilter, m_chain, 0, true, m_prefiltered, m, kPrefilterSamples, roughness);
    }
    dispatch(frame, m_irradianceSum, m_chain, 0, true, m_irradiance, 0, kIrradianceSamples, 1.0f);

    OHAO_LOG(Info, render, "EnvPrefilter: {}x{} environment, {} GGX levels, {}x{} irradiance", width, height, levels,
             m_irradiance.desc.width, m_irradiance.desc.height);
    return true;
}

} // namespace ohao::render
