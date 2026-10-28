#include "render/vfx/decal_pass.hpp"

#include "core/log.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace ohao {

namespace {

constexpr VkImageLayout kReadOnly = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
constexpr VkImageLayout kDepthReadOnly = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
constexpr VkFormat kAlbedoFormat = VK_FORMAT_R8G8B8A8_SRGB;        // the G-buffer's albedo target
constexpr VkFormat kNormalFormat = VK_FORMAT_R16G16B16A16_SFLOAT;  // the G-buffer's normal target
constexpr uint32_t kDecalVertices = 36;                            // six faces, two triangles each
constexpr uint32_t kDecalCapacity = 256;                           // caps.decals: the set's ring

template <size_t N>
void copyMatrix(std::array<float, N>& out, const glm::mat4& m) {
    std::memcpy(out.data(), &m[0][0], sizeof(float) * 16);
}

} // namespace

DecalPass::~DecalPass() { cleanup(); }

bool DecalPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice, VkDescriptorSetLayout textures) {
    cleanup();
    m_device = device;
    m_physical = physicalDevice;
    if (!createLayouts(textures) || !createRenderPass() || !createPipeline()) {
        OHAO_LOG(Warn, render, "DecalPass: the pass could not be made; no decals are drawn");
        cleanup();
        return false;
    }
    return true;
}

bool DecalPass::createLayouts(VkDescriptorSetLayout textures) {
    // Set 0: the records, the position and the depth (sampled where the decal's surface is read).
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings{{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    }};
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_setLayout) != VK_SUCCESS) return false;

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(gpu::shared::DecalPush);
    const std::array<VkDescriptorSetLayout, 2> setLayouts{m_setLayout, textures};
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    pl.pSetLayouts = setLayouts.data();
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(m_device, &pl, nullptr, &m_pipelineLayout) != VK_SUCCESS) return false;

    const std::array<VkDescriptorPoolSize, 2> sizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * MAX_FRAMES_IN_FLIGHT},
    }};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = MAX_FRAMES_IN_FLIGHT;
    pi.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pi.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;

    for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &m_setLayout;
        if (vkAllocateDescriptorSets(m_device, &ai, &m_slots[slot].set) != VK_SUCCESS) return false;
    }
    return true;
}

bool DecalPass::createPipeline() {
    const VkShaderModule vert = rtc::loadShaderModule(m_device, "vfx_decal.slang.vertexMain.spv");
    const VkShaderModule frag = rtc::loadShaderModule(m_device, "vfx_decal.slang.fragmentMain.spv");
    if (!vert || !frag) {
        for (VkShaderModule m : {vert, frag})
            if (m) vkDestroyShaderModule(m_device, m, nullptr);
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& s : stages) s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "vertexMain";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "fragmentMain";

    VkPipelineVertexInputStateCreateInfo noVertices{};
    noVertices.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = vp.scissorCount = 1;
    // The box's far faces: the counter-clockwise faces seen from outside are culled as front faces.
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_FRONT_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // No depth test: a surface is decaled by its position, read per pixel, and the box test decides.
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    // Albedo: blended by the decal's alpha, its alpha (the G-buffer's AO) untouched.
    // Normal: replaced, RGB only (the w channel keeps the luminance or the subsurface strength).
    std::array<VkPipelineColorBlendAttachmentState, 2> blends{};
    blends[0].blendEnable = VK_TRUE;
    blends[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blends[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blends[0].colorBlendOp = VK_BLEND_OP_ADD;
    blends[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blends[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blends[0].alphaBlendOp = VK_BLEND_OP_ADD;
    blends[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    blends[1].blendEnable = VK_FALSE;
    blends[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = static_cast<uint32_t>(blends.size());
    cb.pAttachments = blends.data();
    const std::array<VkDynamicState, 2> dyn{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = static_cast<uint32_t>(dyn.size());
    dy.pDynamicStates = dyn.data();

    // The pass's render pass is made in createRenderPass; the pipeline needs it, so createPipeline
    // runs after it (see initialize's order: layouts, render pass, pipeline).
    VkGraphicsPipelineCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &noVertices;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = m_pipelineLayout;
    pi.renderPass = m_renderPass;
    const bool ok = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pi, nullptr, &m_pipeline) == VK_SUCCESS;
    for (VkShaderModule m : {vert, frag}) vkDestroyShaderModule(m_device, m, nullptr);
    return ok;
}

bool DecalPass::createRenderPass() {
    // Both targets are loaded and stored in the layout they had after gbuffer.late, and left there.
    std::array<VkAttachmentDescription, 2> a{};
    a[0].format = kAlbedoFormat;
    a[1].format = kNormalFormat;
    for (auto& x : a) {
        x.samples = VK_SAMPLE_COUNT_1_BIT;
        x.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        x.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        x.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        x.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        x.initialLayout = kReadOnly;
        x.finalLayout = kReadOnly;
    }
    std::array<VkAttachmentReference, 2> refs{};
    refs[0] = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    refs[1] = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = static_cast<uint32_t>(refs.size());
    sub.pColorAttachments = refs.data();
    // Before: gbuffer.late's writes, and the earlier passes' reads of these targets. After: the
    // lighting's reads of the decaled targets.
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_SHADER_READ_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ri.attachmentCount = static_cast<uint32_t>(a.size());
    ri.pAttachments = a.data();
    ri.subpassCount = 1;
    ri.pSubpasses = &sub;
    ri.dependencyCount = static_cast<uint32_t>(deps.size());
    ri.pDependencies = deps.data();
    return vkCreateRenderPass(m_device, &ri, nullptr, &m_renderPass) == VK_SUCCESS;
}

void DecalPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);
    destroySizeResources();
    if (m_pipeline) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    m_pipelineLayout = VK_NULL_HANDLE;
    if (m_renderPass) vkDestroyRenderPass(m_device, m_renderPass, nullptr);
    m_renderPass = VK_NULL_HANDLE;
    if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);   // frees the sets
    m_pool = VK_NULL_HANDLE;
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    m_setLayout = VK_NULL_HANDLE;
    for (Slot& s : m_slots) {
        rtc::destroyHostBuffer(m_device, s.decals);
        s = Slot{};
    }
    m_device = VK_NULL_HANDLE;
}

void DecalPass::destroySizeResources() {
    if (m_framebuffer) vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
    m_framebuffer = VK_NULL_HANDLE;
}

bool DecalPass::resize(uint32_t width, uint32_t height, VkImageView albedo, VkImageView normal, VkImageView position,
                       VkImageView depth) {
    if (m_device == VK_NULL_HANDLE || m_renderPass == VK_NULL_HANDLE) return false;
    destroySizeResources();
    m_width = std::max(width, 1u);
    m_height = std::max(height, 1u);
    m_albedo = albedo;
    m_normal = normal;
    m_position = position;
    m_depth = depth;
    const std::array<VkImageView, 2> views{albedo, normal};
    VkFramebufferCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = m_renderPass;
    fi.attachmentCount = static_cast<uint32_t>(views.size());
    fi.pAttachments = views.data();
    fi.width = m_width;
    fi.height = m_height;
    fi.layers = 1;
    if (vkCreateFramebuffer(m_device, &fi, nullptr, &m_framebuffer) != VK_SUCCESS) return false;
    // The sampled images changed: every slot's set is written again (the device is idle).
    for (uint32_t slot = 0; slot < m_slots.size(); ++slot) writeSet(slot, m_slots[slot].decals.buffer);
    return true;
}

void DecalPass::writeSet(uint32_t slot, VkBuffer decals) {
    Slot& s = m_slots[slot];
    if (s.set == VK_NULL_HANDLE) return;
    std::vector<VkWriteDescriptorSet> w;
    VkDescriptorBufferInfo buffer{decals, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo position{VK_NULL_HANDLE, m_position, kReadOnly};
    VkDescriptorImageInfo depth{VK_NULL_HANDLE, m_depth, kDepthReadOnly};
    if (decals != VK_NULL_HANDLE) {
        w.push_back(rtc::descriptorWrite(s.set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER));
        w.back().pBufferInfo = &buffer;
    }
    if (m_position != VK_NULL_HANDLE) {
        w.push_back(rtc::descriptorWrite(s.set, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE));
        w.back().pImageInfo = &position;
    }
    if (m_depth != VK_NULL_HANDLE) {
        w.push_back(rtc::descriptorWrite(s.set, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE));
        w.back().pImageInfo = &depth;
    }
    if (!w.empty()) vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
}

void DecalPass::prepare(uint32_t slot, const render::VfxFrame& frame) {
    if (slot >= m_slots.size()) return;
    std::vector<gpu::shared::DecalGpu> table;
    if (frame.set) {
        const vfx::VfxSet& set = *frame.set;
        table.reserve(frame.decals.size());
        for (const render::VfxFrameDecal& d : frame.decals) {
            if (d.decal >= set.decals.size()) continue;
            const vfx::DecalDesc& desc = set.decals[d.decal];
            const glm::mat4 toWorld = d.pose * glm::scale(glm::mat4(1.0f), glm::vec3(desc.size, desc.size, desc.depth));
            gpu::shared::DecalGpu g{};
            copyMatrix(g.decalToWorld, toWorld);
            copyMatrix(g.worldToDecal, glm::inverse(toWorld));
            g.tint = {desc.albedo.x, desc.albedo.y, desc.albedo.z, d.fade};
            g.params = {desc.roughness, desc.normal, 0.0f, 0.0f};
            uint32_t albedo = gpu::shared::kVfxNoTexture, normal = gpu::shared::kVfxNoTexture;
            if (desc.texture >= 0 && static_cast<std::size_t>(desc.texture) < frame.textures.size())
                albedo = frame.textures[static_cast<std::size_t>(desc.texture)];
            if (desc.texture >= 0 && static_cast<std::size_t>(desc.texture) < frame.normals.size())
                normal = frame.normals[static_cast<std::size_t>(desc.texture)];
            g.textures = {albedo, normal, 0u, 0u};
            table.push_back(g);
        }
    }
    upload(slot, table);
}

void DecalPass::upload(uint32_t slot, std::span<const gpu::shared::DecalGpu> decals) {
    if (slot >= m_slots.size() || m_device == VK_NULL_HANDLE) return;
    Slot& s = m_slots[slot];
    s.count = static_cast<uint32_t>(std::min<std::size_t>(decals.size(), kDecalCapacity));
    if (s.count == 0) return;
    const VkDeviceSize bytes = VkDeviceSize(s.count) * sizeof(gpu::shared::DecalGpu);
    if (s.bytes < bytes) {
        // The slot's fence was waited on: nothing reads the old buffer.
        rtc::destroyHostBuffer(m_device, s.decals);
        s.bytes = std::max<VkDeviceSize>(bytes, VkDeviceSize(sizeof(gpu::shared::DecalGpu)) * 16);
        if (!rtc::createHostBuffer(m_device, m_physical, s.bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, s.decals)) {
            s.bytes = 0;
            s.count = 0;
            return;
        }
        writeSet(slot, s.decals.buffer);
    }
    std::memcpy(s.decals.mapped, decals.data(), static_cast<std::size_t>(bytes));
}

void DecalPass::record(VkCommandBuffer cmd, uint32_t slot, const View& view, VkDescriptorSet textures) {
    if (slot >= m_slots.size() || !ready()) return;
    const Slot& s = m_slots[slot];
    if (s.count == 0 || textures == VK_NULL_HANDLE) return;

    VkRenderPassBeginInfo rb{};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = m_renderPass;
    rb.framebuffer = m_framebuffer;
    rb.renderArea.extent = {m_width, m_height};
    rb.clearValueCount = 0;   // loaded, never cleared
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {m_width, m_height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    const std::array<VkDescriptorSet, 2> sets{s.set, textures};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, static_cast<uint32_t>(sets.size()),
                            sets.data(), 0, nullptr);
    gpu::shared::DecalPush pc{};
    copyMatrix(pc.viewProj, view.viewProj);
    pc.camera = {view.camera.x, view.camera.y, view.camera.z, 0.0f};
    pc.screen = {static_cast<float>(m_width), static_cast<float>(m_height), view.clearance, 0.0f};
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, kDecalVertices, s.count, 0, 0);
    vkCmdEndRenderPass(cmd);
}

} // namespace ohao
