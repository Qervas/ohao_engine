#include "csm_pass.hpp"
#include "render/scene/vertex_input.hpp"
#include "render/shader_bindings/shadow_shadow_csm.hpp"
#include "gpu_culling.hpp"
#include "render/graph/pipeline.hpp"
#include "gpu/shared_layout.hpp"
#include "core/log.hpp"
#include <cstring>
#include "render/scene/render_scene.hpp"
#include "asset/vertex.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <array>
#include <span>

namespace ohao {

namespace {
constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;
constexpr float kDepthBiasConstant = 1.25f;
constexpr float kDepthBiasSlope = 1.75f;
} // namespace

CSMPass::~CSMPass() {
    cleanup();
}

bool CSMPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    if (!m_allocator) return false;
    if (!createShadowMap()) return false;
    if (!createShadowSampler()) return false;
    if (!createCascadeBuffer()) return false;
    if (!createSceneDescriptors()) return false;
    if (!createPipeline()) return false;

    // Initialize cascade splits
    calculateCascadeSplits();

    return true;
}

void CSMPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    safeDestroy(m_scenePool);   // frees the sets
    m_sceneSets.fill(VK_NULL_HANDLE);
    safeDestroy(m_sceneLayout);
    safeDestroy(m_shadowSampler);
    if (m_allocator) {
        m_allocator->retire(m_cascadeBuffer);
        m_allocator->retire(m_shadowMap);
    }
}

void CSMPass::record(const render::Frame& frame) {
    render::Timed timed(frame, "csm");
    const uint32_t frameIndex = frame.slot;
    if (!m_renderScene || frameIndex >= MAX_FRAMES_IN_FLIGHT || !m_shadowMap || m_pipeline == VK_NULL_HANDLE) return;
    const VkCommandBuffer cmd = frame.cmd;

    // Update cascade matrices based on current camera
    updateCascadeMatrices();

    // Update cascade buffer
    memcpy(m_cascadeBuffer.getMappedData(), &m_cascadeData, sizeof(CascadeData));
    m_allocator->flushBuffer(m_cascadeBuffer);

    // The slot's set (instance table, draw table), rewritten every frame: the buffers can be
    // replaced by growth; the slot's previous use finished at its fence wait.
    const VkBuffer instances = m_renderScene->instanceBuffer();
    const VkBuffer drawTable = m_renderScene->drawTable();
    const bool haveDraws = m_culling && m_culling->drawCount() > 0 && instances != VK_NULL_HANDLE &&
                           drawTable != VK_NULL_HANDLE && m_renderScene->vertexArena() != VK_NULL_HANDLE &&
                           m_renderScene->indexArena() != VK_NULL_HANDLE && m_sceneSets[frameIndex] != VK_NULL_HANDLE;
    if (haveDraws) {
        const VkDescriptorBufferInfo infos[2] = {{instances, 0, VK_WHOLE_SIZE}, {drawTable, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[2]{shader::write(shader::shadow_shadow_csm::instances, m_sceneSets[frameIndex]),
                                       shader::write(shader::shadow_shadow_csm::draws, m_sceneSets[frameIndex])};
        writes[0].pBufferInfo = &infos[0];
        writes[1].pBufferInfo = &infos[1];
        vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
        // Every cascade's list, before its draws.
        for (uint32_t cascade = 0; cascade < m_cascades; ++cascade)
            m_culling->cull(frame, gpu::shared::kCullPhaseShadow, gpu::shared::kCullListCascade0 + cascade, 1,
                            m_cascadeData.viewProj[cascade], glm::vec3(0.0f));
    }

    // Render each cascade into its layer
    for (uint32_t cascade = 0; cascade < m_cascades; ++cascade) {
        render::Pass pass(frame, nullptr, render::Pass::Raster);
        // LEGACY(step 4a): the instance and draw tables and the vertex and index arenas are RenderScene's, not
        // tracked yet.
        if (haveDraws) m_culling->declareDraws(pass, false);
        pass.depth(m_shadowMap, render::Load::Clear, 1.0f, cascade);
        pass.begin();

        // The cascade's culled list (every draw on the instance path; shadows draw both faces)
        if (haveDraws) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
            vkCmdSetDepthBias(cmd, kDepthBiasConstant, 0.0f, kDepthBiasSlope);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                                    0, 1, &m_sceneSets[frameIndex], 0, nullptr);
            const VkBuffer vertexBuffer = m_renderScene->vertexArena();
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, m_renderScene->indexArena(), 0, VK_INDEX_TYPE_UINT32);

            const ShadowPushConstant push{m_cascadeData.viewProj[cascade]};
            vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(ShadowPushConstant), &push);
            // firstInstance = the draw record: the shader reads its instance through it.
            m_culling->drawList(cmd, gpu::shared::kCullListCascade0 + cascade);
        }
    }
}

bool CSMPass::configure(uint32_t cascades, uint32_t size) {
    m_cascades = std::clamp(cascades, 1u, CASCADE_COUNT);
    calculateCascadeSplits();
    size = std::clamp(size, 512u, 4096u);
    if (size == m_size || m_device == VK_NULL_HANDLE) {
        m_size = size;
        return false;
    }
    m_allocator->retire(m_shadowMap);   // frames in flight sample and render the old map
    m_size = size;
    if (!createShadowMap()) OHAO_LOG(Error, render, "CSM: the {}x{} shadow map could not be created", m_size, m_size);
    return true;
}

void CSMPass::setCameraData(const glm::mat4& view, const glm::mat4& proj,
                            float nearPlane, float farPlane) {
    m_cameraView = view;
    m_cameraProj = proj;
    m_nearPlane = nearPlane;
    m_farPlane = farPlane;
    calculateCascadeSplits();
}

bool CSMPass::createShadowMap() {
    m_shadowMap = m_allocator->createImage({"csm.shadow", kShadowFormat, m_size, m_size,
                                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1,
                                            CASCADE_COUNT});
    return static_cast<bool>(m_shadowMap);
}

bool CSMPass::createShadowSampler() {
    // Create shadow sampler with comparison
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // Outside = no shadow
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    return vkCreateSampler(m_device, &samplerInfo, nullptr, &m_shadowSampler) == VK_SUCCESS;
}

bool CSMPass::createCascadeBuffer() {
    m_cascadeBuffer =
        m_allocator->createBuffer(sizeof(CascadeData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, AllocationUsage::CpuToGpu, true);
    return m_cascadeBuffer.isValid() && m_cascadeBuffer.getMappedData() != nullptr;
}

bool CSMPass::createSceneDescriptors() {
    // The instance table and the draw table.
    const auto bindings = shader::layoutBindings(shader::shadow_shadow_csm::kSet0, VK_SHADER_STAGE_VERTEX_BIT);
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_sceneLayout) != VK_SUCCESS) return false;

    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * MAX_FRAMES_IN_FLIGHT};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_scenePool) != VK_SUCCESS) return false;

    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(m_sceneLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_scenePool;
    allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocInfo.pSetLayouts = layouts.data();
    return vkAllocateDescriptorSets(m_device, &allocInfo, m_sceneSets.data()) == VK_SUCCESS;
}

bool CSMPass::createPipeline() {
    // Push constant: the cascade's view-projection
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(ShadowPushConstant);

    // Set 0: the instance table
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_sceneLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) return false;

    // Depth-only, vertex stage only: each cascade is its own pass into its own layer, so the vertex shader
    // projects straight into that cascade. Full vertex stride: the scene's vertex arena is shared.
    const auto bindings = render::vertexBindings();
    const auto attributes = render::vertexAttributes();
    render::GraphicsPipelineDesc desc{};
    desc.name = "csm";
    desc.vertex = "shadow_shadow_csm.slang.spv";
    desc.layout = m_pipelineLayout;
    desc.depthFormat = kShadowFormat;
    desc.vertexBindings = bindings;
    desc.vertexAttributes = attributes;
    desc.cull = VK_CULL_MODE_FRONT_BIT;   // front-face culling for shadow
    desc.depthTest = true;
    desc.depthWrite = true;
    desc.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
    desc.depthClamp = true;               // clamp depth to [0, 1]
    desc.depthBias = true;                // kDepthBiasConstant, kDepthBiasSlope
    m_pipeline = render::createGraphicsPipeline(m_device, desc);
    return m_pipeline != VK_NULL_HANDLE;
}

void CSMPass::calculateCascadeSplits() {
    float range = m_farPlane - m_nearPlane;
    float ratio = m_farPlane / m_nearPlane;

    m_cascadeSplits[0] = m_nearPlane;

    for (uint32_t i = 1; i <= m_cascades; ++i) {
        float p = static_cast<float>(i) / static_cast<float>(m_cascades);

        // Logarithmic split
        float log = m_nearPlane * std::pow(ratio, p);

        // Linear split
        float linear = m_nearPlane + range * p;

        // Blend between log and linear
        float d = m_splitLambda * log + (1.0f - m_splitLambda) * linear;

        m_cascadeSplits[i] = d;
    }
    for (uint32_t i = m_cascades + 1; i <= CASCADE_COUNT; ++i) m_cascadeSplits[i] = m_farPlane;   // unused
    m_cascadeData.cascadeCount = m_cascades;

    // Store split depths (view space) for shader
    m_cascadeData.splitDepths = glm::vec4(
        m_cascadeSplits[1],
        m_cascadeSplits[2],
        m_cascadeSplits[3],
        m_cascadeSplits[4]
    );
}

void CSMPass::updateCascadeMatrices() {
    glm::mat4 invCameraView = glm::inverse(m_cameraView);

    for (uint32_t cascade = 0; cascade < m_cascades; ++cascade) {
        m_cascadeData.viewProj[cascade] = calculateLightViewProj(cascade);
    }
}

glm::mat4 CSMPass::calculateLightViewProj(uint32_t cascade) {
    float nearSplit = m_cascadeSplits[cascade];
    float farSplit = m_cascadeSplits[cascade + 1];

    // Get frustum corners in world space
    glm::mat4 invViewProj = glm::inverse(m_cameraProj * m_cameraView);

    std::array<glm::vec3, 8> frustumCorners;
    uint32_t idx = 0;

    for (int x = 0; x < 2; ++x) {
        for (int y = 0; y < 2; ++y) {
            for (int z = 0; z < 2; ++z) {
                glm::vec4 pt = invViewProj * glm::vec4(
                    2.0f * x - 1.0f,
                    2.0f * y - 1.0f,
                    z == 0 ? 0.0f : 1.0f, // NDC depth [0, 1] in Vulkan
                    1.0f
                );
                frustumCorners[idx++] = glm::vec3(pt) / pt.w;
            }
        }
    }

    // Adjust frustum corners to cascade split range
    for (uint32_t i = 0; i < 4; ++i) {
        glm::vec3 dist = frustumCorners[i + 4] - frustumCorners[i];
        frustumCorners[i + 4] = frustumCorners[i] + dist * (farSplit - m_nearPlane) / (m_farPlane - m_nearPlane);
        frustumCorners[i] = frustumCorners[i] + dist * (nearSplit - m_nearPlane) / (m_farPlane - m_nearPlane);
    }

    // Calculate frustum center
    glm::vec3 center(0.0f);
    for (const auto& corner : frustumCorners) {
        center += corner;
    }
    center /= 8.0f;

    // Calculate bounding sphere radius for stable shadow edges
    float radius = 0.0f;
    for (const auto& corner : frustumCorners) {
        float dist = glm::length(corner - center);
        radius = std::max(radius, dist);
    }

    // Round to texel size for stable shadows
    float texelsPerUnit = static_cast<float>(m_size) / (radius * 2.0f);
    glm::mat4 scaleBias = glm::scale(glm::mat4(1.0f), glm::vec3(texelsPerUnit));

    // Light view matrix
    glm::vec3 up = std::abs(glm::dot(m_lightDirection, glm::vec3(0, 1, 0))) > 0.99f ?
                   glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    glm::mat4 lightView = glm::lookAt(center - m_lightDirection * radius, center, up);

    // Snap to texel grid
    lightView = scaleBias * lightView;
    glm::vec4 shadowOrigin = lightView * glm::vec4(0, 0, 0, 1);
    shadowOrigin.x = std::round(shadowOrigin.x);
    shadowOrigin.y = std::round(shadowOrigin.y);
    glm::vec4 roundedOrigin = shadowOrigin - lightView * glm::vec4(0, 0, 0, 1);
    lightView[3][0] += roundedOrigin.x;
    lightView[3][1] += roundedOrigin.y;
    lightView = glm::inverse(scaleBias) * lightView;

    // Orthographic projection to Vulkan's depth range [0, 1] (glm::ortho is OpenGL's [-1, 1]:
    // the near half of the cascade came out below 0 and the lighting pass read it as unshadowed).
    glm::mat4 lightProj = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, radius * 2.0f);

    return lightProj * lightView;
}

} // namespace ohao
