#include "bloom_pass.hpp"
#include "render/shader_bindings/postprocess_bloom_downsample.hpp"
#include "render/shader_bindings/postprocess_bloom_input.hpp"
#include "render/shader_bindings/postprocess_bloom_upsample.hpp"
#include "render/graph/pipeline.hpp"
#include <algorithm>
#include <cmath>
#include "core/log.hpp"

namespace ohao {

namespace {
// The bloom input, downsample and upsample shaders read one image at the same binding: one layout serves all three.
constexpr shader::Binding kInput = shader::postprocess_bloom_downsample::inputTexture;
static_assert(shader::postprocess_bloom_upsample::inputTexture.binding == kInput.binding &&
              shader::postprocess_bloom_upsample::inputTexture.type == kInput.type &&
              shader::postprocess_bloom_input::hdrInput.binding == kInput.binding &&
              shader::postprocess_bloom_input::hdrInput.type == kInput.type);
} // namespace

BloomPass::~BloomPass() {
    cleanup();
}

bool BloomPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_width = 1920;
    m_height = 1080;

    if (!m_allocator) return false;
    if (!createMipChain()) return false;
    if (!createDescriptors()) return false;
    if (!createPipelines()) return false;

    return true;
}

void BloomPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    if (m_inputPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_inputPipeline, nullptr);
    if (m_downsamplePipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_downsamplePipeline, nullptr);
    if (m_upsamplePipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_upsamplePipeline, nullptr);
    if (m_inputPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_inputPipelineLayout, nullptr);
    if (m_downsampleLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_downsampleLayout, nullptr);
    if (m_upsampleLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_upsampleLayout, nullptr);
    if (m_descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    if (m_inputLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, m_inputLayout, nullptr);
    if (m_sampler != VK_NULL_HANDLE) vkDestroySampler(m_device, m_sampler, nullptr);
    destroyMipChain();

    m_inputPipeline = VK_NULL_HANDLE;
    m_downsamplePipeline = VK_NULL_HANDLE;
    m_upsamplePipeline = VK_NULL_HANDLE;
    m_inputPipelineLayout = VK_NULL_HANDLE;
    m_downsampleLayout = VK_NULL_HANDLE;
    m_upsampleLayout = VK_NULL_HANDLE;
    m_descriptorPool = VK_NULL_HANDLE;
    m_inputLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
}

void BloomPass::record(const render::Frame& frame) {
    m_executed = false;
    const uint32_t frameIndex = frame.slot;

    if (!m_hdrInput || !*m_hdrInput) return;
    if (frameIndex >= MAX_FRAMES_IN_FLIGHT || m_inputSets[frameIndex] == VK_NULL_HANDLE) return;
    if (m_inputPipeline == VK_NULL_HANDLE || m_downsamplePipeline == VK_NULL_HANDLE ||
        m_upsamplePipeline == VK_NULL_HANDLE) return;
    if (m_mipLevels == 0 || !m_chain) return;

    // This frame slot's input set; its last use was retired by the slot's fence.
    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = m_sampler;
    imageInfo.imageView = m_hdrInput->view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_inputSets[frameIndex];
    write.dstBinding = kInput.binding;
    write.descriptorType = kInput.type;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

    // The frame into level 0 as it is.
    draw(frame, 0, render::Load::Clear, m_inputPipeline, m_inputPipelineLayout, m_inputSets[frameIndex], nullptr);

    // Down: level i from level i - 1; the first with Karis's average (against fireflies).
    for (uint32_t i = 1; i < m_mipLevels; ++i) {
        const SampleParams params{glm::vec2(1.0f / m_mipSizes[i - 1].x, 1.0f / m_mipSizes[i - 1].y), m_filterRadius,
                                  i == 1 ? 1.0f : 0.0f};
        draw(frame, i, render::Load::Clear, m_downsamplePipeline, m_downsampleLayout, m_descriptorSets[i], &params);
    }
    // Up: level i + 1 added to level i at half weight.
    for (int32_t i = static_cast<int32_t>(m_mipLevels) - 2; i >= 0; --i) {
        const SampleParams params{glm::vec2(1.0f / m_mipSizes[i + 1].x, 1.0f / m_mipSizes[i + 1].y), m_filterRadius, 0.5f};
        draw(frame, static_cast<uint32_t>(i), render::Load::Keep, m_upsamplePipeline, m_upsampleLayout,
             m_descriptorSets[i + 2], &params);
    }

    m_executed = true;
}

void BloomPass::draw(const render::Frame& frame, uint32_t mip, render::Load load, VkPipeline pipeline,
                     VkPipelineLayout layout, VkDescriptorSet set, const void* params) {
    render::Pass pass(frame, nullptr, render::Pass::Raster);
    // A level samples its neighbour: the one above going down, the one below coming up. Level 0's input is the frame.
    if (pipeline == m_inputPipeline) pass.sampled(*m_hdrInput);
    if (pipeline == m_downsamplePipeline) pass.sampled(m_chain, {mip - 1, 0});
    if (pipeline == m_upsamplePipeline) pass.sampled(m_chain, {mip + 1, 0});
    pass.color(m_chain, load, {}, mip);
    pass.begin();
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    if (params) vkCmdPushConstants(frame.cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SampleParams), params);
    vkCmdDraw(frame.cmd, 3, 1, 0, 0);
}

float BloomPass::levelWeightSum() const {
    // record() adds each level to the one above at 0.5.
    float sum = 0.0f, w = 1.0f;
    for (uint32_t i = 0; i < m_mipLevels; ++i, w *= 0.5f) sum += w;
    return sum > 0.0f ? sum : 1.0f;
}

void BloomPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    vkDeviceWaitIdle(m_device);   // the descriptor pool below is replaced
    destroyMipChain();
    if (!createMipChain() || !createDescriptors()) {
        OHAO_LOG(Error, render, "BloomPass: resize to {}x{} failed; bloom disabled", width, height);
    }
}

bool BloomPass::createMipChain() {
    m_mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(m_width, m_height)))) + 1;
    m_mipLevels = std::min(m_mipLevels, MAX_MIP_LEVELS);

    uint32_t w = m_width;
    uint32_t h = m_height;
    for (uint32_t i = 0; i < m_mipLevels; ++i) {
        m_mipSizes[i] = glm::uvec2(w, h);
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }

    m_chain = m_allocator->createImage({"bloom.chain", VK_FORMAT_R16G16B16A16_SFLOAT, m_width, m_height,
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, m_mipLevels});
    if (!m_chain) return false;

    if (m_sampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) return false;
    }
    return true;
}

void BloomPass::destroyMipChain() {
    if (m_allocator) m_allocator->retire(m_chain);
}

bool BloomPass::createDescriptors() {
    // One input image, the layout all three bloom shaders share.
    const VkDescriptorSetLayoutBinding binding = shader::layoutBinding(kInput, VK_SHADER_STAGE_FRAGMENT_BIT);

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    if (m_inputLayout == VK_NULL_HANDLE) {
        if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_inputLayout) != VK_SUCCESS) {
            return false;
        }
    }

    // Pool for mip chain descriptors + one HDR input set per frame in flight
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = m_mipLevels + MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = m_mipLevels + MAX_FRAMES_IN_FLIGHT;

    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    }

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    // Allocate descriptor sets: one per mip at [i + 1], plus the per-frame input sets
    m_descriptorSets.assign(m_mipLevels + 1, VK_NULL_HANDLE);
    std::vector<VkDescriptorSetLayout> layouts(m_mipLevels, m_inputLayout);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocInfo.pSetLayouts = layouts.data();

    if (vkAllocateDescriptorSets(m_device, &allocInfo, m_descriptorSets.data() + 1) != VK_SUCCESS) {
        return false;
    }

    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> inputLayouts;
    inputLayouts.fill(m_inputLayout);
    allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocInfo.pSetLayouts = inputLayouts.data();
    if (vkAllocateDescriptorSets(m_device, &allocInfo, m_inputSets.data()) != VK_SUCCESS) {
        return false;
    }

    // Update descriptor sets
    // Set 0: HDR input (updated when setInputImage is called)
    // Sets 1-N: Mip levels for downsampling/upsampling
    for (uint32_t i = 0; i < m_mipLevels; ++i) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = m_sampler;
        imageInfo.imageView = m_chain.subView(i, 0);
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_descriptorSets[i + 1];
        write.dstBinding = kInput.binding;
        write.descriptorType = kInput.type;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }

    return true;
}

bool BloomPass::createPipelines() {
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SampleParams)};
    const auto makeLayout = [&](bool pushed, VkPipelineLayout& out) {
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1;
        info.pSetLayouts = &m_inputLayout;
        info.pushConstantRangeCount = pushed ? 1u : 0u;
        info.pPushConstantRanges = pushed ? &push : nullptr;
        return vkCreatePipelineLayout(m_device, &info, nullptr, &out) == VK_SUCCESS;
    };
    if (!makeLayout(false, m_inputPipelineLayout) || !makeLayout(true, m_downsampleLayout) ||
        !makeLayout(true, m_upsampleLayout)) return false;

    const VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
    const render::Blend add = render::Blend::additive();   // the upsample adds into its level
    render::GraphicsPipelineDesc desc{};
    desc.vertex = "postprocess_fullscreen.slang.spv";
    desc.colorFormats = {&format, 1};

    desc.name = "bloom.input";
    desc.fragment = "postprocess_bloom_input.slang.spv";
    desc.layout = m_inputPipelineLayout;
    m_inputPipeline = render::createGraphicsPipeline(m_device, desc);

    desc.name = "bloom.downsample";
    desc.fragment = "postprocess_bloom_downsample.slang.spv";
    desc.layout = m_downsampleLayout;
    m_downsamplePipeline = render::createGraphicsPipeline(m_device, desc);

    desc.name = "bloom.upsample";
    desc.fragment = "postprocess_bloom_upsample.slang.spv";
    desc.layout = m_upsampleLayout;
    desc.blends = {&add, 1};
    m_upsamplePipeline = render::createGraphicsPipeline(m_device, desc);

    return m_inputPipeline != VK_NULL_HANDLE && m_downsamplePipeline != VK_NULL_HANDLE &&
           m_upsamplePipeline != VK_NULL_HANDLE;
}

} // namespace ohao
