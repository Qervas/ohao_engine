#include "taa_pass.hpp"
#include "render/shader_bindings/postprocess_taa_resolve.hpp"
#include "render/graph/pipeline.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"
#include "core/log.hpp"
#include <array>
#include <string>

namespace ohao {

namespace {
constexpr VkFormat kHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
} // namespace

// Halton sequence for sub-pixel jitter
static float halton(int index, int base) {
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

TAAPass::~TAAPass() {
    cleanup();
}

bool TAAPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    m_width = 1920;
    m_height = 1080;

    // Generate Halton sequence for jitter
    for (uint32_t i = 0; i < UPSCALE_JITTER_SAMPLES; ++i) {
        m_jitterSequence[i] = glm::vec2(
            halton(i + 1, 2) - 0.5f,
            halton(i + 1, 3) - 0.5f
        );
    }

    if (!m_allocator) return false;
    if (!createHistoryBuffers()) return false;
    if (!createDescriptors()) return false;
    if (!createPipeline()) return false;

    return true;
}

void TAAPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    if (m_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    if (m_descriptorLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, m_descriptorLayout, nullptr);
    if (m_sampler != VK_NULL_HANDLE) vkDestroySampler(m_device, m_sampler, nullptr);

    destroyHistoryBuffers();

    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_descriptorPool = VK_NULL_HANDLE;
    m_descriptorLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
}

void TAAPass::record(const render::Frame& frame) {
    if (!m_currentFrame || !*m_currentFrame || m_pipeline == VK_NULL_HANDLE) return;
    if (frame.slot >= MAX_FRAMES_IN_FLIGHT || m_descriptorSets[frame.slot] == VK_NULL_HANDLE) return;

    // Ping-pong: write one history image while sampling the other, which holds
    // last frame's result. Swap first, so output() names the image written here.
    m_currentHistoryIndex = 1 - m_currentHistoryIndex;
    const uint32_t outputIndex = m_currentHistoryIndex;
    gpu::Image& history = m_history[1 - outputIndex];
    if (!history || !m_history[outputIndex]) return;

    if (!m_historyValid) clearHistory(frame, history);
    writeDescriptorSet(frame.slot, 1 - outputIndex);

    render::Pass pass(frame, "post.taa", render::Pass::Raster);
    pass.sampled(*m_currentFrame).sampled(history);
    if (m_mask) pass.sampled(*m_mask);
    if (m_velocity) pass.sampled(*m_velocity);
    if (m_depth) pass.sampled(*m_depth);
    pass.color(m_history[outputIndex], render::Load::Clear, {{0.0f, 0.0f, 0.0f, 1.0f}});
    pass.begin();
    const VkCommandBuffer cmd = frame.cmd;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                            0, 1, &m_descriptorSets[frame.slot], 0, nullptr);

    TAAParams params{};
    params.texelSize = glm::vec2(1.0f / m_width, 1.0f / m_height);
    params.inputTexelSize = glm::vec2(1.0f / (m_inputWidth ? m_inputWidth : m_width), 1.0f / (m_inputHeight ? m_inputHeight : m_height));
    // The projection adds the jitter to clip x and y per unit of view z, and w is -z: the image moves by -jitter
    // in NDC, half of that in uv.
    params.jitterUv = -0.5f * m_frameJitter;
    params.sharpness = 1.0f / (2.0f * 0.47f * 0.47f);   // Unreal's TAAU: sigma 0.47 output pixels
    params.blendFactor = m_historyValid ? m_blendFactor : 0.0f;  // no history yet: current frame only
    params.motionScale = m_motionScale;
    params.flags = 0;
    if (m_velocity && *m_velocity) params.flags |= 1;
    if (m_useVarianceClipping) params.flags |= 2;
    if (m_mask) params.flags |= 4;   // the transparency mask is bound

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    m_historyValid = true;
}

void TAAPass::clearHistory(const render::Frame& frame, gpu::Image& history) {
    // After a reset, frames still in flight may write (TAA) or sample (TAA, tonemap) the image:
    // the clear waits for them by queue order, not for the device.
    render::Pass pass(frame, nullptr, render::Pass::Transfer);
    pass.copyDst(history);
    pass.begin();
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(frame.cmd, history.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
}

void TAAPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    destroyHistoryBuffers();
    if (!createHistoryBuffers())
        OHAO_LOG(Error, render, "TAAPass: resize to {}x{} failed; TAA disabled", width, height);
}

glm::vec2 TAAPass::getJitterOffset(uint32_t frameIndex) const {
    const glm::vec2 input(m_inputWidth ? m_inputWidth : m_width, m_inputHeight ? m_inputHeight : m_height);
    // +-1/2 of a rendered pixel: NDC spans 2.
    return 2.0f * m_jitterSequence[frameIndex % (upscales() ? UPSCALE_JITTER_SAMPLES : JITTER_SAMPLES)] / input;
}

void TAAPass::setTransparencyMask(gpu::Image* mask) {
    m_mask = mask && *mask ? mask : nullptr;
}

void TAAPass::writeDescriptorSet(uint32_t slot, uint32_t historyIdx) {
    if (m_sampler == VK_NULL_HANDLE) return;

    // Every binding is written, the current frame where a view is missing: an unwritten descriptor crashed NVIDIA's driver.
    const VkImageView fallbackView = m_currentFrame->view;
    const VkImageView velocityView = m_velocity ? m_velocity->view : VK_NULL_HANDLE;
    const VkImageView depthView = m_depth ? m_depth->view : VK_NULL_HANDLE;
    namespace T = shader::postprocess_taa_resolve;
    constexpr VkImageLayout kRead = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const auto pick = [&](VkImageView v) { return v != VK_NULL_HANDLE ? v : fallbackView; };
    // A depth image is sampled in the layout it is tested in.
    const VkImageLayout depthLayout =
        depthView != VK_NULL_HANDLE ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : kRead;
    const std::array<std::pair<shader::Binding, VkDescriptorImageInfo>, T::kSet0.size()> images{{
        {T::currentFrame, {m_sampler, m_currentFrame->view, kRead}},
        {T::historyFrame, {m_sampler, m_history[historyIdx].view, kRead}},      // the opposite buffer
        {T::velocityBuffer, {m_sampler, pick(velocityView), kRead}},            // the shader's flags skip a fallback
        {T::depthBuffer, {m_sampler, pick(depthView), depthLayout}},
        {T::transparencyMask, {m_sampler, pick(m_mask ? m_mask->view : VkImageView{VK_NULL_HANDLE}), kRead}},            // flag bit 2 says it is bound
    }};
    std::array<VkWriteDescriptorSet, T::kSet0.size()> writes{};
    for (std::size_t i = 0; i < images.size(); ++i) {
        writes[i] = shader::write(images[i].first, m_descriptorSets[slot]);
        writes[i].pImageInfo = &images[i].second;
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

bool TAAPass::createHistoryBuffers() {
    m_historyValid = false;   // new images hold nothing; the next record() clears the one it samples

    for (uint32_t i = 0; i < HISTORY_COUNT; ++i) {
        m_history[i] = m_allocator->createImage(
            {"taa.history" + std::to_string(i), kHistoryFormat, m_width, m_height,
             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT});   // clearHistory()
        if (!m_history[i]) return false;
    }

    // Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    if (m_sampler == VK_NULL_HANDLE) {
        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

void TAAPass::destroyHistoryBuffers() {
    if (!m_allocator) return;
    for (gpu::Image& image : m_history) m_allocator->retire(image);
}

bool TAAPass::createDescriptors() {
    const auto bindings = shader::layoutBindings(shader::postprocess_taa_resolve::kSet0, VK_SHADER_STAGE_FRAGMENT_BIT);

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorLayout) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 5 * MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(m_descriptorLayout);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocInfo.pSetLayouts = layouts.data();

    if (vkAllocateDescriptorSets(m_device, &allocInfo, m_descriptorSets.data()) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool TAAPass::createPipeline() {
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(TAAParams);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) return false;

    render::GraphicsPipelineDesc desc{};
    desc.name = "taa.resolve";
    desc.vertex = "postprocess_fullscreen.slang.spv";
    desc.fragment = "postprocess_taa_resolve.slang.spv";
    desc.layout = m_pipelineLayout;
    desc.colorFormats = {&kHistoryFormat, 1};
    m_pipeline = render::createGraphicsPipeline(m_device, desc);
    return m_pipeline != VK_NULL_HANDLE;
}

} // namespace ohao
