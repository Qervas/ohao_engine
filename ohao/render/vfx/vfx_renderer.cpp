#include "render/vfx/vfx_renderer.hpp"

#include "vfx/tracer_math.hpp"

#include "render/scene/render_scene.hpp"
#include "scene/asset/model.hpp"
#include "vfx/particle_math.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cstring>

namespace ohao {

namespace {

namespace shared = gpu::shared;

constexpr VkImageLayout kReadOnly = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
constexpr VkImageLayout kDepthReadOnly = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
constexpr VkFormat kMaskFormat = VK_FORMAT_R8_UNORM;
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
// At most this many particles of a batch are evaluated on the host for its distance (the mean
// of the living ones): the object sort's key, not a draw.
constexpr uint32_t kCentroidSamples = 8;

template <size_t N>
void copyMatrix(std::array<float, N>& out, const glm::mat4& m) {
    static_assert(N == 16);
    std::memcpy(out.data(), &m[0][0], sizeof(float) * 16);
}

uint32_t blendIndex(vfx::Blend b) {
    switch (b) {
    case vfx::Blend::Alpha: return shared::kVfxBlendAlpha;
    case vfx::Blend::Premultiplied: return shared::kVfxBlendPremultiplied;
    case vfx::Blend::Additive: break;
    }
    return shared::kVfxBlendAdditive;
}

// The GPU's copy of an emitter: vfx/expand.slang reads it as vfx/particle_math.hpp's evaluate()
// reads an EmitterDesc. Curves keep their first kVfxCurveKeys keys.
shared::VfxEmitterGpu packEmitter(const vfx::EmitterDesc& em, uint32_t texture, uint32_t cols, uint32_t rows) {
    shared::VfxEmitterGpu g{};
    g.lifeSpeed = {em.life.min, em.life.max, em.speed.min, em.speed.max};
    g.shape = {em.sizeStart, em.sizeEnd, em.spreadDeg, em.gravity};
    g.motion = {em.drag, em.offset, em.intensity, em.soft};
    const uint32_t mode = em.frames == vfx::FrameMode::Life ? 0u : em.frames == vfx::FrameMode::Random ? 1u : 2u;
    g.look = {blendIndex(em.blend), em.lit ? 1u : 0u, texture, mode | (em.frame << 8)};
    const uint32_t colorKeys = static_cast<uint32_t>(std::min<size_t>(em.color.keys.size(), shared::kVfxCurveKeys));
    const uint32_t alphaKeys = static_cast<uint32_t>(std::min<size_t>(em.alpha.keys.size(), shared::kVfxCurveKeys));
    g.atlas = {cols, rows, std::max(colorKeys, 1u), std::max(alphaKeys, 1u)};
    std::array<std::array<float, 4>*, 4> colors{&g.color0, &g.color1, &g.color2, &g.color3};
    for (uint32_t i = 0; i < shared::kVfxCurveKeys; ++i) {
        const uint32_t k = std::min(i, colorKeys > 0 ? colorKeys - 1 : 0u);
        if (colorKeys == 0) *colors[i] = {0.0f, 1.0f, 1.0f, 1.0f};
        else {
            const auto& key = em.color.keys[k];
            *colors[i] = {key.first, key.second.x, key.second.y, key.second.z};
        }
        const uint32_t a = std::min(i, alphaKeys > 0 ? alphaKeys - 1 : 0u);
        g.alphaT[i] = alphaKeys == 0 ? 0.0f : em.alpha.keys[a].first;
        g.alphaV[i] = alphaKeys == 0 ? 1.0f : em.alpha.keys[a].second;
    }
    return g;
}

VkPipelineColorBlendAttachmentState blendState(uint32_t mode) {
    VkPipelineColorBlendAttachmentState s{};
    s.blendEnable = VK_TRUE;
    s.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    s.colorBlendOp = VK_BLEND_OP_ADD;
    s.alphaBlendOp = VK_BLEND_OP_ADD;
    s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;   // the lit image's alpha is kept
    s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    switch (mode) {
    case shared::kVfxBlendAlpha:
        s.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case shared::kVfxBlendPremultiplied:
        s.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    default:   // additive: light added by coverage
        s.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    }
    return s;
}

VkPipelineColorBlendAttachmentState maskState() {
    VkPipelineColorBlendAttachmentState s{};
    s.blendEnable = VK_TRUE;
    s.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    s.colorBlendOp = VK_BLEND_OP_MAX;
    s.alphaBlendOp = VK_BLEND_OP_MAX;
    s.srcColorBlendFactor = s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    s.srcAlphaBlendFactor = s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    return s;
}

VkDescriptorSetLayout makeLayout(VkDevice device, std::span<const VkDescriptorSetLayoutBinding> bindings,
                                 int partiallyBound = -1) {
    std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    if (partiallyBound >= 0) flags[static_cast<size_t>(partiallyBound)] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagInfo{};
    flagInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagInfo.bindingCount = static_cast<uint32_t>(flags.size());
    flagInfo.pBindingFlags = flags.data();
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.pNext = &flagInfo;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) != VK_SUCCESS) return VK_NULL_HANDLE;
    return layout;
}

} // namespace

VfxRenderer::~VfxRenderer() { cleanup(); }

bool VfxRenderer::initialize(VkDevice device, VkPhysicalDevice physicalDevice, VkDescriptorSetLayout textures,
                             VkDescriptorSetLayout gbufferScene) {
    m_device = device;
    m_physical = physicalDevice;
    if (!m_openpbrTables.create(device, physicalDevice)) return false;

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;   // equirect u wraps
    si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device, &si, nullptr, &m_sampler) != VK_SUCCESS) return false;

    if (!rtc::createImage(device, physicalDevice, m_dummy, kHdrFormat,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, 1) ||
        !rtc::createHostBuffer(device, physicalDevice, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_dummyBuffer))
        return false;

    if (!createLayouts(textures, gbufferScene) || !createRenderPass() || !createPipelines()) {
        OHAO_LOG(Warn, render, "VfxRenderer: pipelines failed (no effects or transparent surfaces drawn)");
        cleanup();
        return false;
    }
    return true;
}

void VfxRenderer::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);
    destroySizeResources();
    for (auto& p : m_particlePipelines)
        if (p) vkDestroyPipeline(m_device, p, nullptr);
    for (auto& p : m_meshPipelines)
        if (p) vkDestroyPipeline(m_device, p, nullptr);
    m_particlePipelines.fill(VK_NULL_HANDLE);
    m_meshPipelines.fill(VK_NULL_HANDLE);
    for (VkPipeline* p : {&m_expandPipeline, &m_sortPipeline, &m_tracerPipeline}) {
        if (*p) vkDestroyPipeline(m_device, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    for (VkPipelineLayout* l :
         {&m_expandPipelineLayout, &m_sortPipelineLayout, &m_drawPipelineLayout, &m_meshPipelineLayout, &m_tracerPipelineLayout}) {
        if (*l) vkDestroyPipelineLayout(m_device, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
    if (m_renderPass) vkDestroyRenderPass(m_device, m_renderPass, nullptr);
    m_renderPass = VK_NULL_HANDLE;
    if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);   // frees the sets
    m_pool = VK_NULL_HANDLE;
    for (VkDescriptorSetLayout* l : {&m_expandLayout, &m_sortLayout, &m_drawLayout, &m_forwardLayout}) {
        if (*l) vkDestroyDescriptorSetLayout(m_device, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
    for (auto& s : m_slots) {
        rtc::destroyHostBuffer(m_device, s.emitters);
        rtc::destroyHostBuffer(m_device, s.batches);
        rtc::destroyHostBuffer(m_device, s.tracerVerts);
        s = Slot{};
    }
    rtc::destroyHostBuffer(m_device, m_particles);
    rtc::destroyHostBuffer(m_device, m_order);
    m_particleCapacity = 0;
    rtc::destroyHostBuffer(m_device, m_dummyBuffer);
    m_openpbrTables.destroy();
    rtc::destroyImage(m_device, m_dummy);
    if (m_sampler) vkDestroySampler(m_device, m_sampler, nullptr);
    m_sampler = VK_NULL_HANDLE;
    m_objects.clear();
    m_batches.clear();
    m_stats = {};
    m_device = VK_NULL_HANDLE;
}

void VfxRenderer::destroySizeResources() {
    if (m_framebuffer) vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
    m_framebuffer = VK_NULL_HANDLE;
    rtc::destroyImage(m_device, m_mask);
    rtc::destroyImage(m_device, m_refraction);
}

bool VfxRenderer::createLayouts(VkDescriptorSetLayout textures, VkDescriptorSetLayout gbufferScene) {
    constexpr VkDescriptorType kStorage = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    constexpr VkDescriptorType kSampled = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    constexpr VkDescriptorType kSampler = VK_DESCRIPTOR_TYPE_SAMPLER;
    constexpr VkDescriptorType kTlas = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    constexpr VkShaderStageFlags kDraw = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    constexpr VkShaderStageFlags kFrag = VK_SHADER_STAGE_FRAGMENT_BIT;

    const std::array<VkDescriptorSetLayoutBinding, 10> expand{{
        rtc::layoutBinding(0, kStorage), rtc::layoutBinding(1, kStorage), rtc::layoutBinding(2, kStorage),
        rtc::layoutBinding(3, kStorage), rtc::layoutBinding(4, kStorage), rtc::layoutBinding(5, kStorage),
        rtc::layoutBinding(6, kTlas), rtc::layoutBinding(7, kSampled), rtc::layoutBinding(8, kSampled),
        rtc::layoutBinding(9, kSampler),
    }};
    const std::array<VkDescriptorSetLayoutBinding, 2> sort{{rtc::layoutBinding(0, kStorage), rtc::layoutBinding(1, kStorage)}};
    const std::array<VkDescriptorSetLayoutBinding, 4> draw{{
        rtc::layoutBinding(0, kStorage, kDraw), rtc::layoutBinding(1, kStorage, kDraw),
        rtc::layoutBinding(2, kStorage, kDraw), rtc::layoutBinding(3, kSampled, kDraw),
    }};
    const std::array<VkDescriptorSetLayoutBinding, 12> forward{{
        rtc::layoutBinding(0, kStorage, kFrag), rtc::layoutBinding(1, kStorage, kFrag),
        rtc::layoutBinding(2, kSampled, kFrag), rtc::layoutBinding(3, kSampled, kFrag),
        rtc::layoutBinding(4, kSampler, kFrag), rtc::layoutBinding(5, kTlas, kFrag),
        rtc::layoutBinding(6, kSampled, kFrag), rtc::layoutBinding(7, kStorage, kFrag),   // 7: OpenPBR's tables
        rtc::layoutBinding(8, kSampled, kFrag),                                           // 8: the refraction source
        // 9-11: the scene's meshes, indices and vertices (glass finds its far side's normal)
        rtc::layoutBinding(9, kStorage, kFrag), rtc::layoutBinding(10, kStorage, kFrag),
        rtc::layoutBinding(11, kStorage, kFrag),
    }};
    m_expandLayout = makeLayout(m_device, expand, 6);
    m_sortLayout = makeLayout(m_device, sort);
    m_drawLayout = makeLayout(m_device, draw);
    m_forwardLayout = makeLayout(m_device, forward, 5);
    if (!m_expandLayout || !m_sortLayout || !m_drawLayout || !m_forwardLayout) return false;

    constexpr uint32_t n = MAX_FRAMES_IN_FLIGHT;
    const std::array<VkDescriptorPoolSize, 4> sizes{{
        {kStorage, 18 * n}, {kSampled, 7 * n}, {kSampler, 2 * n}, {kTlas, 2 * n},
    }};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 4 * n;
    pi.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pi.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(m_device, &pi, nullptr, &m_pool) != VK_SUCCESS) return false;
    for (auto& s : m_slots) {
        const std::array<VkDescriptorSetLayout, 4> layouts{m_expandLayout, m_sortLayout, m_drawLayout, m_forwardLayout};
        std::array<VkDescriptorSet, 4> sets{};
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_pool;
        ai.descriptorSetCount = static_cast<uint32_t>(layouts.size());
        ai.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(m_device, &ai, sets.data()) != VK_SUCCESS) return false;
        s.expandSet = sets[0];
        s.sortSet = sets[1];
        s.drawSet = sets[2];
        s.forwardSet = sets[3];
    }

    // Graphics pipeline layouts: particles (their set, then the bindless textures), blended meshes
    // (core/gbuffer.slang's: the textures, the G-buffer's scene set; then the forward set).
    const std::array<VkDescriptorSetLayout, 2> drawSets{m_drawLayout, textures};
    const VkPushConstantRange drawPush{kDraw, 0, sizeof(shared::VfxDrawPush)};
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = static_cast<uint32_t>(drawSets.size());
    li.pSetLayouts = drawSets.data();
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &drawPush;
    if (vkCreatePipelineLayout(m_device, &li, nullptr, &m_drawPipelineLayout) != VK_SUCCESS) return false;
    const std::array<VkDescriptorSetLayout, 3> meshSets{textures, gbufferScene, m_forwardLayout};
    const VkPushConstantRange meshPush{kFrag, 0, sizeof(shared::ForwardPush)};
    li.setLayoutCount = static_cast<uint32_t>(meshSets.size());
    li.pSetLayouts = meshSets.data();
    li.pPushConstantRanges = &meshPush;
    return vkCreatePipelineLayout(m_device, &li, nullptr, &m_meshPipelineLayout) == VK_SUCCESS;
}

bool VfxRenderer::createRenderPass() {
    // 0: the lit image, loaded and kept (SSR and SSS leave it SHADER_READ_ONLY, bloom reads it so);
    // 1: the coverage mask, cleared; 2: the G-buffer's depth, tested and sampled, never written.
    std::array<VkAttachmentDescription, 3> a{};
    a[0].format = kHdrFormat;
    a[0].samples = VK_SAMPLE_COUNT_1_BIT;
    a[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[0].initialLayout = kReadOnly;
    a[0].finalLayout = kReadOnly;
    a[1] = a[0];
    a[1].format = kMaskFormat;
    a[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a[2] = a[0];
    a[2].format = kDepthFormat;
    a[2].initialLayout = kDepthReadOnly;
    a[2].finalLayout = kDepthReadOnly;   // stored: TAA and the next readers keep the depth

    const std::array<VkAttachmentReference, 2> colors{{{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                                       {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}}};
    const VkAttachmentReference depth{2, kDepthReadOnly};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = static_cast<uint32_t>(colors.size());
    sub.pColorAttachments = colors.data();
    sub.pDepthStencilAttachment = &depth;

    // In: the lit image's writers (lighting and sky as attachments, SSS and SSR in compute), the
    // particles vfx.expand and vfx.sort wrote, and last frame's TAA read of the mask. Out: bloom,
    // TAA and the tonemapper read both images.
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
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

bool VfxRenderer::createPipelines() {
    if (!rtc::createComputePipeline(m_device, "vfx_expand.slang.spv", "expand", m_expandLayout,
                                    sizeof(shared::VfxExpandPush), m_expandPipelineLayout, m_expandPipeline) ||
        !rtc::createComputePipeline(m_device, "vfx_sort.slang.spv", "sort", m_sortLayout, sizeof(shared::VfxSortPush),
                                    m_sortPipelineLayout, m_sortPipeline))
        return false;

    const VkShaderModule particleVert = rtc::loadShaderModule(m_device, "vfx_particle.slang.vertexMain.spv");
    const VkShaderModule particleFrag = rtc::loadShaderModule(m_device, "vfx_particle.slang.fragmentMain.spv");
    const VkShaderModule meshVert = rtc::loadShaderModule(m_device, "core_gbuffer.slang.vertexMain.spv");
    const VkShaderModule meshFrag = rtc::loadShaderModule(m_device, "vfx_transparent_mesh.slang.spv");
    const VkShaderModule tracerVert = rtc::loadShaderModule(m_device, "vfx_tracer.slang.vertexMain.spv");
    const VkShaderModule tracerFrag = rtc::loadShaderModule(m_device, "vfx_tracer.slang.fragmentMain.spv");
    bool ok = particleVert && particleFrag && meshVert && meshFrag && tracerVert && tracerFrag;

    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& s : stages) s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkPipelineVertexInputStateCreateInfo noVertices{};
    noVertices.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    std::array<VkPipelineColorBlendAttachmentState, 2> blends{};
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = static_cast<uint32_t>(blends.size());
    cb.pAttachments = blends.data();
    const std::array<VkDynamicState, 2> dyn{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = static_cast<uint32_t>(dyn.size());
    dy.pDynamicStates = dyn.data();

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.renderPass = m_renderPass;

    // Particles: one pipeline a blend mode, both faces (a quad faces the camera).
    if (ok) {
        stages[0].module = particleVert;
        stages[1].module = particleFrag;
        stages[0].pName = "vertexMain";
        stages[1].pName = "fragmentMain";
        pi.pVertexInputState = &noVertices;
        pi.layout = m_drawPipelineLayout;
        blends[1] = maskState();
        for (uint32_t mode = 0; mode < 3 && ok; ++mode) {
            blends[0] = blendState(mode);
            ok = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pi, nullptr, &m_particlePipelines[mode]) == VK_SUCCESS;
        }
    }
    // Blended meshes: core/gbuffer.slang's vertex input, straight alpha by coverage; [0] one-sided
    // (back faces culled, as the G-buffer culls them), [1] double-sided.
    if (ok) {
        const auto bindingDescs = Vertex::getBindingDescriptions();
        const auto attributeDescs = Vertex::getAttributeDescriptions();
        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDescs.size());
        vi.pVertexBindingDescriptions = bindingDescs.data();
        vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescs.size());
        vi.pVertexAttributeDescriptions = attributeDescs.data();
        stages[0].module = meshVert;
        stages[0].pName = "vertexMain";
        stages[1].module = meshFrag;
        stages[1].pName = "fragmentMain";
        pi.pVertexInputState = &vi;
        pi.layout = m_meshPipelineLayout;
        blends[0] = blendState(shared::kVfxBlendAlpha);
        for (uint32_t k = 0; k < 2 && ok; ++k) {
            rs.cullMode = k == 0 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
            ok = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pi, nullptr, &m_meshPipelines[k]) == VK_SUCCESS;
        }
    }
    // Tracers: six host-built vertices a tracer (position, across and along, colour), additive light,
    // the particles' depth test, both faces (a quad faces the camera).
    if (ok) {
        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(shared::TracerPush)};
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        ok = vkCreatePipelineLayout(m_device, &pl, nullptr, &m_tracerPipelineLayout) == VK_SUCCESS;
    }
    if (ok) {
        const VkVertexInputBindingDescription binding{0, sizeof(shared::TracerVertexGpu), VK_VERTEX_INPUT_RATE_VERTEX};
        std::array<VkVertexInputAttributeDescription, 3> attributes{};
        for (uint32_t k = 0; k < attributes.size(); ++k)
            attributes[k] = {k, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<uint32_t>(k * sizeof(glm::vec4))};
        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vi.pVertexAttributeDescriptions = attributes.data();
        stages[0].module = tracerVert;
        stages[0].pName = "vertexMain";
        stages[1].module = tracerFrag;
        stages[1].pName = "fragmentMain";
        pi.pVertexInputState = &vi;
        pi.layout = m_tracerPipelineLayout;
        blends[0] = blendState(shared::kVfxBlendAdditive);
        blends[1] = maskState();
        rs.cullMode = VK_CULL_MODE_NONE;
        ok = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pi, nullptr, &m_tracerPipeline) == VK_SUCCESS;
    }
    for (VkShaderModule m : {particleVert, particleFrag, meshVert, meshFrag, tracerVert, tracerFrag})
        if (m) vkDestroyShaderModule(m_device, m, nullptr);
    return ok;
}

bool VfxRenderer::resize(uint32_t width, uint32_t height, VkImageView hdr, VkImageView depth, VkImage hdrImage) {
    if (m_renderPass == VK_NULL_HANDLE) return false;
    destroySizeResources();
    m_width = std::max(width, 1u);
    m_height = std::max(height, 1u);
    m_hdrView = hdr;
    m_depthView = depth;
    m_hdrImage = hdrImage;
    if (!rtc::createImage(m_device, m_physical, m_mask, kMaskFormat,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, m_width, m_height) ||
        !rtc::createImage(m_device, m_physical, m_refraction, kHdrFormat,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, m_width, m_height))
        return false;
    const std::array<VkImageView, 3> views{hdr, m_mask.view, depth};
    VkFramebufferCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = m_renderPass;
    fi.attachmentCount = static_cast<uint32_t>(views.size());
    fi.pAttachments = views.data();
    fi.width = m_width;
    fi.height = m_height;
    fi.layers = 1;
    return vkCreateFramebuffer(m_device, &fi, nullptr, &m_framebuffer) == VK_SUCCESS;
}

bool VfxRenderer::ensureHostBuffer(rtc::HostBuffer& buf, VkDeviceSize& cap, VkDeviceSize bytes, VkBufferUsageFlags usage) {
    if (buf.buffer != VK_NULL_HANDLE && cap >= bytes) return true;
    rtc::destroyHostBuffer(m_device, buf);
    cap = std::max<VkDeviceSize>(bytes, 4096);
    return rtc::createHostBuffer(m_device, m_physical, cap, usage, buf);
}

void VfxRenderer::prepare(uint32_t slot, const render::RenderScene& scene, const View& view) {
    m_view = view;
    m_batches.clear();
    m_objects.clear();
    m_meshDraws.clear();
    m_emitterTable.clear();
    m_emitterBase.clear();
    m_tracerVerts.clear();
    m_stats = {};
    if (m_renderPass == VK_NULL_HANDLE || m_framebuffer == VK_NULL_HANDLE || slot >= m_slots.size()) return;

    // The effect records: one batch an emitter of a record, in record order, its particles a
    // range of slots. A batch whose particles are all unborn or dead is skipped; past the set's
    // particle cap the rest are dropped (counted).
    const render::VfxFrame& frame = scene.vfx();
    if (frame.set && m_particlesEnabled) {
        const vfx::VfxSet& set = *frame.set;
        for (const vfx::EffectDesc& effect : set.effects) {
            m_emitterBase.push_back(static_cast<uint32_t>(m_emitterTable.size()));
            for (const vfx::EmitterDesc& em : effect.emitters) {
                uint32_t texture = shared::kVfxNoTexture, cols = 1, rows = 1;
                if (em.texture >= 0 && static_cast<size_t>(em.texture) < set.textures.size()) {
                    const auto& t = set.textures[static_cast<size_t>(em.texture)];
                    cols = std::max(t.cols, 1u);
                    rows = std::max(t.rows, 1u);
                    if (static_cast<size_t>(em.texture) < frame.textures.size()) texture = frame.textures[static_cast<size_t>(em.texture)];
                }
                shared::VfxEmitterGpu g = packEmitter(em, texture, cols, rows);
                if (!m_softEnabled) g.motion[3] = 0.0f;   // render.settings vfx.soft off: the shader's hard edge (motion.w > 0 only)
                m_emitterTable.push_back(g);
            }
        }
        const uint32_t cap = std::max(set.caps.particles, 1u);
        if (cap > m_particleCapacity) {
            vkDeviceWaitIdle(m_device);   // the shared slots are re-made (a set's cap grew: rare)
            rtc::destroyHostBuffer(m_device, m_particles);
            rtc::destroyHostBuffer(m_device, m_order);
            m_particleCapacity = 0;
            if (rtc::createHostBuffer(m_device, m_physical, VkDeviceSize(cap) * sizeof(shared::VfxParticleGpu),
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_particles) &&
                rtc::createHostBuffer(m_device, m_physical, VkDeviceSize(cap) * sizeof(uint32_t),
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_order))
                m_particleCapacity = cap;
        }
        uint32_t total = 0;
        for (const render::VfxFrameEffect& rec : frame.effects) {
            if (rec.effect >= set.effects.size()) continue;
            const vfx::EffectDesc& effect = set.effects[rec.effect];
            for (uint32_t k = 0; k < effect.emitters.size(); ++k) {
                const vfx::EmitterDesc& em = effect.emitters[k];
                if (em.count == 0 || rec.age < 0.0f || rec.age >= em.life.max) continue;
                // The batch's distance: the mean of its first living particles, as the GPU places them.
                glm::vec3 sum(0.0f);
                uint32_t alive = 0;
                for (uint32_t i = 0; i < em.count && i < kCentroidSamples; ++i) {
                    const vfx::ParticleState p = vfx::evaluate(em, rec.seed, i, rec.age, rec.pose);
                    if (!p.alive) continue;
                    sum += p.position;
                    ++alive;
                }
                if (alive == 0 && em.count <= kCentroidSamples) continue;
                if (total + em.count > m_particleCapacity) {
                    m_stats.dropped += em.count;
                    continue;
                }
                const glm::vec3 centre = alive > 0 ? sum / static_cast<float>(alive) : glm::vec3(rec.pose[3]);
                const bool sorted = em.count <= shared::kVfxSortMax &&
                                    (em.sort == vfx::SortMode::Particles ||
                                     (em.sort == vfx::SortMode::Auto && em.blend != vfx::Blend::Additive));
                Batch b{};
                b.emitter = m_emitterBase[rec.effect] + k;
                b.seed = rec.seed;
                b.first = total;
                b.count = em.count;
                b.pose = rec.pose;
                b.age = rec.age;
                b.blend = blendIndex(em.blend);
                b.sorted = sorted;
                m_objects.push_back({0u, rec.id * 64u + k, glm::distance(centre, view.cameraPos),
                                     static_cast<uint32_t>(m_batches.size())});
                m_batches.push_back(b);
                total += em.count;
                m_stats.sortedBatches += sorted ? 1u : 0u;
                if (sorted) m_stats.particlesSortedMax = std::max(m_stats.particlesSortedMax, em.count);
            }
        }
        m_stats.particles = total;
        m_stats.batches = static_cast<uint32_t>(m_batches.size());
    }

    // The tracers: a stretched camera-facing quad from the tail to the head, the head at speed * age
    // along the round (clamped at its end), the tail `length` behind it. Six vertices each; a tracer
    // not yet fired (head at its tail) is skipped. Objects of kind 2, index = the tracer's quad.
    m_tracerVerts.clear();
    static const std::vector<render::VfxFrameTracer> kNoTracers;
    for (const render::VfxFrameTracer& t : m_tracersEnabled ? frame.tracers : kNoTracers) {
        const vfx::TracerQuad q = vfx::tracerQuad(t.from, t.to, t.speed, t.length, t.width, t.age, view.cameraPos);
        if (!q.visible) continue;
        for (const vfx::TracerQuad::Corner& c : q.corners) {
            shared::TracerVertexGpu v{};
            v.pos = {c.pos.x, c.pos.y, c.pos.z, 0.0f};
            v.uv = {c.across, c.along, 0.0f, 0.0f};
            v.color = {t.color.x, t.color.y, t.color.z, 1.0f};
            m_tracerVerts.push_back(v);
        }
        m_objects.push_back({2u, t.id, glm::distance(q.centre, view.cameraPos), static_cast<uint32_t>(m_tracerVerts.size() / 6 - 1)});
        ++m_stats.tracers;
    }

    // The blended draws, each at its world bounds' centre.
    const auto draws = scene.draws();
    for (const uint32_t i : scene.transparentDraws()) {
        if (i >= draws.size()) continue;
        const auto& d = draws[i];
        const glm::vec3 local = 0.5f * (d.aabbMin + d.aabbMax);
        const glm::vec3 centre = d.world ? glm::vec3(*d.world * glm::vec4(local, 1.0f)) : local;
        m_objects.push_back({1u, i, glm::distance(centre, view.cameraPos), static_cast<uint32_t>(m_meshDraws.size())});
        m_meshDraws.push_back(i);
    }
    m_stats.meshes = static_cast<uint32_t>(m_meshDraws.size());

    // Back to front; equal distances by kind, then id: the same order every run.
    std::sort(m_objects.begin(), m_objects.end(), [](const Object& a, const Object& b) {
        if (a.distance != b.distance) return a.distance > b.distance;
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.id < b.id;
    });
    m_stats.objects = static_cast<uint32_t>(m_objects.size());

    // This slot's tables (its fence was waited on).
    Slot& s = m_slots[slot];
    if (m_stats.particles > 0) {
        const VkDeviceSize emitterBytes = m_emitterTable.size() * sizeof(shared::VfxEmitterGpu);
        const VkDeviceSize batchBytes = m_batches.size() * sizeof(shared::VfxBatchGpu);
        if (!ensureHostBuffer(s.emitters, s.emitterBytes, emitterBytes) ||
            !ensureHostBuffer(s.batches, s.batchBytes, batchBytes)) {
            m_stats.particles = 0;
            m_objects.erase(std::remove_if(m_objects.begin(), m_objects.end(), [](const Object& o) { return o.kind == 0; }),
                            m_objects.end());
            m_batches.clear();
            return;
        }
        std::memcpy(s.emitters.mapped, m_emitterTable.data(), static_cast<size_t>(emitterBytes));
        auto* out = static_cast<shared::VfxBatchGpu*>(s.batches.mapped);
        for (size_t i = 0; i < m_batches.size(); ++i) {
            const Batch& b = m_batches[i];
            shared::VfxBatchGpu g{};
            copyMatrix(g.pose, b.pose);
            g.info = {b.emitter, b.seed, b.first, b.count};
            g.timing = {b.age, 0.0f, 0.0f, 0.0f};
            out[i] = g;
        }
    }
    // The tracers' vertices, in this slot's buffer; without one, no tracer is drawn.
    if (!m_tracerVerts.empty()) {
        const VkDeviceSize bytes = m_tracerVerts.size() * sizeof(shared::TracerVertexGpu);
        if (ensureHostBuffer(s.tracerVerts, s.tracerBytes, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
            std::memcpy(s.tracerVerts.mapped, m_tracerVerts.data(), static_cast<size_t>(bytes));
        } else {
            m_tracerVerts.clear();
            m_stats.tracers = 0;
            m_objects.erase(std::remove_if(m_objects.begin(), m_objects.end(), [](const Object& o) { return o.kind == 2; }),
                            m_objects.end());
            m_stats.objects = static_cast<uint32_t>(m_objects.size());
        }
    }
}

void VfxRenderer::writeExpandSet(uint32_t slot, const Lighting& lighting) {
    Slot& s = m_slots[slot];
    const VkBuffer dummy = m_dummyBuffer.buffer;
    const std::array<VkDescriptorBufferInfo, 6> buffers{{
        {s.emitters.buffer, 0, VK_WHOLE_SIZE},
        {s.batches.buffer, 0, VK_WHOLE_SIZE},
        {m_particles.buffer, 0, VK_WHOLE_SIZE},
        {m_order.buffer, 0, VK_WHOLE_SIZE},
        {lighting.lights ? lighting.lights : dummy, 0, VK_WHOLE_SIZE},
        {lighting.clusters ? lighting.clusters : dummy, 0, VK_WHOLE_SIZE},
    }};
    const std::array<VkDescriptorImageInfo, 2> images{{
        {VK_NULL_HANDLE, lighting.giRadiance ? lighting.giRadiance : m_dummy.view, kReadOnly},
        {VK_NULL_HANDLE, lighting.envIrradiance ? lighting.envIrradiance : m_dummy.view, kReadOnly},
    }};
    const VkDescriptorImageInfo sampler{m_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSetAccelerationStructureKHR as{};
    as.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    as.accelerationStructureCount = 1;
    as.pAccelerationStructures = &lighting.tlas;

    std::vector<VkWriteDescriptorSet> w;
    for (uint32_t i = 0; i < buffers.size(); ++i) {
        w.push_back(rtc::descriptorWrite(s.expandSet, i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER));
        w.back().pBufferInfo = &buffers[i];
    }
    if (lighting.tlas != VK_NULL_HANDLE) {
        w.push_back(rtc::descriptorWrite(s.expandSet, 6, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR));
        w.back().pNext = &as;
    }
    for (uint32_t i = 0; i < images.size(); ++i) {
        w.push_back(rtc::descriptorWrite(s.expandSet, 7 + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE));
        w.back().pImageInfo = &images[i];
    }
    w.push_back(rtc::descriptorWrite(s.expandSet, 9, VK_DESCRIPTOR_TYPE_SAMPLER));
    w.back().pImageInfo = &sampler;
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
}

void VfxRenderer::writeForwardSet(uint32_t slot, const Lighting& lighting, const render::RenderScene& scene) {
    Slot& s = m_slots[slot];
    const VkBuffer dummy = m_dummyBuffer.buffer;
    const std::array<VkDescriptorBufferInfo, 3> geometry{{
        {scene.meshTable() ? scene.meshTable() : dummy, 0, VK_WHOLE_SIZE},
        {scene.indexArena() ? scene.indexArena() : dummy, 0, VK_WHOLE_SIZE},
        {scene.vertexArena() ? scene.vertexArena() : dummy, 0, VK_WHOLE_SIZE},
    }};
    const std::array<VkDescriptorBufferInfo, 2> buffers{{
        {lighting.lights ? lighting.lights : dummy, 0, VK_WHOLE_SIZE},
        {lighting.clusters ? lighting.clusters : dummy, 0, VK_WHOLE_SIZE},
    }};
    const VkDescriptorBufferInfo tables{m_openpbrTables.buffer(), 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo irradiance{VK_NULL_HANDLE, lighting.envIrradiance ? lighting.envIrradiance : m_dummy.view,
                                           kReadOnly};
    const VkDescriptorImageInfo prefiltered{VK_NULL_HANDLE,
                                            lighting.envPrefiltered ? lighting.envPrefiltered : m_dummy.view, kReadOnly};
    const VkDescriptorImageInfo sampler{m_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    const VkDescriptorImageInfo gi{VK_NULL_HANDLE, lighting.giRadiance ? lighting.giRadiance : m_dummy.view, kReadOnly};
    const VkDescriptorImageInfo refraction{VK_NULL_HANDLE,
                                           m_refraction.layout == kReadOnly ? m_refraction.view : m_dummy.view, kReadOnly};
    VkWriteDescriptorSetAccelerationStructureKHR as{};
    as.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    as.accelerationStructureCount = 1;
    as.pAccelerationStructures = &lighting.tlas;

    std::vector<VkWriteDescriptorSet> w;
    for (uint32_t i = 0; i < buffers.size(); ++i) {
        w.push_back(rtc::descriptorWrite(s.forwardSet, i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER));
        w.back().pBufferInfo = &buffers[i];
    }
    w.push_back(rtc::descriptorWrite(s.forwardSet, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE));
    w.back().pImageInfo = &irradiance;
    w.push_back(rtc::descriptorWrite(s.forwardSet, 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE));
    w.back().pImageInfo = &prefiltered;
    w.push_back(rtc::descriptorWrite(s.forwardSet, 4, VK_DESCRIPTOR_TYPE_SAMPLER));
    w.back().pImageInfo = &sampler;
    if (lighting.tlas != VK_NULL_HANDLE) {
        w.push_back(rtc::descriptorWrite(s.forwardSet, 5, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR));
        w.back().pNext = &as;
    }
    w.push_back(rtc::descriptorWrite(s.forwardSet, 6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE));
    w.back().pImageInfo = &gi;
    w.push_back(rtc::descriptorWrite(s.forwardSet, 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER));
    w.back().pBufferInfo = &tables;
    w.push_back(rtc::descriptorWrite(s.forwardSet, 8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE));
    w.back().pImageInfo = &refraction;
    for (uint32_t i = 0; i < geometry.size(); ++i) {
        w.push_back(rtc::descriptorWrite(s.forwardSet, 9 + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER));
        w.back().pBufferInfo = &geometry[i];
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
}

namespace {

// The flags vfx/expand.slang and vfx/transparent_mesh.slang read (config.w / config.y).
uint32_t lightingFlags(const VfxRenderer::Lighting& l) {
    return (l.clusters ? 1u : 0u) | (l.giRadiance ? 2u : 0u) | (l.envIrradiance && l.envPrefiltered ? 4u : 0u) |
           (l.tlas ? 8u : 0u);
}

void zeroDummy(VkCommandBuffer cmd, rtc::Image& dummy) {
    if (dummy.layout == kReadOnly) return;
    rtc::transition(cmd, dummy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, dummy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    rtc::transition(cmd, dummy, kReadOnly, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                    false);
}

} // namespace

void VfxRenderer::expand(VkCommandBuffer cmd, uint32_t slot, const Lighting& lighting) {
    if (m_stats.particles == 0 || slot >= m_slots.size()) return;
    zeroDummy(cmd, m_dummy);
    writeExpandSet(slot, lighting);

    // In: the light clusters and RT GI (compute writes), and the previous frame's draws of the
    // shared particle slots (vertex and fragment reads, which this dispatch overwrites).
    VkMemoryBarrier in{};
    in.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    in.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    in.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &in, 0, nullptr, 0, nullptr);

    const glm::mat4 viewProj = m_view.proj * m_view.view;
    const uint32_t flags = lightingFlags(lighting);
    shared::VfxExpandPush pc{};
    copyMatrix(pc.viewProj, viewProj);
    pc.viewZ = {m_view.view[0][2], m_view.view[1][2], m_view.view[2][2], m_view.view[3][2]};
    pc.camera = {m_view.cameraPos.x, m_view.cameraPos.y, m_view.cameraPos.z,
                 (flags & 4u) != 0u ? lighting.envIntensity : 0.0f};
    pc.screen = {static_cast<float>(m_width), static_cast<float>(m_height), lighting.sliceScale, lighting.sliceBias};
    pc.config = {m_stats.particles, m_stats.batches, lighting.lightCount, flags};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_expandPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_expandPipelineLayout, 0, 1, &m_slots[slot].expandSet, 0,
                            nullptr);
    vkCmdPushConstants(cmd, m_expandPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (m_stats.particles + shared::kVfxGroupSize - 1) / shared::kVfxGroupSize, 1, 1);

    // Out: vfx.sort and the draws read the particles and the order.
    VkMemoryBarrier out{};
    out.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    out.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    out.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 1, &out, 0, nullptr, 0, nullptr);
}

void VfxRenderer::sort(VkCommandBuffer cmd, uint32_t slot) {
    if (m_stats.sortedBatches == 0 || slot >= m_slots.size()) return;
    Slot& s = m_slots[slot];
    const std::array<VkDescriptorBufferInfo, 2> buffers{{{m_particles.buffer, 0, VK_WHOLE_SIZE},
                                                         {m_order.buffer, 0, VK_WHOLE_SIZE}}};
    std::array<VkWriteDescriptorSet, 2> w{rtc::descriptorWrite(s.sortSet, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                                          rtc::descriptorWrite(s.sortSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)};
    w[0].pBufferInfo = &buffers[0];
    w[1].pBufferInfo = &buffers[1];
    vkUpdateDescriptorSets(m_device, 2, w.data(), 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sortPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_sortPipelineLayout, 0, 1, &s.sortSet, 0, nullptr);
    // Each sorted batch is its own range of the order: one workgroup each, no overlap.
    for (const Batch& b : m_batches) {
        if (!b.sorted) continue;
        shared::VfxSortPush pc{};
        pc.range = {b.first, b.count, 0u, 0u};
        vkCmdPushConstants(cmd, m_sortPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, 1, 1, 1);
    }
    VkMemoryBarrier out{};
    out.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    out.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    out.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &out, 0,
                         nullptr, 0, nullptr);
}

// The lit frame as it is before the transparent pass, into m_refraction: what glass refracts. The
// lit image waits between its writers (lighting, SSS) and this pass in the read-only layout.
void VfxRenderer::copyRefraction(VkCommandBuffer cmd) {
    if (m_hdrImage == VK_NULL_HANDLE || m_refraction.image == VK_NULL_HANDLE) return;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout = kReadOnly;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = m_hdrImage;
    toSrc.subresourceRange = range;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
    rtc::transition(cmd, m_refraction, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {m_width, m_height, 1};
    vkCmdCopyImage(cmd, m_hdrImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_refraction.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier back = toSrc;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = kReadOnly;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &back);
    rtc::transition(cmd, m_refraction, kReadOnly, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
}

void VfxRenderer::drawTransparent(VkCommandBuffer cmd, uint32_t slot, const render::RenderScene& scene,
                                  const Lighting& lighting, VkDescriptorSet textures, VkDescriptorSet gbufferScene) {
    if (m_objects.empty() || slot >= m_slots.size() || m_framebuffer == VK_NULL_HANDLE) return;
    Slot& s = m_slots[slot];
    zeroDummy(cmd, m_dummy);
    const bool meshes = !m_meshDraws.empty() && gbufferScene != VK_NULL_HANDLE && scene.vertexArena() != VK_NULL_HANDLE &&
                        scene.indexArena() != VK_NULL_HANDLE;
    if (m_stats.particles > 0) {
        const std::array<VkDescriptorBufferInfo, 3> buffers{{{m_particles.buffer, 0, VK_WHOLE_SIZE},
                                                             {m_order.buffer, 0, VK_WHOLE_SIZE},
                                                             {s.emitters.buffer, 0, VK_WHOLE_SIZE}}};
        const VkDescriptorImageInfo depth{VK_NULL_HANDLE, m_depthView, kDepthReadOnly};
        std::array<VkWriteDescriptorSet, 4> w{};
        for (uint32_t i = 0; i < 3; ++i) {
            w[i] = rtc::descriptorWrite(s.drawSet, i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            w[i].pBufferInfo = &buffers[i];
        }
        w[3] = rtc::descriptorWrite(s.drawSet, 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        w[3].pImageInfo = &depth;
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
    }
    if (meshes) {
        copyRefraction(cmd);
        writeForwardSet(slot, lighting, scene);
    }

    std::array<VkClearValue, 3> clears{};
    VkRenderPassBeginInfo rb{};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = m_renderPass;
    rb.framebuffer = m_framebuffer;
    rb.renderArea.extent = {m_width, m_height};
    rb.clearValueCount = static_cast<uint32_t>(clears.size());
    rb.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {m_width, m_height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const glm::mat4 viewProj = m_view.proj * m_view.view;
    const glm::mat4 invView = glm::inverse(m_view.view);
    shared::VfxDrawPush dp{};
    copyMatrix(dp.viewProj, viewProj);
    dp.right = {invView[0][0], invView[0][1], invView[0][2], m_view.proj[2][2]};
    dp.up = {invView[1][0], invView[1][1], invView[1][2], m_view.proj[3][2]};
    dp.screen = {static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 0.0f};

    const uint32_t flags = lightingFlags(lighting);
    shared::ForwardPush fp{};
    copyMatrix(fp.view, m_view.view);
    fp.camera = {m_view.cameraPos.x, m_view.cameraPos.y, m_view.cameraPos.z, 0.0f};
    fp.screen = {static_cast<float>(m_width), static_cast<float>(m_height), lighting.sliceScale, lighting.sliceBias};
    fp.config = {lighting.lightCount, flags, 0u, 0u};
    fp.env = {lighting.envIntensity, 0.0f, 0.0f, 0.0f};
    copyMatrix(fp.proj, m_view.proj);

    const auto draws = scene.draws();
    enum class Bound { None, Particles, Meshes, Tracers } bound = Bound::None;
    VkPipeline pipeline = VK_NULL_HANDLE;
    for (const Object& o : m_objects) {
        if (o.kind == 0) {
            const Batch& b = m_batches[o.index];
            if (bound != Bound::Particles) {
                const std::array<VkDescriptorSet, 2> sets{s.drawSet, textures};
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_drawPipelineLayout, 0,
                                        static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
                bound = Bound::Particles;
            }
            const VkPipeline p = m_particlePipelines[b.blend];
            if (p != pipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                pipeline = p;
            }
            dp.range = {b.first, b.emitter, b.sorted ? 1u : 0u, 0u};
            vkCmdPushConstants(cmd, m_drawPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(dp), &dp);
            vkCmdDraw(cmd, 6, b.count, 0, 0);
        } else if (meshes) {
            const uint32_t i = m_meshDraws[o.index];
            const auto& d = draws[i];
            if (bound != Bound::Meshes) {
                const std::array<VkDescriptorSet, 3> sets{textures, gbufferScene, s.forwardSet};
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_meshPipelineLayout, 0,
                                        static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
                vkCmdPushConstants(cmd, m_meshPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(fp), &fp);
                const VkBuffer vertices = scene.vertexArena();
                const VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &vertices, &offset);
                vkCmdBindIndexBuffer(cmd, scene.indexArena(), 0, VK_INDEX_TYPE_UINT32);
                bound = Bound::Meshes;
            }
            const VkPipeline p = m_meshPipelines[d.oneSided ? 0 : 1];
            if (p != pipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                pipeline = p;
            }
            // firstInstance is the draw's index: core/gbuffer.slang reads its record.
            vkCmdDrawIndexed(cmd, d.indexCount, 1, d.firstIndex, d.vertexOffset, i);
        } else if (o.kind == 2) {
            if (bound != Bound::Tracers) {
                const VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &s.tracerVerts.buffer, &offset);
                bound = Bound::Tracers;
            }
            if (m_tracerPipeline != pipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tracerPipeline);
                pipeline = m_tracerPipeline;
            }
            shared::TracerPush tp{};
            copyMatrix(tp.viewProj, viewProj);
            vkCmdPushConstants(cmd, m_tracerPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(tp), &tp);
            vkCmdDraw(cmd, 6, 1, 6u * o.index, 0);
        }
    }
    vkCmdEndRenderPass(cmd);
}

std::vector<shared::VfxParticleGpu> VfxRenderer::readParticles() {
    std::vector<shared::VfxParticleGpu> out;
    if (m_device == VK_NULL_HANDLE || m_particles.mapped == nullptr) return out;
    vkDeviceWaitIdle(m_device);
    const auto* p = static_cast<const shared::VfxParticleGpu*>(m_particles.mapped);
    out.assign(p, p + m_stats.particles);
    return out;
}

} // namespace ohao
