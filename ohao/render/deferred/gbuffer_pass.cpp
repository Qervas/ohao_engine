#include "gbuffer_pass.hpp"
#include "render/scene/vertex_input.hpp"
#include "render/shader_bindings/core_gbuffer.hpp"
#include "render/shader_bindings/cull_meshlets.hpp"
#include "gpu_culling.hpp"
#include "render/frame/gpu_pass_timer.hpp"
#include "render/scene/render_scene.hpp"
#include "asset/model.hpp"
#include "gpu/shared_layout.hpp"
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

    if (!createGBuffer()) return false;
    if (!createRenderPass()) return false;
    if (!createFramebuffer()) return false;
    if (!createSceneDescriptors()) return false;
    if (!createPipeline()) return false;

    return true;
}

void GBufferPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    if (m_wireframePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_wireframePipeline, nullptr);
        m_wireframePipeline = VK_NULL_HANDLE;
    }
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    safeDestroy(m_cullBackPipeline);
    for (VkPipeline& p : m_meshPipelines) safeDestroy(p);
    for (VkPipeline& p : m_pullPipelines) safeDestroy(p);
    for (VkPipeline& p : m_maskPipelines) safeDestroy(p);
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    destroySceneDescriptors();
    if (m_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
        m_framebuffer = VK_NULL_HANDLE;
    }
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
    safeDestroy(m_lateRenderPass);

    destroyGBuffer();
}

void GBufferPass::timerBegin(VkCommandBuffer cmd, const char* name) {
    if (m_passTimer) m_passTimer->begin(cmd, name);
}

void GBufferPass::timerEnd(VkCommandBuffer cmd) {
    if (m_passTimer) m_passTimer->end(cmd);
}

void GBufferPass::execute(VkCommandBuffer cmd, uint32_t frameIndex) {
    // Always run both render passes, scene or not: the early one's clears are what every
    // later pass samples. With no scene the G-buffer is simply empty (depth 1 = all sky).

    // The slot's set and camera UBO are written and the SSS classification reset (no tile
    // flagged: the SSS dispatches are empty) before the first render pass. The culling began
    // the frame before the graph ran (DeferredRenderer::render).
    const bool haveScene = m_renderScene && m_culling && m_culling->drawCount() > 0 && updateSceneSet(frameIndex);
    resetSSSTiles(cmd);
    // The draws are culled with the view-projection they are rasterized with (jitter included).
    const glm::mat4 viewProj = m_projection * m_view;
    m_cameraPos = glm::vec3(glm::inverse(m_view)[3]);
    namespace shared = gpu::shared;

    timerBegin(cmd, "cull.instances");
    if (haveScene) m_culling->cull(cmd, shared::kCullPhaseEarly, shared::kCullListEarly, 4, viewProj, m_cameraPos);
    timerEnd(cmd);

    timerBegin(cmd, "gbuffer.early");
    drawPass(cmd, frameIndex, false, haveScene);
    timerEnd(cmd);

    timerBegin(cmd, "hiz.build");
    if (haveScene) m_culling->buildHiZ(cmd);
    timerEnd(cmd);

    timerBegin(cmd, "cull.late");
    if (haveScene) m_culling->cull(cmd, shared::kCullPhaseLate, shared::kCullListLate, 4, viewProj, m_cameraPos);
    timerEnd(cmd);

    timerBegin(cmd, "gbuffer.late");
    drawPass(cmd, frameIndex, true, haveScene);
    timerEnd(cmd);
}

VkShaderStageFlags GBufferPass::geometryStages() const noexcept {
    VkShaderStageFlags s = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    if (m_meshShaderSupported) s |= VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
    return s;
}

void GBufferPass::drawPass(VkCommandBuffer cmd, uint32_t frameIndex, bool late, bool draw) {
    std::array<VkClearValue, GBUFFER_COUNT> clearValues{};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}}; // Position (rgb) + Metallic (a)
    clearValues[1].color = {{0.5f, 0.5f, 0.5f, 0.0f}}; // OctNormal (rg) + Roughness (b) + Emissive (a)
    clearValues[2].color = {{0.0f, 0.0f, 0.0f, 1.0f}}; // Albedo (rgb) + AO (a)
    clearValues[3].color = {{0.0f, 0.0f, 0.0f, 0.0f}}; // Velocity
    clearValues[kDepth].depthStencil = {1.0f, 0};      // Depth
    clearValues[kEmissive].color = {{0.0f, 0.0f, 0.0f, 0.0f}};   // Emitted radiance
    clearValues[kSpecular].color = {{0.0f, 0.0f, 0.0f, 0.0f}};   // OpenPBR's specular
    clearValues[kOpenPBR].color = {{0.0f, 0.0f, 0.0f, 0.0f}};    // OpenPBR's base
    clearValues[kLayers].color.uint32[0] = 0u;                     // OpenPBR's layers: none
    clearValues[kLayers].color.uint32[1] = 0u;

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = late ? m_lateRenderPass : m_renderPass;
    renderPassInfo.framebuffer = m_framebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {m_width, m_height};
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Set viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_width);
    viewport.height = static_cast<float>(m_height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {m_width, m_height};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    if (draw && m_culling->drawCount() > 0) {
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

    vkCmdEndRenderPass(cmd);
}

VkExtent2D GBufferPass::sssTiles() const {
    return {(m_width + gpu::shared::kSSSTile - 1) / gpu::shared::kSSSTile,
            (m_height + gpu::shared::kSSSTile - 1) / gpu::shared::kSSSTile};
}

uint32_t GBufferPass::sssTilesFlagged() const {
    return m_sssTiles.mapped ? static_cast<const uint32_t*>(m_sssTiles.mapped)[0] : 0u;
}

void GBufferPass::resetSSSTiles(VkCommandBuffer cmd) {
    if (!m_sssTiles.buffer) return;
    namespace shared = gpu::shared;
    const VkExtent2D tiles = sssTiles();
    // The last frame's SSS read the lists (indirect and in its shaders) before this rewrite.
    VkBufferMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = m_sssTiles.buffer;
    b.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &b, 0, nullptr);
    // Header: two empty indirect dispatches (x 0, y 1, z 1); the mask cleared for this frame's tiles.
    const uint32_t header[shared::kSSSHeaderWords] = {0u, 1u, 1u, 0u, 0u, 1u, 1u, 0u};
    vkCmdUpdateBuffer(cmd, m_sssTiles.buffer, 0, sizeof(header), header);
    vkCmdFillBuffer(cmd, m_sssTiles.buffer, sizeof(header), uint64_t(tiles.width) * tiles.height * sizeof(uint32_t), 0u);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 1,
                         &b, 0, nullptr);
}

VkBuffer GBufferPass::materialBuffer() const {
    return m_renderScene ? m_renderScene->materialBuffer() : VK_NULL_HANDLE;
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
        m_culling->commandBuffer() == VK_NULL_HANDLE || m_culling->hizView() == VK_NULL_HANDLE)
        return false;

    const CameraUBO camera{m_projection * m_view, m_prevViewProj};
    std::memcpy(m_cameraMapped[frameIndex], &camera, sizeof(camera));

    // The table buffers can be replaced (arena / table growth), so the set is
    // rewritten every frame; the slot's previous use finished at its fence wait.
    const std::array<VkDescriptorBufferInfo, kSceneBindings> infos{{
        {instances, 0, VK_WHOLE_SIZE},
        {materials, 0, VK_WHOLE_SIZE},
        {materialRefs, 0, VK_WHOLE_SIZE},
        {m_cameraBuffers[frameIndex], 0, sizeof(CameraUBO)},
        {m_sssTiles.buffer, 0, VK_WHOLE_SIZE},
        {drawTable, 0, VK_WHOLE_SIZE},
        {m_renderScene->vertexArena(), 0, VK_WHOLE_SIZE},
        {m_renderScene->meshletArena(), 0, VK_WHOLE_SIZE},
        {m_renderScene->meshletVertexArena(), 0, VK_WHOLE_SIZE},
        {m_renderScene->meshletTriangleArena(), 0, VK_WHOLE_SIZE},
        {m_culling->commandBuffer(), 0, VK_WHOLE_SIZE},
        {},
    }};
    const VkDescriptorImageInfo hizInfo{VK_NULL_HANDLE, m_culling->hizView(), VK_IMAGE_LAYOUT_GENERAL};
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
    if (!rtc::createHostBuffer(m_device, m_physicalDevice, sssBytes,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               m_sssTiles))
        return false;

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

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = sizeof(CameraUBO);
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_cameraBuffers[i]) != VK_SUCCESS) return false;

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(m_device, m_cameraBuffers[i], &req);
        VkMemoryAllocateInfo memInfo{};
        memInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memInfo.allocationSize = req.size;
        memInfo.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &memInfo, nullptr, &m_cameraMemory[i]) != VK_SUCCESS) return false;
        vkBindBufferMemory(m_device, m_cameraBuffers[i], m_cameraMemory[i], 0);
        if (vkMapMemory(m_device, m_cameraMemory[i], 0, sizeof(CameraUBO), 0, &m_cameraMapped[i]) != VK_SUCCESS)
            return false;
    }
    return true;
}

void GBufferPass::destroySceneDescriptors() {
    rtc::destroyHostBuffer(m_device, m_sssTiles);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (m_cameraMapped[i]) {
            vkUnmapMemory(m_device, m_cameraMemory[i]);
            m_cameraMapped[i] = nullptr;
        }
        safeDestroy(m_cameraBuffers[i]);
        safeFree(m_cameraMemory[i]);
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

    vkDeviceWaitIdle(m_device);

    // Recreate framebuffer
    if (m_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
    }

    destroyGBuffer();
    createGBuffer();
    createFramebuffer();
}

void GBufferPass::setViewProjection(const glm::mat4& view, const glm::mat4& proj,
                                    const glm::mat4& prevViewProj) {
    m_view = view;
    m_projection = proj;
    m_prevViewProj = prevViewProj;
}

bool GBufferPass::createGBuffer() {
    // G-Buffer formats:
    // 0: Position + Metallic - RGBA16F
    // 1: Normal + Roughness - RGB10A2
    // 2: Albedo + AO - RGBA8 SRGB
    // 3: Velocity - RG16F
    // 4: Depth - D32F
    // 5: Emitted radiance - RGBA16F

    struct GBufferFormat {
        VkFormat format;
        VkImageUsageFlags usage;
        VkImageAspectFlags aspect;
    };

    std::array<GBufferFormat, GBUFFER_COUNT> formats = {{
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT},
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT},
        {VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT},
        {VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT},
        {VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT},
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT},
        {VK_FORMAT_R8G8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT},
        {VK_FORMAT_R32G32_UINT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT}
    }};

    for (uint32_t i = 0; i < GBUFFER_COUNT; ++i) {
        auto& target = m_gbuffer[i];
        target.format = formats[i].format;
        target.width = m_width;
        target.height = m_height;

        // Create image
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = formats[i].format;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = formats[i].usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &target.image) != VK_SUCCESS) {
            return false;
        }

        // Allocate memory
        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(m_device, target.image, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &target.memory) != VK_SUCCESS) {
            return false;
        }

        vkBindImageMemory(m_device, target.image, target.memory, 0);

        // Create image view
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = target.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = formats[i].format;
        viewInfo.subresourceRange.aspectMask = formats[i].aspect;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &target.view) != VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

void GBufferPass::destroyGBuffer() {
    for (auto& target : m_gbuffer) {
        target.destroy(m_device);
    }
}

bool GBufferPass::createRenderPass() {
    // Two compatible render passes over the same six targets: the early one clears them and
    // leaves the colors as attachments and the depth readable (the Hi-Z build samples it); the
    // late one loads them all and leaves them for the passes that sample the G-buffer.
    constexpr std::array<VkFormat, GBUFFER_COUNT> kFormats = {
        VK_FORMAT_R16G16B16A16_SFLOAT,   // position + metallic
        VK_FORMAT_R16G16B16A16_SFLOAT,   // octahedral normal + roughness + emissive / -subsurface
        VK_FORMAT_R8G8B8A8_SRGB,         // albedo + AO
        VK_FORMAT_R16G16_SFLOAT,         // velocity
        VK_FORMAT_D32_SFLOAT,            // depth
        VK_FORMAT_R16G16B16A16_SFLOAT,   // emitted radiance
        VK_FORMAT_R8G8B8A8_UNORM,        // OpenPBR's specular: specular_color, specular_weight / 1.5
        VK_FORMAT_R8G8_UNORM,            // OpenPBR's base: base_diffuse_roughness, (specular_ior - 1) / 2.55
        VK_FORMAT_R32G32_UINT,           // OpenPBR's layers: material index, anisotropy and its angle; coat, fuzz
    };
    const auto make = [&](bool late, VkRenderPass& out) {
        std::array<VkAttachmentDescription, GBUFFER_COUNT> attachments{};
        for (uint32_t i = 0; i < GBUFFER_COUNT; ++i) {
            const bool depth = i == kDepth;
            VkAttachmentDescription& a = attachments[i];
            a.format = kFormats[i];
            a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = late ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            const VkImageLayout between =
                depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            a.initialLayout = late ? between : VK_IMAGE_LAYOUT_UNDEFINED;
            a.finalLayout = late ? (depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                          : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                 : between;
        }

        // The fragment shader's outputs 0-3 are attachments 0-3, outputs 4-7 the emissive, specular,
        // OpenPBR and layers ones (past the depth attachment)
        std::array<VkAttachmentReference, GBUFFER_COUNT - 1> colorRefs{};
        for (uint32_t i = 0; i < 4; ++i) colorRefs[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        colorRefs[4] = {kEmissive, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        colorRefs[5] = {kSpecular, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        colorRefs[6] = {kOpenPBR, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        colorRefs[7] = {kLayers, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkAttachmentReference depthRef{kDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
        subpass.pColorAttachments = colorRefs.data();
        subpass.pDepthStencilAttachment = &depthRef;

        constexpr VkPipelineStageFlags kAttachments = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        constexpr VkAccessFlags kWrites = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        // Identical in both passes (render pass compatibility includes the dependencies: one
        // framebuffer and one set of pipelines serve both), so each is the union of what the two
        // need. In: the previous frame's readers of the targets (lighting, the compute passes),
        // the early pass's writes and the Hi-Z build's and late cull's reads of the depth. Out:
        // the late pass's and the Hi-Z build's readers, and the late pass's attachments.
        constexpr VkPipelineStageFlags kReaders =
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        constexpr VkAccessFlags kAll = kWrites | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        std::array<VkSubpassDependency, 2> dependencies{};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = kAttachments | kReaders;
        dependencies[0].srcAccessMask = kWrites;
        dependencies[0].dstStageMask = kAttachments;
        dependencies[0].dstAccessMask = kWrites | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].srcAccessMask = kWrites;
        dependencies[1].dstStageMask = kReaders | kAttachments;
        dependencies[1].dstAccessMask = kAll;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        renderPassInfo.pAttachments = attachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
        renderPassInfo.pDependencies = dependencies.data();
        return vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &out) == VK_SUCCESS;
    };
    return make(false, m_renderPass) && make(true, m_lateRenderPass);
}

bool GBufferPass::createFramebuffer() {
    std::array<VkImageView, GBUFFER_COUNT> attachments;
    for (uint32_t i = 0; i < GBUFFER_COUNT; ++i) {
        attachments[i] = m_gbuffer[i].view;
    }

    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_renderPass;
    framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    framebufferInfo.pAttachments = attachments.data();
    framebufferInfo.width = m_width;
    framebufferInfo.height = m_height;
    framebufferInfo.layers = 1;

    return vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffer) == VK_SUCCESS;
}

bool GBufferPass::createPipeline() {
    // Load shaders
    VkShaderModule vertShader = loadShaderModule("core_gbuffer.slang.vertexMain.spv");
    VkShaderModule fragShader = loadShaderModule("core_gbuffer.slang.opaque.spv");

    std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{};

    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertShader;
    shaderStages[0].pName = "vertexMain";

    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = fragShader;
    shaderStages[1].pName = "opaque";

    // Vertex input - matching model.hpp Vertex struct (uses render::vertexAttributes())
    auto bindingDescs = render::vertexBindings();
    auto attributeDescs = render::vertexAttributes();

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDescs.size());
    vertexInputInfo.pVertexBindingDescriptions = bindingDescs.data();
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescs.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescs.data();

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // Viewport (dynamic)
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // Rasterizer
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;  // No culling: imported meshes are not guaranteed consistently wound
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    // Multisampling
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth stencil
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    // Color blending (no blending for G-Buffer)
    std::array<VkPipelineColorBlendAttachmentState, GBUFFER_COUNT - 1> colorBlendAttachments{};
    for (auto& attachment : colorBlendAttachments) {
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        attachment.blendEnable = VK_FALSE;
    }

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = static_cast<uint32_t>(colorBlendAttachments.size());
    colorBlending.pAttachments = colorBlendAttachments.data();

    // Dynamic state
    std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

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

    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertShader, nullptr);
        vkDestroyShaderModule(m_device, fragShader, nullptr);
        return false;
    }

    // Create pipeline
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_renderPass;
    pipelineInfo.subpass = 0;

    VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                nullptr, &m_pipeline);

    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertShader, nullptr);
        vkDestroyShaderModule(m_device, fragShader, nullptr);
        return false;
    }

    // One-sided materials: back faces culled. Front faces are counter-clockwise as the
    // rasterizer sees them (glTF's convention; measured on the helmet: test 6, the goldens).
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_cullBackPipeline);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertShader, nullptr);
        vkDestroyShaderModule(m_device, fragShader, nullptr);
        return false;
    }

    // Alpha-tested materials: the same state with core/gbuffer.slang's `masked` (a discard; no
    // forced early depth test), [0] culling back faces, [1] none.
    if (VkShaderModule maskFrag = loadShaderModule("core_gbuffer.slang.masked.spv")) {
        shaderStages[1].module = maskFrag;
        shaderStages[1].pName = "masked";
        for (uint32_t k = 0; k < 2; ++k) {
            rasterizer.cullMode = k == 0 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
            if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_maskPipelines[k]) !=
                VK_SUCCESS)
                m_maskPipelines[k] = VK_NULL_HANDLE;
        }
        vkDestroyShaderModule(m_device, maskFrag, nullptr);
        shaderStages[1].module = fragShader;
        shaderStages[1].pName = "opaque";
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    }

    // The meshlet path: the same fragment shader and state, its own geometry stages.
    if (!createMeshletPipelines(pipelineInfo, rasterizer)) {
        vkDestroyShaderModule(m_device, vertShader, nullptr);
        vkDestroyShaderModule(m_device, fragShader, nullptr);
        return false;
    }

    // Create wireframe pipeline variant
    rasterizer.polygonMode = VK_POLYGON_MODE_LINE;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;

    result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                       nullptr, &m_wireframePipeline);

    vkDestroyShaderModule(m_device, vertShader, nullptr);
    vkDestroyShaderModule(m_device, fragShader, nullptr);

    // Wireframe pipeline is optional - don't fail if it doesn't create
    // (e.g., if wideLines feature isn't supported)
    return true;
}

bool GBufferPass::createMeshletPipelines(VkGraphicsPipelineCreateInfo base, VkPipelineRasterizationStateCreateInfo raster) {
    // base: the instance pipeline's state (its fragment stage second); [0] culls back faces
    // (front faces counter-clockwise, as the instance path), [1] culls none.
    const VkPipelineShaderStageCreateInfo frag = base.pStages[1];
    const auto stage = [](VkShaderStageFlagBits s, VkShaderModule m, const char* entry) {
        VkPipelineShaderStageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage = s;
        info.module = m;
        info.pName = entry;
        return info;
    };
    const auto build = [&](const VkPipelineShaderStageCreateInfo* stages, uint32_t count, bool vertexInput,
                           std::array<VkPipeline, 2>& out) {
        VkPipelineVertexInputStateCreateInfo noVertices{};
        noVertices.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkGraphicsPipelineCreateInfo info = base;
        info.stageCount = count;
        info.pStages = stages;
        // A mesh pipeline has no vertex input or input assembly; the pull shader reads no
        // attributes (a triangle list).
        info.pVertexInputState = vertexInput ? &noVertices : nullptr;
        if (!vertexInput) info.pInputAssemblyState = nullptr;
        for (uint32_t k = 0; k < 2; ++k) {
            VkPipelineRasterizationStateCreateInfo r = raster;
            r.polygonMode = VK_POLYGON_MODE_FILL;
            r.cullMode = k == 0 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
            r.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            info.pRasterizationState = &r;
            if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &out[k]) != VK_SUCCESS) return false;
        }
        return true;
    };

    VkShaderModule pull = loadShaderModule("cull_meshlets.slang.pull.spv");
    bool ok = pull != VK_NULL_HANDLE;
    if (ok) {
        const VkPipelineShaderStageCreateInfo stages[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, pull, "pull"), frag};
        ok = build(stages, 2, true, m_pullPipelines);
    }
    if (pull) vkDestroyShaderModule(m_device, pull, nullptr);
    if (!ok || !m_meshShaderSupported) return ok;

    VkShaderModule task = loadShaderModule("cull_meshlets.slang.task.spv");
    VkShaderModule mesh = loadShaderModule("cull_meshlets.slang.mesh.spv");
    ok = task != VK_NULL_HANDLE && mesh != VK_NULL_HANDLE;
    if (ok) {
        const VkPipelineShaderStageCreateInfo stages[3] = {stage(VK_SHADER_STAGE_TASK_BIT_EXT, task, "task"),
                                                           stage(VK_SHADER_STAGE_MESH_BIT_EXT, mesh, "mesh"), frag};
        ok = build(stages, 3, false, m_meshPipelines);
    }
    if (task) vkDestroyShaderModule(m_device, task, nullptr);
    if (mesh) vkDestroyShaderModule(m_device, mesh, nullptr);
    return ok;
}


} // namespace ohao
