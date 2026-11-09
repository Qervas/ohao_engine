#include "render/hud/hud_pass.hpp"
#include "render/shader_bindings/hud_hud.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cstring>

namespace ohao {

namespace {

struct HudPush {
    float invSize[2];
    float pad[2];
};
OHAO_CHECK_PUSH(HudPush, hud_hud)   // hud/hud.slang's

constexpr VkDeviceSize kItemBytes = sizeof(render::HudItem) * render::kHudMaxItems;

} // namespace

HudPass::~HudPass() { cleanup(); }

bool HudPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding bindings[] = {shader::layoutBinding(shader::hud_hud::items, stages),
                                                     shader::layoutBinding(shader::hud_hud::font, stages)};
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 2;
    li.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_setLayout) != VK_SUCCESS) return false;

    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;

    // The font, once; the items, a buffer a frame slot (written when its frame is recorded).
    const auto rows = render::hudFontRows();
    if (!rtc::createHostBuffer(m_device, m_physicalDevice, rows.size_bytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_font))
        return false;
    std::memcpy(m_font.mapped, rows.data(), rows.size_bytes());
    for (uint32_t s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s) {
        if (!rtc::createHostBuffer(m_device, m_physicalDevice, kItemBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_itemBuffers[s]))
            return false;
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &m_setLayout;
        if (vkAllocateDescriptorSets(m_device, &ai, &m_sets[s]) != VK_SUCCESS) return false;
        const VkDescriptorBufferInfo items{m_itemBuffers[s].buffer, 0, kItemBytes};
        const VkDescriptorBufferInfo font{m_font.buffer, 0, rows.size_bytes()};
        VkWriteDescriptorSet writes[2] = {shader::write(shader::hud_hud::items, m_sets[s]),
                                          shader::write(shader::hud_hud::font, m_sets[s])};
        writes[0].pBufferInfo = &items;
        writes[1].pBufferInfo = &font;
        vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
    }
    if (!createPipeline()) {
        OHAO_LOG(Error, render, "HudPass: pipeline failed");
        return false;
    }
    return true;
}

bool HudPass::createPipeline() {
    VkShaderModule vert = loadShaderModule("hud_hud.slang.vs.spv");
    VkShaderModule frag = loadShaderModule("hud_hud.slang.fs.spv");
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
        if (vert) vkDestroyShaderModule(m_device, vert, nullptr);
        if (frag) vkDestroyShaderModule(m_device, frag, nullptr);
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "vs";   // Slang keeps the entry points' names (-fvk-use-entrypoint-name)
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "fs";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    // Over the image by alpha; the image's alpha kept.
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dyn;

    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(HudPush)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    bool ok = vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_layout) == VK_SUCCESS;

    // The final image, loaded and kept; it arrives as a color attachment (execute's barrier) and
    // leaves for sampling, preview and copies.
    VkAttachmentDescription color{};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    VkSubpassDependency out{};
    out.srcSubpass = 0;
    out.dstSubpass = VK_SUBPASS_EXTERNAL;
    out.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    out.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                       VK_PIPELINE_STAGE_TRANSFER_BIT;
    out.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    out.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 1;
    rp.pDependencies = &out;
    ok = ok && vkCreateRenderPass(m_device, &rp, nullptr, &m_renderPass) == VK_SUCCESS;

    if (ok) {
        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vertexInput;
        gp.pInputAssemblyState = &assembly;
        gp.pViewportState = &viewport;
        gp.pRasterizationState = &raster;
        gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &depth;
        gp.pColorBlendState = &blending;
        gp.pDynamicState = &dynamic;
        gp.layout = m_layout;
        gp.renderPass = m_renderPass;
        ok = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &gp, nullptr, &m_pipeline) == VK_SUCCESS;
    }
    vkDestroyShaderModule(m_device, vert, nullptr);
    vkDestroyShaderModule(m_device, frag, nullptr);
    return ok;
}

void HudPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    destroyFramebuffer();
    safeDestroy(m_pipeline);
    safeDestroy(m_layout);
    safeDestroy(m_renderPass);
    safeDestroy(m_pool);   // frees the sets
    safeDestroy(m_setLayout);
    for (auto& b : m_itemBuffers) rtc::destroyHostBuffer(m_device, b);
    rtc::destroyHostBuffer(m_device, m_font);
    m_sets = {};
    m_device = VK_NULL_HANDLE;
}

void HudPass::setItems(std::span<const render::HudItem> items) {
    m_items.assign(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(std::min(items.size(), render::kHudMaxItems)));
}

void HudPass::destroyFramebuffer() {
    if (m_framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
    m_framebuffer = VK_NULL_HANDLE;
}

void HudPass::onResize(uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;
    destroyFramebuffer();   // the final image is recreated: setTarget makes the next one
    m_targetImage = VK_NULL_HANDLE;
    m_targetView = VK_NULL_HANDLE;
}

void HudPass::setTarget(VkImage image, VkImageView view, uint32_t width, uint32_t height) {
    if (image == m_targetImage && view == m_targetView && width == m_width && height == m_height &&
        m_framebuffer != VK_NULL_HANDLE)
        return;
    destroyFramebuffer();
    m_targetImage = image;
    m_targetView = view;
    m_width = width;
    m_height = height;
    if (view == VK_NULL_HANDLE || m_renderPass == VK_NULL_HANDLE || m_width == 0 || m_height == 0) return;
    VkFramebufferCreateInfo fb{};
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass = m_renderPass;
    fb.attachmentCount = 1;
    fb.pAttachments = &m_targetView;
    fb.width = m_width;
    fb.height = m_height;
    fb.layers = 1;
    if (vkCreateFramebuffer(m_device, &fb, nullptr, &m_framebuffer) != VK_SUCCESS) {
        OHAO_LOG(Error, render, "HudPass: framebuffer failed");
        m_framebuffer = VK_NULL_HANDLE;
    }
}

void HudPass::execute(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (m_items.empty() || m_framebuffer == VK_NULL_HANDLE || m_pipeline == VK_NULL_HANDLE) return;
    const uint32_t slot = frameIndex % MAX_FRAMES_IN_FLIGHT;
    std::memcpy(m_itemBuffers[slot].mapped, m_items.data(), m_items.size() * sizeof(render::HudItem));

    // The final image's writers (tonemapping's render pass, the gizmo, a compute pass) before it is
    // loaded and blended over.
    VkImageMemoryBarrier toAttachment{};
    toAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toAttachment.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    toAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toAttachment.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAttachment.srcQueueFamilyIndex = toAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toAttachment.image = m_targetImage;
    toAttachment.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &toAttachment);

    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = m_renderPass;
    begin.framebuffer = m_framebuffer;
    begin.renderArea.extent = {m_width, m_height};
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {m_width, m_height}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_sets[slot], 0, nullptr);
    const HudPush pc{{1.0f / static_cast<float>(m_width), 1.0f / static_cast<float>(m_height)}, {0.0f, 0.0f}};
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 6, static_cast<uint32_t>(m_items.size()), 0, 0);
    vkCmdEndRenderPass(cmd);   // the image back in SHADER_READ_ONLY_OPTIMAL, its writes visible to readers
}

} // namespace ohao
