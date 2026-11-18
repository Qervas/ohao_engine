#include "render/vfx/vfx_renderer.hpp"
#include "render/scene/vertex_input.hpp"
#include "render/shader_bindings/vfx_expand.hpp"
#include "render/shader_bindings/vfx_sort.hpp"
#include "render/shader_bindings/vfx_particle.hpp"
#include "render/shader_bindings/vfx_transparent_mesh.hpp"

#include "vfx/effects/tracer_math.hpp"

#include "render/deferred/gbuffer_pass.hpp"
#include "render/graph/pipeline.hpp"
#include "render/rt/rt_compute.hpp"
#include "render/scene/render_scene.hpp"
#include "asset/vertex.hpp"
#include "vfx/effects/particle_math.hpp"
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

render::Blend blendState(uint32_t mode) {
    render::Blend s{};
    s.enable = true;
    s.writeMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    s.srcAlpha = VK_BLEND_FACTOR_ZERO;   // the lit image's alpha is kept
    s.dstAlpha = VK_BLEND_FACTOR_ONE;
    switch (mode) {
    case shared::kVfxBlendAlpha:
        s.srcColor = VK_BLEND_FACTOR_SRC_ALPHA;
        s.dstColor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case shared::kVfxBlendPremultiplied:
        s.srcColor = VK_BLEND_FACTOR_ONE;
        s.dstColor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    default:   // additive: light added by coverage
        s.srcColor = VK_BLEND_FACTOR_SRC_ALPHA;
        s.dstColor = VK_BLEND_FACTOR_ONE;
        break;
    }
    return s;
}

render::Blend maskState() {
    render::Blend s{};
    s.enable = true;
    s.writeMask = VK_COLOR_COMPONENT_R_BIT;
    s.colorOp = VK_BLEND_OP_MAX;
    s.alphaOp = VK_BLEND_OP_MAX;
    s.srcColor = s.dstColor = VK_BLEND_FACTOR_ONE;
    s.srcAlpha = s.dstAlpha = VK_BLEND_FACTOR_ONE;
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

bool VfxRenderer::initialize(VkDevice device, VkPhysicalDevice physicalDevice, GpuAllocator& allocator,
                             VkDescriptorSetLayout textures, VkDescriptorSetLayout gbufferScene) {
    m_device = device;
    m_allocator = &allocator;
    if (!m_openpbrTables.create(allocator)) return false;

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;   // equirect u wraps
    si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device, &si, nullptr, &m_sampler) != VK_SUCCESS) return false;

    m_dummy = allocator.createImage(
        {"vfx.dummy", kHdrFormat, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT});
    m_dummyBuffer = rtc::createHostBuffer(allocator, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!m_dummy || !m_dummyBuffer.isValid()) return false;

    if (!createLayouts(textures, gbufferScene) || !createPipelines()) {
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
    if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);   // frees the sets
    m_pool = VK_NULL_HANDLE;
    for (VkDescriptorSetLayout* l : {&m_expandLayout, &m_sortLayout, &m_drawLayout, &m_forwardLayout}) {
        if (*l) vkDestroyDescriptorSetLayout(m_device, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
    for (auto& s : m_slots) {
        for (GpuBuffer* b : {&s.emitters, &s.batches, &s.tracerVerts}) m_allocator->retire(*b);
        s = Slot{};
    }
    m_allocator->retire(m_particles);
    m_allocator->retire(m_order);
    m_particleCapacity = 0;
    m_allocator->retire(m_dummyBuffer);
    m_openpbrTables.destroy();
    m_allocator->retire(m_dummy);
    if (m_sampler) vkDestroySampler(m_device, m_sampler, nullptr);
    m_sampler = VK_NULL_HANDLE;
    m_objects.clear();
    m_batches.clear();
    m_stats = {};
    m_device = VK_NULL_HANDLE;
}

void VfxRenderer::destroySizeResources() {
    m_allocator->retire(m_mask);
    m_allocator->retire(m_refraction);
}

bool VfxRenderer::createLayouts(VkDescriptorSetLayout textures, VkDescriptorSetLayout gbufferScene) {
    constexpr VkDescriptorType kStorage = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    constexpr VkDescriptorType kSampled = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    constexpr VkDescriptorType kSampler = VK_DESCRIPTOR_TYPE_SAMPLER;
    constexpr VkDescriptorType kTlas = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    constexpr VkShaderStageFlags kDraw = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    constexpr VkShaderStageFlags kFrag = VK_SHADER_STAGE_FRAGMENT_BIT;

    const auto expand = shader::layoutBindings(shader::vfx_expand::kSet0, VK_SHADER_STAGE_COMPUTE_BIT);
    const auto sort = shader::layoutBindings(shader::vfx_sort::kSet0, VK_SHADER_STAGE_COMPUTE_BIT);
    const auto draw = shader::layoutBindings(shader::vfx_particle::kSet0, kDraw);   // particles and tracers
    // The forward (transparent mesh) pass's own set: the light loop's inputs, OpenPBR's tables, the refraction
    // source, and the scene's meshes, indices and vertices (glass finds its far side's normal).
    const auto forward = shader::layoutBindings(shader::vfx_transparent_mesh::kSet2, kFrag);
    // The TLAS is partially bound: a frame without one writes none (positions are bindings: the sets run 0..n).
    m_expandLayout = makeLayout(m_device, expand, static_cast<int>(shader::vfx_expand::scene.binding));
    m_sortLayout = makeLayout(m_device, sort);
    m_drawLayout = makeLayout(m_device, draw);
    m_forwardLayout = makeLayout(m_device, forward, static_cast<int>(shader::vfx_transparent_mesh::scene.binding));
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

bool VfxRenderer::createPipelines() {
    if (!rtc::createComputePipeline(m_device, "vfx_expand.slang.spv", "expandParticles", m_expandLayout,
                                    sizeof(shared::VfxExpandPush), m_expandPipelineLayout, m_expandPipeline) ||
        !rtc::createComputePipeline(m_device, "vfx_sort.slang.spv", "sort", m_sortLayout, sizeof(shared::VfxSortPush),
                                    m_sortPipelineLayout, m_sortPipeline))
        return false;

    // Every pipeline of the transparent pass: the lit image and the coverage mask (MAX), the G-buffer's depth
    // tested and never written.
    const std::array<VkFormat, 2> formats{kHdrFormat, kMaskFormat};
    std::array<render::Blend, 2> blends{blendState(shared::kVfxBlendAlpha), maskState()};
    render::GraphicsPipelineDesc desc{};
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.colorFormats = formats;
    desc.blends = blends;
    desc.depthFormat = kDepthFormat;
    desc.depthTest = true;
    desc.depthWrite = false;
    desc.depthCompare = VK_COMPARE_OP_LESS;
    bool ok = true;

    // Particles: one pipeline a blend mode, both faces (a quad faces the camera).
    desc.name = "vfx.particle";
    desc.vertex = "vfx_particle.slang.vertexMain.spv";
    desc.fragment = "vfx_particle.slang.fragmentMain.spv";
    desc.layout = m_drawPipelineLayout;
    for (uint32_t mode = 0; mode < 3 && ok; ++mode) {
        blends[0] = blendState(mode);
        m_particlePipelines[mode] = render::createGraphicsPipeline(m_device, desc);
        ok = m_particlePipelines[mode] != VK_NULL_HANDLE;
    }
    // Blended meshes: core/gbuffer.slang's vertex input, straight alpha by coverage; [0] one-sided
    // (back faces culled, as the G-buffer culls them), [1] double-sided.
    const auto bindingDescs = render::vertexBindings();
    const auto attributeDescs = render::vertexAttributes();
    if (ok) {
        desc.name = "vfx.mesh";
        desc.vertex = "core_gbuffer.slang.vertexMain.spv";
        desc.fragment = "vfx_transparent_mesh.slang.spv";
        desc.layout = m_meshPipelineLayout;
        desc.vertexBindings = bindingDescs;
        desc.vertexAttributes = attributeDescs;
        blends[0] = blendState(shared::kVfxBlendAlpha);
        for (uint32_t k = 0; k < 2 && ok; ++k) {
            desc.cull = k == 0 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
            m_meshPipelines[k] = render::createGraphicsPipeline(m_device, desc);
            ok = m_meshPipelines[k] != VK_NULL_HANDLE;
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
        desc.name = "vfx.tracer";
        desc.vertex = "vfx_tracer.slang.vertexMain.spv";
        desc.fragment = "vfx_tracer.slang.fragmentMain.spv";
        desc.layout = m_tracerPipelineLayout;
        desc.vertexBindings = {&binding, 1};
        desc.vertexAttributes = attributes;
        desc.cull = VK_CULL_MODE_NONE;
        blends[0] = blendState(shared::kVfxBlendAdditive);
        m_tracerPipeline = render::createGraphicsPipeline(m_device, desc);
        ok = m_tracerPipeline != VK_NULL_HANDLE;
    }
    return ok;
}

bool VfxRenderer::resize(uint32_t width, uint32_t height, gpu::Image& hdr, gpu::Image& depth) {
    if (!ready()) return false;
    destroySizeResources();
    m_width = std::max(width, 1u);
    m_height = std::max(height, 1u);
    m_hdr = &hdr;
    m_depth = &depth;
    m_mask = m_allocator->createImage({"vfx.mask", kMaskFormat, m_width, m_height,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
    m_refraction = m_allocator->createImage({"vfx.refraction", kHdrFormat, m_width, m_height,
                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
    return m_mask && m_refraction;
}

bool VfxRenderer::ensureHostBuffer(GpuBuffer& buf, VkDeviceSize& cap, VkDeviceSize bytes, VkBufferUsageFlags usage) {
    if (buf.buffer != VK_NULL_HANDLE && cap >= bytes) return true;
    m_allocator->destroyBuffer(buf);   // a slot's buffer: its fence was waited
    cap = std::max<VkDeviceSize>(bytes, 4096);
    buf = rtc::createHostBuffer(*m_allocator, cap, usage);
    return buf.isValid();
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
    if (!ready() || !m_mask || slot >= m_slots.size()) return;

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
            // The shared slots are re-made (a set's cap grew: rare); the frames in flight keep the old ones.
            m_allocator->retire(m_particles);
            m_allocator->retire(m_order);
            m_particleCapacity = 0;
            m_particles = rtc::createHostBuffer(*m_allocator, VkDeviceSize(cap) * sizeof(shared::VfxParticleGpu),
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            m_order = rtc::createHostBuffer(*m_allocator, VkDeviceSize(cap) * sizeof(uint32_t),
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            if (m_particles.isValid() && m_order.isValid()) m_particleCapacity = cap;
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
        std::memcpy(s.emitters.getMappedData(), m_emitterTable.data(), static_cast<size_t>(emitterBytes));
        m_allocator->flushBuffer(s.emitters, 0, emitterBytes);
        auto* out = static_cast<shared::VfxBatchGpu*>(s.batches.getMappedData());
        for (size_t i = 0; i < m_batches.size(); ++i) {
            const Batch& b = m_batches[i];
            shared::VfxBatchGpu g{};
            copyMatrix(g.pose, b.pose);
            g.info = {b.emitter, b.seed, b.first, b.count};
            g.timing = {b.age, 0.0f, 0.0f, 0.0f};
            out[i] = g;
        }
        m_allocator->flushBuffer(s.batches, 0, batchBytes);
    }
    // The tracers' vertices, in this slot's buffer; without one, no tracer is drawn.
    if (!m_tracerVerts.empty()) {
        const VkDeviceSize bytes = m_tracerVerts.size() * sizeof(shared::TracerVertexGpu);
        if (ensureHostBuffer(s.tracerVerts, s.tracerBytes, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
            std::memcpy(s.tracerVerts.getMappedData(), m_tracerVerts.data(), static_cast<size_t>(bytes));
            m_allocator->flushBuffer(s.tracerVerts, 0, bytes);
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
        {lighting.lights ? lighting.lights->buffer : dummy, 0, VK_WHOLE_SIZE},
        {lighting.clusters ? lighting.clusters->buffer : dummy, 0, VK_WHOLE_SIZE},
    }};
    const std::array<VkDescriptorImageInfo, 2> images{{
        {VK_NULL_HANDLE, lighting.giRadiance ? lighting.giRadiance->view : m_dummy.view, kReadOnly},
        {VK_NULL_HANDLE, lighting.envIrradiance ? lighting.envIrradiance->view : m_dummy.view, kReadOnly},
    }};
    const VkDescriptorImageInfo sampler{m_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSetAccelerationStructureKHR as{};
    as.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    as.accelerationStructureCount = 1;
    as.pAccelerationStructures = &lighting.tlas;

    std::vector<VkWriteDescriptorSet> w;
    namespace E = shader::vfx_expand;
    const shader::Binding bufferBindings[] = {E::emitters, E::batches, E::particles, E::order, E::lights, E::clusters};
    static_assert(std::size(bufferBindings) == decltype(buffers){}.size());
    for (uint32_t i = 0; i < buffers.size(); ++i) {
        w.push_back(shader::write(bufferBindings[i], s.expandSet));
        w.back().pBufferInfo = &buffers[i];
    }
    if (lighting.tlas != VK_NULL_HANDLE) {
        w.push_back(shader::write(E::scene, s.expandSet));
        w.back().pNext = &as;
    }
    const shader::Binding imageBindings[] = {E::giRadiance, E::envIrradiance};
    static_assert(std::size(imageBindings) == decltype(images){}.size());
    for (uint32_t i = 0; i < images.size(); ++i) {
        w.push_back(shader::write(imageBindings[i], s.expandSet));
        w.back().pImageInfo = &images[i];
    }
    w.push_back(shader::write(E::linearSampler, s.expandSet));
    w.back().pImageInfo = &sampler;
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
}

void VfxRenderer::writeForwardSet(uint32_t slot, const Lighting& lighting, const render::RenderScene& scene,
                                  bool refracted) {
    Slot& s = m_slots[slot];
    const VkBuffer dummy = m_dummyBuffer.buffer;
    const std::array<VkDescriptorBufferInfo, 3> geometry{{
        {scene.meshTable() ? scene.meshTable() : dummy, 0, VK_WHOLE_SIZE},
        {scene.indexArena() ? scene.indexArena() : dummy, 0, VK_WHOLE_SIZE},
        {scene.vertexArena() ? scene.vertexArena() : dummy, 0, VK_WHOLE_SIZE},
    }};
    const std::array<VkDescriptorBufferInfo, 2> buffers{{
        {lighting.lights ? lighting.lights->buffer : dummy, 0, VK_WHOLE_SIZE},
        {lighting.clusters ? lighting.clusters->buffer : dummy, 0, VK_WHOLE_SIZE},
    }};
    const VkDescriptorBufferInfo tables{m_openpbrTables.buffer(), 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo irradiance{
        VK_NULL_HANDLE, lighting.envIrradiance ? lighting.envIrradiance->view : m_dummy.view, kReadOnly};
    const VkDescriptorImageInfo prefiltered{
        VK_NULL_HANDLE, lighting.envPrefiltered ? lighting.envPrefiltered->view : m_dummy.view, kReadOnly};
    const VkDescriptorImageInfo sampler{m_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    const VkDescriptorImageInfo gi{VK_NULL_HANDLE, lighting.giRadiance ? lighting.giRadiance->view : m_dummy.view,
                                   kReadOnly};
    const VkDescriptorImageInfo refraction{VK_NULL_HANDLE, refracted ? m_refraction.view : m_dummy.view, kReadOnly};
    VkWriteDescriptorSetAccelerationStructureKHR as{};
    as.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    as.accelerationStructureCount = 1;
    as.pAccelerationStructures = &lighting.tlas;

    std::vector<VkWriteDescriptorSet> w;
    namespace F = shader::vfx_transparent_mesh;
    const auto add = [&](const shader::Binding& b) -> VkWriteDescriptorSet& {
        w.push_back(shader::write(b, s.forwardSet));
        return w.back();
    };
    add(F::lights).pBufferInfo = &buffers[0];
    add(F::clusters).pBufferInfo = &buffers[1];
    add(F::envIrradiance).pImageInfo = &irradiance;
    add(F::envPrefiltered).pImageInfo = &prefiltered;
    add(F::linearSampler).pImageInfo = &sampler;
    if (lighting.tlas != VK_NULL_HANDLE) add(F::scene).pNext = &as;
    add(F::giRadiance).pImageInfo = &gi;
    add(F::openpbrTables).pBufferInfo = &tables;
    add(F::refraction).pImageInfo = &refraction;
    add(F::meshes).pBufferInfo = &geometry[0];
    add(F::indices).pBufferInfo = &geometry[1];
    add(F::vertices).pBufferInfo = &geometry[2];
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
}

namespace {

// The flags vfx/expand.slang and vfx/transparent_mesh.slang read (config.w / config.y).
uint32_t lightingFlags(const VfxRenderer::Lighting& l) {
    return (l.clusters ? 1u : 0u) | (l.giRadiance ? 2u : 0u) | (l.envIrradiance && l.envPrefiltered ? 4u : 0u) |
           (l.tlas ? 8u : 0u);
}

} // namespace

void VfxRenderer::zeroDummy(const render::Frame& frame) {
    if (m_dummy.state(0, 0).layout != VK_IMAGE_LAYOUT_UNDEFINED) return;
    render::Pass pass(frame, nullptr, render::Pass::Transfer);
    pass.copyDst(m_dummy);
    pass.begin();
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(frame.cmd, m_dummy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
}

void VfxRenderer::expand(const render::Frame& frame, const render::RenderScene& scene, const Lighting& lighting) {
    const VkCommandBuffer cmd = frame.cmd;
    const uint32_t slot = frame.slot;
    if (m_stats.particles == 0 || slot >= m_slots.size()) return;
    zeroDummy(frame);
    writeExpandSet(slot, lighting);

    // The emitters and batches in, each particle slot and its place in the order out, lit by the clusters'
    // lights, the sun's ray and RT GI.
    Slot& s = m_slots[slot];
    render::Pass pass(frame, nullptr, render::Pass::Compute);
    pass.read(s.emitters, render::BufferUse::Storage).read(s.batches, render::BufferUse::Storage);
    pass.write(m_particles, render::BufferUse::Storage).write(m_order, render::BufferUse::Storage);
    pass.read(lighting.lights ? *lighting.lights : m_dummyBuffer, render::BufferUse::Storage);
    pass.read(lighting.clusters ? *lighting.clusters : m_dummyBuffer, render::BufferUse::Storage);
    if (lighting.tlas != VK_NULL_HANDLE) scene.traced(pass);
    if (lighting.envIrradiance) pass.sampled(*lighting.envIrradiance);
    pass.sampled(m_dummy);
    if (lighting.giRadiance) pass.sampled(*lighting.giRadiance);
    pass.begin();

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

}

void VfxRenderer::sort(const render::Frame& frame) {
    const VkCommandBuffer cmd = frame.cmd;
    const uint32_t slot = frame.slot;
    if (m_stats.sortedBatches == 0 || slot >= m_slots.size()) return;
    Slot& s = m_slots[slot];
    const std::array<VkDescriptorBufferInfo, 2> buffers{{{m_particles.buffer, 0, VK_WHOLE_SIZE},
                                                         {m_order.buffer, 0, VK_WHOLE_SIZE}}};
    std::array<VkWriteDescriptorSet, 2> w{shader::write(shader::vfx_sort::particles, s.sortSet),
                                          shader::write(shader::vfx_sort::order, s.sortSet)};
    w[0].pBufferInfo = &buffers[0];
    w[1].pBufferInfo = &buffers[1];
    vkUpdateDescriptorSets(m_device, 2, w.data(), 0, nullptr);

    render::Pass pass(frame, nullptr, render::Pass::Compute);
    pass.read(m_particles, render::BufferUse::Storage).readWrite(m_order, render::BufferUse::Storage);
    pass.begin();
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
}

bool VfxRenderer::copyRefraction(const render::Frame& frame) {
    if (!m_hdr || !*m_hdr || !m_refraction) return false;
    render::Pass pass(frame, nullptr, render::Pass::Transfer);
    pass.copySrc(*m_hdr).copyDst(m_refraction);
    pass.begin();
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {m_width, m_height, 1};
    vkCmdCopyImage(frame.cmd, m_hdr->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_refraction.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    return true;
}

void VfxRenderer::drawTransparent(const render::Frame& frame, const render::RenderScene& scene, const Lighting& lighting,
                                  VkDescriptorSet textures, GBufferPass& gbuffer) {
    const VkCommandBuffer cmd = frame.cmd;
    const uint32_t slot = frame.slot;
    if (m_objects.empty() || slot >= m_slots.size() || !m_mask || !m_hdr || !*m_hdr || !m_depth || !*m_depth) return;
    Slot& s = m_slots[slot];
    zeroDummy(frame);
    const VkDescriptorSet gbufferScene = gbuffer.sceneSet(slot);
    const bool meshes = !m_meshDraws.empty() && gbufferScene != VK_NULL_HANDLE && scene.vertexArena() != VK_NULL_HANDLE &&
                        scene.indexArena() != VK_NULL_HANDLE;
    if (m_stats.particles > 0) {
        const std::array<VkDescriptorBufferInfo, 3> buffers{{{m_particles.buffer, 0, VK_WHOLE_SIZE},
                                                             {m_order.buffer, 0, VK_WHOLE_SIZE},
                                                             {s.emitters.buffer, 0, VK_WHOLE_SIZE}}};
        const VkDescriptorImageInfo depth{VK_NULL_HANDLE, m_depth->view, kDepthReadOnly};
        std::array<VkWriteDescriptorSet, 4> w{};
        namespace P = shader::vfx_particle;
        const shader::Binding bufferBindings[3] = {P::particles, P::order, P::emitters};
        for (uint32_t i = 0; i < 3; ++i) {
            w[i] = shader::write(bufferBindings[i], s.drawSet);
            w[i].pBufferInfo = &buffers[i];
        }
        w[3] = shader::write(P::sceneDepth, s.drawSet);
        w[3].pImageInfo = &depth;
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(w.size()), w.data(), 0, nullptr);
    }
    bool refracted = false;
    if (meshes) {
        refracted = copyRefraction(frame);
        writeForwardSet(slot, lighting, scene, refracted);
    }

    // The lit image kept and drawn over, the coverage mask cleared, the G-buffer's depth tested, sampled (the
    // particles' soft edge) and never written.
    render::Pass pass(frame, nullptr, render::Pass::Raster);
    if (m_stats.particles > 0) {
        pass.read(m_particles, render::BufferUse::Storage).read(m_order, render::BufferUse::Storage);
        pass.read(s.emitters, render::BufferUse::Storage);
    }
    if (meshes) {
        gbuffer.declareScene(pass, slot);
        const render::RenderScene::Buffers b = scene.buffers();
        // The forward set's own view of the geometry (a refracted ray's hit).
        pass.read(b.meshes, render::BufferUse::Storage).read(b.indices, render::BufferUse::Storage);
        pass.read(b.vertices, render::BufferUse::Storage);
        pass.read(lighting.lights ? *lighting.lights : m_dummyBuffer, render::BufferUse::Storage);
        pass.read(lighting.clusters ? *lighting.clusters : m_dummyBuffer, render::BufferUse::Storage);
        if (lighting.tlas != VK_NULL_HANDLE) scene.traced(pass);
        pass.read(m_openpbrTables.tracked(), render::BufferUse::Storage);
        if (lighting.envIrradiance) pass.sampled(*lighting.envIrradiance);
        if (lighting.envPrefiltered) pass.sampled(*lighting.envPrefiltered);
        pass.sampled(m_dummy);
        if (lighting.giRadiance) pass.sampled(*lighting.giRadiance);
        if (refracted) pass.sampled(m_refraction);
    }
    if (!m_tracerVerts.empty()) pass.read(s.tracerVerts, render::BufferUse::Vertex);
    pass.color(*m_hdr, render::Load::Keep).color(m_mask, render::Load::Clear).depthRead(*m_depth);
    pass.begin();

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
}

std::vector<shared::VfxParticleGpu> VfxRenderer::readParticles() {
    std::vector<shared::VfxParticleGpu> out;
    if (m_device == VK_NULL_HANDLE || m_particles.getMappedData() == nullptr) return out;
    vkDeviceWaitIdle(m_device);
    m_allocator->invalidateBuffer(m_particles);
    const auto* p = static_cast<const shared::VfxParticleGpu*>(m_particles.getMappedData());
    out.assign(p, p + m_stats.particles);
    return out;
}

} // namespace ohao
