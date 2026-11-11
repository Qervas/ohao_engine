#include "render/vfx/decal_pass.hpp"
#include "render/shader_bindings/vfx_decal.hpp"
#include "render/graph/pipeline.hpp"

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

bool DecalPass::initialize(VkDevice device, GpuAllocator& allocator, VkDescriptorSetLayout textures) {
    cleanup();
    m_device = device;
    m_allocator = &allocator;
    if (!createLayouts(textures) || !createPipeline()) {
        OHAO_LOG(Warn, render, "DecalPass: the pass could not be made; no decals are drawn");
        cleanup();
        return false;
    }
    return true;
}

bool DecalPass::createLayouts(VkDescriptorSetLayout textures) {
    // Set 0: the records, the position and the depth (sampled where the decal's surface is read).
    namespace D = shader::vfx_decal;
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings{{
        shader::layoutBinding(D::decals, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
        shader::layoutBinding(D::gPosition, VK_SHADER_STAGE_FRAGMENT_BIT),
        shader::layoutBinding(D::sceneDepth, VK_SHADER_STAGE_FRAGMENT_BIT),
    }};
    static_assert(D::kSet0.size() == 3);
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
    // Albedo: blended by the decal's alpha, its alpha (the G-buffer's AO) untouched.
    // Normal: replaced, RGB only (the w channel keeps the luminance or the subsurface strength).
    constexpr VkColorComponentFlags kRgb = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    const std::array<VkFormat, 2> formats{kAlbedoFormat, kNormalFormat};
    const std::array<render::Blend, 2> blends{{
        {true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE,
         VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, kRgb},
        {false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
         VK_BLEND_OP_ADD, kRgb},
    }};
    render::GraphicsPipelineDesc desc{};
    desc.name = "decals";
    desc.vertex = "vfx_decal.slang.vertexMain.spv";
    desc.vertexEntry = "vertexMain";
    desc.fragment = "vfx_decal.slang.fragmentMain.spv";
    desc.fragmentEntry = "fragmentMain";
    desc.layout = m_pipelineLayout;
    desc.colorFormats = formats;
    desc.blends = blends;
    // The box's far faces: the counter-clockwise faces seen from outside are culled as front faces.
    desc.cull = VK_CULL_MODE_FRONT_BIT;
    desc.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // No depth test: a surface is decaled by its position, read per pixel, and the box test decides.
    m_pipeline = render::createGraphicsPipeline(m_device, desc);
    return m_pipeline != VK_NULL_HANDLE;
}

void DecalPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);
    if (m_pipeline) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    m_pipelineLayout = VK_NULL_HANDLE;
    if (m_pool) vkDestroyDescriptorPool(m_device, m_pool, nullptr);   // frees the sets
    m_pool = VK_NULL_HANDLE;
    if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    m_setLayout = VK_NULL_HANDLE;
    for (Slot& s : m_slots) {
        m_allocator->retire(s.decals);
        s = Slot{};
    }
    m_albedo = m_normal = m_position = m_depth = nullptr;
    m_device = VK_NULL_HANDLE;
}

bool DecalPass::resize(gpu::Image& albedo, gpu::Image& normal, gpu::Image& position, gpu::Image& depth) {
    if (m_device == VK_NULL_HANDLE || m_pipeline == VK_NULL_HANDLE) return false;
    m_albedo = &albedo;
    m_normal = &normal;
    m_position = &position;
    m_depth = &depth;
    // The sampled images changed: every slot's set is written again (the device is idle).
    for (uint32_t slot = 0; slot < m_slots.size(); ++slot) writeSet(slot, m_slots[slot].decals.buffer);
    return true;
}

void DecalPass::writeSet(uint32_t slot, VkBuffer decals) {
    Slot& s = m_slots[slot];
    if (s.set == VK_NULL_HANDLE) return;
    std::vector<VkWriteDescriptorSet> w;
    VkDescriptorBufferInfo buffer{decals, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo position{VK_NULL_HANDLE, m_position ? m_position->view : VK_NULL_HANDLE, kReadOnly};
    VkDescriptorImageInfo depth{VK_NULL_HANDLE, m_depth ? m_depth->view : VK_NULL_HANDLE, kDepthReadOnly};
    if (decals != VK_NULL_HANDLE) {
        w.push_back(shader::write(shader::vfx_decal::decals, s.set));
        w.back().pBufferInfo = &buffer;
    }
    if (position.imageView != VK_NULL_HANDLE) {
        w.push_back(shader::write(shader::vfx_decal::gPosition, s.set));
        w.back().pImageInfo = &position;
    }
    if (depth.imageView != VK_NULL_HANDLE) {
        w.push_back(shader::write(shader::vfx_decal::sceneDepth, s.set));
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
        m_allocator->retire(s.decals);
        s.bytes = std::max<VkDeviceSize>(bytes, VkDeviceSize(sizeof(gpu::shared::DecalGpu)) * 16);
        s.decals = m_allocator->createBuffer(s.bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, AllocationUsage::CpuToGpu, true);
        if (!s.decals.isValid() || !s.decals.getMappedData()) {
            m_allocator->retire(s.decals);
            s.bytes = 0;
            s.count = 0;
            return;
        }
        // The slot's fence was waited on: nothing reads its set.
        writeSet(slot, s.decals.buffer);
    }
    std::memcpy(s.decals.getMappedData(), decals.data(), static_cast<std::size_t>(bytes));
    m_allocator->flushBuffer(s.decals, 0, bytes);
}

void DecalPass::record(const render::Frame& frame, const View& view, VkDescriptorSet textures) {
    render::Timed timed(frame, "decals");
    const uint32_t slot = frame.slot;
    if (slot >= m_slots.size() || !ready()) return;
    Slot& s = m_slots[slot];
    if (s.count == 0 || textures == VK_NULL_HANDLE) return;
    const VkCommandBuffer cmd = frame.cmd;
    const VkExtent2D extent = m_albedo->extent();

    {
        // LEGACY(step 4a): the textures are the texture manager's, not tracked yet.
        render::Pass pass(frame, nullptr, render::Pass::Raster);
        pass.read(s.decals, render::BufferUse::Storage)
            .sampled(*m_position)
            .sampled(*m_depth)
            .color(*m_albedo, render::Load::Keep)
            .color(*m_normal, render::Load::Keep);
        pass.begin();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
        const std::array<VkDescriptorSet, 2> sets{s.set, textures};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0,
                                static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        gpu::shared::DecalPush pc{};
        copyMatrix(pc.viewProj, view.viewProj);
        pc.camera = {view.camera.x, view.camera.y, view.camera.z, 0.0f};
        pc.screen = {static_cast<float>(extent.width), static_cast<float>(extent.height), view.clearance, 0.0f};
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
                           &pc);
        vkCmdDraw(cmd, kDecalVertices, s.count, 0, 0);
    }

    // LEGACY(step 4a): RT direct light, RT GI and SSSPass sample the decaled targets undeclared.
    render::Pass legacy(frame, nullptr, render::Pass::Compute);
    legacy.sampled(*m_albedo).sampled(*m_normal);
    legacy.begin();
}

} // namespace ohao
