#include "post_processing_pipeline.hpp"
#include "render/shader_bindings/postprocess_tonemap.hpp"
#include "render/graph/pipeline.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"
#include "core/log.hpp"
#include <array>

namespace ohao {

namespace {
constexpr VkFormat kOutputFormat = VK_FORMAT_R8G8B8A8_UNORM;
} // namespace

PostProcessingPipeline::~PostProcessingPipeline() {
    cleanup();
}

bool PostProcessingPipeline::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    m_width = 1920;
    m_height = 1080;

    // Initialize sub-passes
    m_bloomPass = std::make_unique<BloomPass>();
    m_bloomPass->setAllocator(*m_allocator);
    if (!m_bloomPass->initialize(device, physicalDevice)) {
        return false;
    }

    m_taaPass = std::make_unique<TAAPass>();
    m_taaPass->setAllocator(*m_allocator);
    if (!m_taaPass->initialize(device, physicalDevice)) {
        return false;
    }

    // The display pass binds the metered EV100 whether or not a window meters.
    m_autoExposure = std::make_unique<AutoExposure>();
    if (!m_autoExposure->initialize(device, *m_allocator)) return false;

    // Create tonemapping pass
    if (!createFinalOutput()) return false;
    if (!createTonemappingPass()) return false;

    return true;
}

void PostProcessingPipeline::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    // Cleanup sub-passes
    auto resetPass = [](auto& pass) {
        if (pass) { pass->cleanup(); pass.reset(); }
    };
    resetPass(m_bloomPass);
    resetPass(m_taaPass);
    resetPass(m_autoExposure);

    // Cleanup tonemapping
    safeDestroy(m_tonemapPipeline);
    safeDestroy(m_tonemapLayout);
    safeDestroy(m_tonemapDescPool);
    safeDestroy(m_tonemapDescLayout);
    safeDestroy(m_sampler);

    destroyFinalOutput();
}

void PostProcessingPipeline::record(const render::Frame& frame) {
    m_didExecute = false;

    if (!m_hdrInput || !*m_hdrInput) return;

    gpu::Image* current = m_hdrInput;   // the frame so far: the lit image, then TAA's resolve

    // ── 1. Bloom ──────────────────────────────────────────────────────
    float bloomStrength = 0.0f;   // bloom's share of the frame (kBloomStrength when it ran)
    if (m_bloomEnabled && m_bloomPass) {
        render::Timed timed(frame, "post.bloom");
        m_bloomPass->setInputImage(current);
        m_bloomPass->record(frame);
        if (m_bloomPass->didExecute()) bloomStrength = kBloomStrength;
    }

    // ── 2. TAA (temporal anti-aliasing) ───────────────────────────────
    if (m_taaEnabled && m_taaPass) {
        m_taaPass->setCurrentFrame(current);
        m_taaPass->record(frame);
        if (gpu::Image& resolved = m_taaPass->output(); resolved) current = &resolved;
    }

    // ── 3. Auto exposure (a window's): the frame it shows, metered ─────
    const bool metered = m_autoExposureEnabled && m_autoExposure && frame.slot < MAX_FRAMES_IN_FLIGHT;
    if (metered) {
        render::Timed timed(frame, "exposure");
        m_autoExposure->record(frame, *current, m_sampler, m_width, m_height, m_frameDt, m_autoExposureSettings);
    }

    // ── 4. Tonemapping: HDR → LDR ─────────────────────────────────────
    if (m_tonemapPipeline != VK_NULL_HANDLE && m_output && frame.slot < MAX_FRAMES_IN_FLIGHT &&
        m_tonemapDescSets[frame.slot] != VK_NULL_HANDLE) {
        // Bloom output only when bloom ran this frame; otherwise the image was never
        // written. Strength is 0 then, so any read-only image fills the binding.
        const bool bloomed = bloomStrength > 0.0f;
        writeTonemapSet(frame.slot, current->view, bloomed ? m_bloomPass->chain().subView(0, 0) : m_hdrInput->view);

        render::Pass pass(frame, "post.tonemap", render::Pass::Raster);
        pass.sampled(*current);
        if (bloomed) pass.sampled(m_bloomPass->chain(), {0, 0});
        else pass.sampled(*m_hdrInput);
        pass.read(m_autoExposure->stateBuffer(), render::BufferUse::Storage);
        pass.color(m_output, render::Load::Clear, {{0.0f, 0.0f, 0.0f, 1.0f}});
        pass.begin();
        const VkCommandBuffer cmd = frame.cmd;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_tonemapLayout, 0, 1, &m_tonemapDescSets[frame.slot], 0, nullptr);

        TonemapParams params{};
        params.exposure      = render::displayExposure(m_ev100);
        params.bloomWeight   = bloomed ? bloomStrength / m_bloomPass->levelWeightSum() : 0.0f;
        params.frameWeight   = 1.0f - bloomStrength;
        params.curve         = static_cast<uint32_t>(m_displayCurve);
        params.autoExposure  = metered ? 1u : 0u;

        vkCmdPushConstants(cmd, m_tonemapLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(params), &params);

        vkCmdDraw(cmd, 3, 1, 0, 0);

        m_didExecute = true;
    }
}

void PostProcessingPipeline::setRenderSize(uint32_t width, uint32_t height) {
    if (width == m_renderWidth && height == m_renderHeight) return;
    m_renderWidth = width;
    m_renderHeight = height;
    if (m_bloomPass) m_bloomPass->onResize(width, height);
    if (m_taaPass) {
        m_taaPass->setInputSize(width, height);
        m_taaPass->resetHistory();
    }
}

void PostProcessingPipeline::setFrameJitter(glm::vec2 ndc) {
    if (m_taaPass) m_taaPass->setFrameJitter(ndc);
}

void PostProcessingPipeline::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    // Resize sub-passes
    if (m_bloomPass) m_bloomPass->onResize(m_renderWidth ? m_renderWidth : width, m_renderHeight ? m_renderHeight : height);
    if (m_taaPass) m_taaPass->onResize(width, height);

    destroyFinalOutput();
    if (!createFinalOutput())
        OHAO_LOG(Error, render, "PostProcessingPipeline: the final image failed at {}x{}", width, height);
}

void PostProcessingPipeline::setGBufferInputs(gpu::Image* depth, gpu::Image* velocity) {
    if (m_taaPass) m_taaPass->setGBufferInputs(depth, velocity);
}

void PostProcessingPipeline::setTransparencyMask(gpu::Image* mask) {
    if (m_taaPass) m_taaPass->setTransparencyMask(mask);
}

void PostProcessingPipeline::setTAABlendFactor(float factor) {
    if (m_taaPass) m_taaPass->setBlendFactor(factor);
}

void PostProcessingPipeline::resetHistory() noexcept {
    if (m_taaPass) m_taaPass->resetHistory();
    if (m_autoExposure) m_autoExposure->reset();
}

glm::vec2 PostProcessingPipeline::getJitterOffset(uint32_t frameIndex) const {
    if (m_taaPass && m_taaEnabled) {
        return m_taaPass->getJitterOffset(frameIndex);
    }
    return glm::vec2(0.0f);
}

void PostProcessingPipeline::writeTonemapSet(uint32_t slot, VkImageView input, VkImageView bloom) {
    const VkDescriptorSet set = m_tonemapDescSets[slot];
    if (set == VK_NULL_HANDLE || m_sampler == VK_NULL_HANDLE) return;
    if (input == VK_NULL_HANDLE || bloom == VK_NULL_HANDLE) return;

    // The HDR input, the bloom, and auto exposure's state (x EV100).
    namespace T = shader::postprocess_tonemap;
    const VkDescriptorImageInfo images[2] = {{m_sampler, input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                             {m_sampler, bloom, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    const VkDescriptorBufferInfo exposureInfo{m_autoExposure->stateBuffer().buffer, 0, VK_WHOLE_SIZE};
    std::array<VkWriteDescriptorSet, 3> writes{shader::write(T::hdrInput, set), shader::write(T::bloomInput, set),
                                               shader::write(T::exposureState, set)};
    writes[0].pImageInfo = &images[0];
    writes[1].pImageInfo = &images[1];
    writes[2].pBufferInfo = &exposureInfo;

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

bool PostProcessingPipeline::createFinalOutput() {
    // LDR: the display pass encodes sRGB itself.
    m_output = m_allocator->createImage({"post.output", kOutputFormat, m_width, m_height,
                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    return static_cast<bool>(m_output);
}

void PostProcessingPipeline::destroyFinalOutput() {
    if (m_allocator) m_allocator->retire(m_output);
}

bool PostProcessingPipeline::createTonemappingPass() {
    // Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        return false;
    }

    const auto bindings = shader::layoutBindings(shader::postprocess_tonemap::kSet0, shader::postprocess_tonemap::kStages);

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_tonemapDescLayout) != VK_SUCCESS) {
        return false;
    }

    // Descriptor pool
    const VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * MAX_FRAMES_IN_FLIGHT},  // HDR + bloom, per frame slot
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES_IN_FLIGHT},           // the metered EV100
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_tonemapDescPool) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_tonemapDescPool;
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> setLayouts;
    setLayouts.fill(m_tonemapDescLayout);
    allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocInfo.pSetLayouts = setLayouts.data();

    if (vkAllocateDescriptorSets(m_device, &allocInfo, m_tonemapDescSets.data()) != VK_SUCCESS) {
        return false;
    }

    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(TonemapParams);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_tonemapDescLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_tonemapLayout) != VK_SUCCESS) return false;

    render::GraphicsPipelineDesc desc{};
    desc.name = "post.tonemap";
    desc.vertex = "postprocess_fullscreen.slang.spv";
    desc.fragment = "postprocess_tonemap.slang.spv";
    desc.layout = m_tonemapLayout;
    desc.colorFormats = {&kOutputFormat, 1};
    m_tonemapPipeline = render::createGraphicsPipeline(m_device, desc);
    return m_tonemapPipeline != VK_NULL_HANDLE;
}

} // namespace ohao
