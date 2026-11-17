#include "gbuffer_pass.hpp"
#include "render/scene/vertex_input.hpp"
#include "render/shader_bindings/core_gbuffer.hpp"
#include "render/shader_bindings/cull_meshlets.hpp"
#include "gpu_culling.hpp"
#include "render/graph/pipeline.hpp"
#include "render/scene/render_scene.hpp"
#include "asset/vertex.hpp"
#include "gpu/shared_layout.hpp"
#include "core/log.hpp"
#include <array>
#include <cstring>

namespace ohao {

namespace {
// Set 1, the scene, as the G-buffer (core/gbuffer.slang) and its meshlet path (cull/meshlets.slang) read it: the
// G-buffer's six tables, then the meshlet path's arenas, commands and Hi-Z. Both shaders declare the shared ones alike.
namespace G = shader::core_gbuffer;
namespace M = shader::cull_meshlets;
constexpr std::array<shader::Binding, 12> kScene{G::instances, G::matRows, G::materialRefs, G::camera, G::sssTiles,
                                                 G::draws, M::vertices, M::meshlets, M::meshletVertices,
                                                 M::meshletTriangles, M::commands, M::hiz};
constexpr bool same(const shader::Binding& a, const shader::Binding& b) {
    return a.set == b.set && a.binding == b.binding && a.type == b.type;
}
static_assert(same(M::instances, G::instances) && same(M::camera, G::camera) && same(M::draws, G::draws));
constexpr bool inOrder() {
    for (std::size_t i = 0; i < kScene.size(); ++i)
        if (kScene[i].set != 1 || kScene[i].binding != i) return false;
    return true;
}
static_assert(inOrder(), "the scene set's bindings are 0..11 in order (the infos below are indexed by binding)");

struct Target {
    const char* name;
    VkFormat format;
};
// The targets by index (gbuffer_pass.hpp); 4 is the depth.
constexpr std::array<Target, 9> kTargets{{
    {"gbuffer.position", VK_FORMAT_R16G16B16A16_SFLOAT},   // position + metallic
    {"gbuffer.normal", VK_FORMAT_R16G16B16A16_SFLOAT},     // octahedral normal + roughness + emissive / -subsurface
    {"gbuffer.albedo", VK_FORMAT_R8G8B8A8_SRGB},           // albedo + AO
    {"gbuffer.velocity", VK_FORMAT_R16G16_SFLOAT},
    {"gbuffer.depth", VK_FORMAT_D32_SFLOAT},
    {"gbuffer.emissive", VK_FORMAT_R16G16B16A16_SFLOAT},   // emitted radiance
    {"gbuffer.specular", VK_FORMAT_R8G8B8A8_UNORM},        // OpenPBR's specular: specular_color, specular_weight / 1.5
    {"gbuffer.openpbr", VK_FORMAT_R8G8_UNORM},             // OpenPBR's base: base_diffuse_roughness, (specular_ior - 1) / 2.55
    {"gbuffer.layers", VK_FORMAT_R32G32_UINT},             // OpenPBR's layers: material index, anisotropy and its angle; coat, fuzz
}};
// The colour attachments in the fragment shader's output order: outputs 0-3 are targets 0-3, outputs 4-7 the
// emissive, specular, OpenPBR and layers ones (past the depth).
constexpr std::array<uint32_t, 8> kColorTargets{0, 1, 2, 3, 5, 6, 7, 8};
constexpr std::array<VkClearColorValue, 8> kColorClears{{
    {{0.0f, 0.0f, 0.0f, 0.0f}},   // position (rgb) + metallic (a)
    {{0.5f, 0.5f, 0.5f, 0.0f}},   // octahedral normal (rg) + roughness (b) + emissive (a)
    {{0.0f, 0.0f, 0.0f, 1.0f}},   // albedo (rgb) + AO (a)
    {{0.0f, 0.0f, 0.0f, 0.0f}},   // velocity
    {{0.0f, 0.0f, 0.0f, 0.0f}},   // emitted radiance
    {{0.0f, 0.0f, 0.0f, 0.0f}},   // OpenPBR's specular
    {{0.0f, 0.0f, 0.0f, 0.0f}},   // OpenPBR's base
    {{0.0f, 0.0f, 0.0f, 0.0f}},   // OpenPBR's layers: none (every bit zero)
}};
} // namespace

GBufferPass::~GBufferPass() {
    cleanup();
}

bool GBufferPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    // Default size - will be resized
    m_width = 1920;
    m_height = 1080;

    if (!m_allocator) return false;
    if (!createGBuffer()) return false;
    if (!createSceneDescriptors()) return false;
    if (!createPipeline()) return false;

    return true;
}

void GBufferPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    safeDestroy(m_wireframePipeline);
    safeDestroy(m_pipeline);
    safeDestroy(m_cullBackPipeline);
    for (VkPipeline& p : m_meshPipelines) safeDestroy(p);
    for (VkPipeline& p : m_pullPipelines) safeDestroy(p);
    for (VkPipeline& p : m_maskPipelines) safeDestroy(p);
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    destroySceneDescriptors();
    destroyGBuffer();
}

void GBufferPass::record(const render::Frame& frame) {
    // Always run both raster phases, scene or not: the early one's clears are what every
    // later pass samples. With no scene the G-buffer is simply empty (depth 1 = all sky).

    // The slot's set and camera UBO are written and the SSS classification reset (no tile
    // flagged: the SSS dispatches are empty) before the first phase.
    const bool haveScene = m_renderScene && m_culling && m_culling->drawCount() > 0 && updateSceneSet(frame.slot);
    resetSSSTiles(frame);
    // The draws are culled with the view-projection they are rasterized with (jitter included).
    const glm::mat4 viewProj = m_projection * m_view;
    m_cameraPos = glm::vec3(glm::inverse(m_view)[3]);
    namespace shared = gpu::shared;

    {
        render::Timed timed(frame, "cull.instances");
        if (haveScene) m_culling->cull(frame, shared::kCullPhaseEarly, shared::kCullListEarly, 4, viewProj, m_cameraPos);
    }
    drawPass(frame, false, haveScene);
    {
        render::Timed timed(frame, "hiz.build");
        if (haveScene) m_culling->buildHiZ(frame);
    }
    {
        render::Timed timed(frame, "cull.late");
        if (haveScene) m_culling->cull(frame, shared::kCullPhaseLate, shared::kCullListLate, 4, viewProj, m_cameraPos);
    }
    drawPass(frame, true, haveScene);
}

void GBufferPass::declareScene(render::Pass& pass, uint32_t slot) {
    const render::RenderScene::Buffers b = m_renderScene->buffers();
    for (GpuBuffer* table : {&b.instances, &b.materials, &b.materialRefs, &b.draws})
        pass.read(*table, render::BufferUse::Storage);
    pass.read(b.vertices, render::BufferUse::Vertex).read(b.indices, render::BufferUse::Index);
    pass.read(m_cameraBuffers[slot], render::BufferUse::Uniform);
}

VkShaderStageFlags GBufferPass::geometryStages() const noexcept {
    VkShaderStageFlags s = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    if (m_meshShaderSupported) s |= VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
    return s;
}

void GBufferPass::drawPass(const render::Frame& frame, bool late, bool draw) {
    const VkCommandBuffer cmd = frame.cmd;
    const uint32_t frameIndex = frame.slot;
    draw = draw && m_culling->drawCount() > 0;

    // The early phase clears the targets; the late one loads what it drew.
    render::Pass pass(frame, late ? "gbuffer.late" : "gbuffer.early", render::Pass::Raster);
    pass.meshShaders(draw && m_culling->meshShadersActive());
    const render::Load load = late ? render::Load::Keep : render::Load::Clear;
    for (std::size_t i = 0; i < kColorTargets.size(); ++i) pass.color(m_gbuffer[kColorTargets[i]], load, kColorClears[i]);
    pass.depth(m_gbuffer[kDepth], load, 1.0f);
    if (draw) {
        // The bindless textures are immutable once uploaded and always SHADER_READ_ONLY: nothing to declare.
        declareScene(pass, frameIndex);
        const render::RenderScene::Buffers b = m_renderScene->buffers();
        for (GpuBuffer* arena : {&b.vertices, &b.meshlets, &b.meshletVertices, &b.meshletTriangles})
            pass.read(*arena, render::BufferUse::Storage);   // the meshlet path pulls its vertices itself
        m_culling->declareDraws(pass, true);
        pass.write(m_sssTiles, render::BufferUse::Storage);
    }
    pass.begin();

    if (draw) {
        // Set 0: bindless textures; set 1: the scene.
        const VkDescriptorSet texSet = m_textureManager ? m_textureManager->getDescriptorSet() : VK_NULL_HANDLE;
        if (texSet != VK_NULL_HANDLE)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &texSet, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 1, 1, &m_sceneSets[frameIndex],
                                0, nullptr);
        const VkBuffer vertexBuffer = m_renderScene->vertexArena();
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);
        vkCmdBindIndexBuffer(cmd, m_renderScene->indexArena(), 0, VK_INDEX_TYPE_UINT32);
        namespace shared = gpu::shared;
        const uint32_t phase = late ? shared::kCullPhaseLate : shared::kCullPhaseEarly;
        const uint32_t firstList = late ? shared::kCullListLate : shared::kCullListEarly;
        const VkExtent2D tiles = sssTiles();
        const DrawPushConstant push{tiles.width, tiles.height, 0u, 0u};
        vkCmdPushConstants(cmd, m_pipelineLayout, geometryStages(), 0, sizeof(DrawPushConstant), &push);
        // The phase's lists: instance draws one-sided (back faces culled) and double-sided,
        // then meshlet draws one-sided and double-sided.
        const bool wire = m_wireframeEnabled && m_wireframePipeline != VK_NULL_HANDLE;
        for (uint32_t k = 0; k < 2; ++k) {
            const VkPipeline pipeline = wire ? m_wireframePipeline : (k == 0 ? m_cullBackPipeline : m_pipeline);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            m_culling->drawList(cmd, firstList + k);
        }
        const bool meshShaders = m_culling->meshShadersActive() && m_meshPipelines[0] != VK_NULL_HANDLE;
        const VkShaderStageFlags meshletStages = geometryStages() & ~VkShaderStageFlags(VK_SHADER_STAGE_FRAGMENT_BIT);
        for (uint32_t k = 0; k < 2; ++k) {
            const uint32_t list = firstList + 2 + k;
            if (meshShaders) {
                shared::MeshletPush mp{};
                mp.config = {list * m_culling->drawCount() * 5u, m_culling->meshletFlags(phase), m_culling->hizLevels(), 0u};
                mp.hiz = {m_width, m_height, 0u, 0u};
                mp.camera = {m_cameraPos.x, m_cameraPos.y, m_cameraPos.z, 0.0f};
                vkCmdPushConstants(cmd, m_pipelineLayout, meshletStages, sizeof(DrawPushConstant), sizeof(mp), &mp);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_meshPipelines[k]);
                m_culling->drawMeshTasks(cmd, list);
            } else if (m_pullPipelines[k] != VK_NULL_HANDLE) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pullPipelines[k]);
                m_culling->drawMeshletFallback(cmd, (late ? 2u : 0u) + k);
            }
        }
        // The alpha-tested draws (in no list): every one, in draw order, after the opaque ones so
        // their discards cost no early depth to the rest. firstInstance is the draw's index, as
        // the culling's commands give it.
        if (late && !wire && m_maskPipelines[0] != VK_NULL_HANDLE) {
            const auto all = m_renderScene->draws();
            for (uint32_t k = 0; k < 2; ++k) {
                bool bound = false;
                for (const uint32_t i : m_renderScene->maskDraws()) {
                    const auto& d = all[i];
                    if ((k == 0) != d.oneSided) continue;
                    if (!bound) {
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_maskPipelines[k]);
                        bound = true;
                    }
                    vkCmdDrawIndexed(cmd, d.indexCount, 1, d.firstIndex, d.vertexOffset, i);
                }
            }
        }
    }
}

VkExtent2D GBufferPass::sssTiles() const {
    return {(m_width + gpu::shared::kSSSTile - 1) / gpu::shared::kSSSTile,
            (m_height + gpu::shared::kSSSTile - 1) / gpu::shared::kSSSTile};
}

uint32_t GBufferPass::sssTilesFlagged() const {
    const void* mapped = m_sssTiles.getMappedData();
    return mapped ? static_cast<const uint32_t*>(mapped)[0] : 0u;
}

void GBufferPass::resetSSSTiles(const render::Frame& frame) {
    if (!m_sssTiles.buffer) return;
    namespace shared = gpu::shared;
    const VkExtent2D tiles = sssTiles();
    render::Pass pass(frame, nullptr, render::Pass::Transfer);
    pass.write(m_sssTiles, render::BufferUse::CopyDst);
    pass.begin();
    // Header: two empty indirect dispatches (x 0, y 1, z 1); the mask cleared for this frame's tiles.
    const uint32_t header[shared::kSSSHeaderWords] = {0u, 1u, 1u, 0u, 0u, 1u, 1u, 0u};
    vkCmdUpdateBuffer(frame.cmd, m_sssTiles.buffer, 0, sizeof(header), header);
    vkCmdFillBuffer(frame.cmd, m_sssTiles.buffer, sizeof(header), uint64_t(tiles.width) * tiles.height * sizeof(uint32_t),
                    0u);
}

bool GBufferPass::updateSceneSet(uint32_t frameIndex) {
    if (frameIndex >= MAX_FRAMES_IN_FLIGHT || m_sceneSets[frameIndex] == VK_NULL_HANDLE) return false;
    const VkBuffer instances = m_renderScene->instanceBuffer();
    const VkBuffer materials = m_renderScene->materialBuffer();
    const VkBuffer materialRefs = m_renderScene->materialRefBuffer();
    const VkBuffer drawTable = m_renderScene->drawTable();
    if (instances == VK_NULL_HANDLE || materials == VK_NULL_HANDLE || materialRefs == VK_NULL_HANDLE ||
        drawTable == VK_NULL_HANDLE || m_renderScene->vertexArena() == VK_NULL_HANDLE ||
        m_renderScene->indexArena() == VK_NULL_HANDLE || m_renderScene->meshletArena() == VK_NULL_HANDLE ||
        m_culling->commands().buffer == VK_NULL_HANDLE || !m_culling->hiz())
        return false;

    const CameraUBO camera{m_projection * m_view, m_prevViewProj};
    std::memcpy(m_cameraBuffers[frameIndex].getMappedData(), &camera, sizeof(camera));
    m_allocator->flushBuffer(m_cameraBuffers[frameIndex]);

    // The table buffers can be replaced (arena / table growth), so the set is
    // rewritten every frame; the slot's previous use finished at its fence wait.
    const std::array<VkDescriptorBufferInfo, kSceneBindings> infos{{
        {instances, 0, VK_WHOLE_SIZE},
        {materials, 0, VK_WHOLE_SIZE},
        {materialRefs, 0, VK_WHOLE_SIZE},
        {m_cameraBuffers[frameIndex].buffer, 0, sizeof(CameraUBO)},
        {m_sssTiles.buffer, 0, VK_WHOLE_SIZE},
        {drawTable, 0, VK_WHOLE_SIZE},
        {m_renderScene->vertexArena(), 0, VK_WHOLE_SIZE},
        {m_renderScene->meshletArena(), 0, VK_WHOLE_SIZE},
        {m_renderScene->meshletVertexArena(), 0, VK_WHOLE_SIZE},
        {m_renderScene->meshletTriangleArena(), 0, VK_WHOLE_SIZE},
        {m_culling->commands().buffer, 0, VK_WHOLE_SIZE},
        {},
    }};
    const VkDescriptorImageInfo hizInfo{VK_NULL_HANDLE, m_culling->hiz().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    std::array<VkWriteDescriptorSet, kSceneBindings> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i] = shader::write(kScene[i], m_sceneSets[frameIndex]);
        if (kScene[i].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) writes[i].pImageInfo = &hizInfo;
        else writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return true;
}

bool GBufferPass::createSceneDescriptors() {
    namespace shared = gpu::shared;
    const VkDeviceSize sssBytes = VkDeviceSize(shared::kSSSHeaderWords + 3ull * shared::kSSSMaxTiles) * sizeof(uint32_t);
    m_sssTiles = m_allocator->createBuffer(sssBytes,
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           AllocationUsage::CpuOnly, true);   // the fragments write it, the host reads it
    if (!m_sssTiles.isValid() || !m_sssTiles.getMappedData()) return false;

    std::array<VkDescriptorSetLayoutBinding, kSceneBindings> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i] = shader::layoutBinding(kScene[i], geometryStages());
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_sceneLayout) != VK_SUCCESS) return false;

    // Set 0 must exist in the pipeline layout for set 1 to follow it.
    if (!m_textureManager || m_textureManager->getDescriptorSetLayout() == VK_NULL_HANDLE) {
        VkDescriptorSetLayoutCreateInfo emptyInfo{};
        emptyInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        if (vkCreateDescriptorSetLayout(m_device, &emptyInfo, nullptr, &m_emptyLayout) != VK_SUCCESS) return false;
    }

    const std::array<VkDescriptorPoolSize, 3> poolSizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (kSceneBindings - 2) * MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES_IN_FLIGHT},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MAX_FRAMES_IN_FLIGHT},
    }};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_scenePool) != VK_SUCCESS) return false;

    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(m_sceneLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_scenePool;
    allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(m_device, &allocInfo, m_sceneSets.data()) != VK_SUCCESS) return false;

    for (GpuBuffer& camera : m_cameraBuffers) {
        camera = m_allocator->createBuffer(sizeof(CameraUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, AllocationUsage::CpuToGpu,
                                           true);
        if (!camera.isValid() || !camera.getMappedData()) return false;
    }
    return true;
}

void GBufferPass::destroySceneDescriptors() {
    if (m_allocator) {
        m_allocator->retire(m_sssTiles);
        for (GpuBuffer& camera : m_cameraBuffers) m_allocator->retire(camera);
    }
    safeDestroy(m_scenePool);   // frees the sets
    m_sceneSets.fill(VK_NULL_HANDLE);
    safeDestroy(m_sceneLayout);
    safeDestroy(m_emptyLayout);
}

void GBufferPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    // The passes that follow the new targets (the Hi-Z pyramid, the decals, the transparent pass) replace
    // descriptor pools that frames in flight use.
    vkDeviceWaitIdle(m_device);

    destroyGBuffer();
    if (!createGBuffer()) OHAO_LOG(Error, render, "GBufferPass: failed to recreate its targets at {}x{}", width, height);
}

void GBufferPass::setViewProjection(const glm::mat4& view, const glm::mat4& proj,
                                    const glm::mat4& prevViewProj) {
    m_view = view;
    m_projection = proj;
    m_prevViewProj = prevViewProj;
}

bool GBufferPass::createGBuffer() {
    for (uint32_t i = 0; i < GBUFFER_COUNT; ++i) {
        const VkImageUsageFlags attachment =
            i == kDepth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        m_gbuffer[i] = m_allocator->createImage(
            {kTargets[i].name, kTargets[i].format, m_width, m_height, attachment | VK_IMAGE_USAGE_SAMPLED_BIT});
        if (!m_gbuffer[i]) return false;
    }
    return true;
}

void GBufferPass::destroyGBuffer() {
    if (!m_allocator) return;
    for (gpu::Image& target : m_gbuffer) m_allocator->retire(target);
}

bool GBufferPass::createPipeline() {
    // Push constants: the SSS tiles across and down (fragment stage, bytes 0-15); the meshlet
    // shaders' block (bytes 0-63, MeshletPush from 16; the instance vertex shader has none).
    const std::array<VkPushConstantRange, 2> pushRanges{{
        {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DrawPushConstant)},
        {geometryStages() & ~VkShaderStageFlags(VK_SHADER_STAGE_FRAGMENT_BIT), 0,
         sizeof(DrawPushConstant) + sizeof(gpu::shared::MeshletPush)},
    }};
    static_assert(sizeof(DrawPushConstant) + sizeof(gpu::shared::MeshletPush) <= 128);

    // Pipeline layout: set 0 = bindless textures (an empty stand-in without a
    // texture manager), set 1 = the scene
    const std::array<VkDescriptorSetLayout, 2> setLayouts{
        m_emptyLayout != VK_NULL_HANDLE ? m_emptyLayout : m_textureManager->getDescriptorSetLayout(),
        m_sceneLayout,
    };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushRanges.size());
    pipelineLayoutInfo.pPushConstantRanges = pushRanges.data();

    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) return false;

    // Every pipeline writes the eight colour targets unblended and tests and writes the depth.
    std::array<VkFormat, kColorTargets.size()> colorFormats{};
    for (std::size_t i = 0; i < colorFormats.size(); ++i) colorFormats[i] = kTargets[kColorTargets[i]].format;
    // Vertex input - matching model.hpp Vertex struct (uses render::vertexAttributes())
    const auto bindings = render::vertexBindings();
    const auto attributes = render::vertexAttributes();
    render::GraphicsPipelineDesc base{};
    base.fragment = "core_gbuffer.slang.opaque.spv";
    base.fragmentEntry = "opaque";
    base.layout = m_pipelineLayout;
    base.colorFormats = colorFormats;
    base.depthFormat = kTargets[kDepth].format;
    base.depthTest = true;
    base.depthWrite = true;
    base.depthCompare = VK_COMPARE_OP_LESS;
    // Front faces are counter-clockwise as the rasterizer sees them (glTF's convention; measured on the
    // helmet: test 6, the goldens).
    base.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    // The instance path. Double-sided: no culling (imported meshes are not guaranteed consistently wound);
    // one-sided materials: back faces culled.
    render::GraphicsPipelineDesc instance = base;
    instance.vertex = "core_gbuffer.slang.vertexMain.spv";
    instance.vertexEntry = "vertexMain";
    instance.vertexBindings = bindings;
    instance.vertexAttributes = attributes;
    instance.name = "gbuffer";
    instance.cull = VK_CULL_MODE_NONE;
    m_pipeline = render::createGraphicsPipeline(m_device, instance);
    instance.name = "gbuffer.cullBack";
    instance.cull = VK_CULL_MODE_BACK_BIT;
    m_cullBackPipeline = render::createGraphicsPipeline(m_device, instance);
    if (m_pipeline == VK_NULL_HANDLE || m_cullBackPipeline == VK_NULL_HANDLE) return false;

    // Alpha-tested materials: the same state with core/gbuffer.slang's `masked` (a discard; no
    // forced early depth test), [0] culling back faces, [1] none.
    render::GraphicsPipelineDesc masked = instance;
    masked.name = "gbuffer.masked";
    masked.fragment = "core_gbuffer.slang.masked.spv";
    masked.fragmentEntry = "masked";
    for (uint32_t k = 0; k < 2; ++k) {
        masked.cull = k == 0 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        m_maskPipelines[k] = render::createGraphicsPipeline(m_device, masked);
    }

    // The meshlet path: the same fragment shader and state, its own geometry stages; [0] culls back
    // faces, [1] none. The fallback's pull vertex shader reads no attributes (a triangle list).
    render::GraphicsPipelineDesc pull = base;
    pull.name = "gbuffer.pull";
    pull.vertex = "cull_meshlets.slang.pull.spv";
    pull.vertexEntry = "pull";
    render::GraphicsPipelineDesc mesh = base;
    mesh.name = "gbuffer.mesh";
    mesh.task = "cull_meshlets.slang.task.spv";
    mesh.taskEntry = "task";
    mesh.mesh = "cull_meshlets.slang.mesh.spv";
    mesh.meshEntry = "mesh";
    for (uint32_t k = 0; k < 2; ++k) {
        pull.cull = mesh.cull = k == 0 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        m_pullPipelines[k] = render::createGraphicsPipeline(m_device, pull);
        if (m_pullPipelines[k] == VK_NULL_HANDLE) return false;
        if (!m_meshShaderSupported) continue;
        m_meshPipelines[k] = render::createGraphicsPipeline(m_device, mesh);
        if (m_meshPipelines[k] == VK_NULL_HANDLE) return false;
    }

    // The wireframe variant is optional (it needs fillModeNonSolid): the pass draws filled without it.
    instance.name = "gbuffer.wireframe";
    instance.polygonMode = VK_POLYGON_MODE_LINE;
    instance.cull = VK_CULL_MODE_NONE;
    m_wireframePipeline = render::createGraphicsPipeline(m_device, instance);
    return true;
}

} // namespace ohao
