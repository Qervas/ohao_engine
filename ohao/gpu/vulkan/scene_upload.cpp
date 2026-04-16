#include "renderer_impl.hpp"
#include <array>
#include <deque>
#include "render/camera/camera.hpp"
#include "scene/scene.hpp"
#include "scene/component/light_component.hpp"
#include "render/rt/gpu_light.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "stb_image.h"
#include "scene/actor/actor.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/component/material_component.hpp"
#include "scene/asset/model.hpp"
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include "render/deferred/deferred_renderer.hpp"

namespace ohao {

bool VulkanRenderer::updateSceneBuffers() {
    if (!m_scene) {
        std::cout << "Cannot update buffers - no scene exists" << std::endl;
        m_hasSceneMeshes = false;
        return false;
    }

    vkDeviceWaitIdle(m_device);

    // Clear old mappings
    m_meshBufferMap.clear();

    // First pass: Count actors with valid meshes
    size_t totalVertices = 0;
    size_t totalIndices = 0;

    std::vector<std::pair<Actor*, std::shared_ptr<Model>>> actorsWithModels;

    for (const auto& [actorId, actor] : m_scene->getAllActors()) {
        auto meshComponent = actor->getComponent<MeshComponent>();
        if (meshComponent && meshComponent->getModel() && meshComponent->isVisible()) {
            auto model = meshComponent->getModel();
            totalVertices += model->vertices.size();
            totalIndices += model->indices.size();
            actorsWithModels.push_back({actor.get(), model});
        }
    }

    if (actorsWithModels.empty()) {
        std::cout << "No actors with mesh components found in scene" << std::endl;
        m_hasSceneMeshes = false;
        return false;
    }

    // Pre-allocate buffers
    std::vector<Vertex> combinedVertices;
    std::vector<uint32_t> combinedIndices;
    combinedVertices.reserve(totalVertices);
    combinedIndices.reserve(totalIndices);

    // Second pass: Build combined buffers
    for (const auto& [actor, model] : actorsWithModels) {
        const uint32_t vertexOffset = static_cast<uint32_t>(combinedVertices.size());
        const uint32_t indexOffset = static_cast<uint32_t>(combinedIndices.size());
        const uint32_t indexCount = static_cast<uint32_t>(model->indices.size());

        // Store buffer info
        MeshBufferInfo bufferInfo{};
        bufferInfo.vertexOffset = vertexOffset;
        bufferInfo.vertexCount = static_cast<uint32_t>(model->vertices.size());
        bufferInfo.indexOffset = indexOffset;
        bufferInfo.indexCount = indexCount;
        m_meshBufferMap[actor->getID()] = bufferInfo;

        // Add vertices
        combinedVertices.insert(combinedVertices.end(), model->vertices.begin(), model->vertices.end());

        // Add indices with offset
        for (uint32_t index : model->indices) {
            combinedIndices.push_back(index + vertexOffset);
        }
    }

    if (combinedVertices.empty() || combinedIndices.empty()) {
        m_hasSceneMeshes = false;
        return false;
    }

    // Cleanup old buffers
    if (m_vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
        m_vertexBuffer = VK_NULL_HANDLE;
    }
    if (m_vertexBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_vertexBufferMemory, nullptr);
        m_vertexBufferMemory = VK_NULL_HANDLE;
    }
    if (m_indexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_indexBuffer, nullptr);
        m_indexBuffer = VK_NULL_HANDLE;
    }
    if (m_indexBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_indexBufferMemory, nullptr);
        m_indexBufferMemory = VK_NULL_HANDLE;
    }

    // Create vertex buffer
    VkDeviceSize vertexBufferSize = sizeof(Vertex) * combinedVertices.size();
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = vertexBufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_vertexBuffer) != VK_SUCCESS) {
            return false;
        }

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(m_device, m_vertexBuffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(m_physicalDevice, memRequirements.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_vertexBufferMemory) != VK_SUCCESS) {
            return false;
        }

        vkBindBufferMemory(m_device, m_vertexBuffer, m_vertexBufferMemory, 0);

        void* data;
        vkMapMemory(m_device, m_vertexBufferMemory, 0, vertexBufferSize, 0, &data);
        memcpy(data, combinedVertices.data(), vertexBufferSize);
        vkUnmapMemory(m_device, m_vertexBufferMemory);
    }

    // Create index buffer
    VkDeviceSize indexBufferSize = sizeof(uint32_t) * combinedIndices.size();
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = indexBufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_indexBuffer) != VK_SUCCESS) {
            return false;
        }

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(m_device, m_indexBuffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(m_physicalDevice, memRequirements.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_indexBufferMemory) != VK_SUCCESS) {
            return false;
        }

        vkBindBufferMemory(m_device, m_indexBuffer, m_indexBufferMemory, 0);

        void* data;
        vkMapMemory(m_device, m_indexBufferMemory, 0, indexBufferSize, 0, &data);
        memcpy(data, combinedIndices.data(), indexBufferSize);
        vkUnmapMemory(m_device, m_indexBufferMemory);
    }

    m_vertexCount = static_cast<uint32_t>(combinedVertices.size());
    m_indexCount = static_cast<uint32_t>(combinedIndices.size());
    m_hasSceneMeshes = true;

    std::cout << "Scene buffers updated: " << m_vertexCount << " vertices, "
              << m_indexCount << " indices across " << actorsWithModels.size() << " actors" << std::endl;

    // RT acceleration structures — uses separate device-local buffers (not the raster vertex/index buffers)
    m_rtAccelDirty = true;
    buildAccelerationStructures();

    // Cache emissive mesh lights for deferred pipeline (avoids per-frame texture scan)
    cacheEmissiveLights();

    return true;
}

void VulkanRenderer::renderSceneObjects(VkCommandBuffer cmd) {
    if (!m_hasSceneMeshes || !m_scene || !m_pipeline) {
        return;
    }

    // Draw each actor (pipeline/viewport/scissor/buffers already bound by caller)
    for (const auto& [actorId, actor] : m_scene->getAllActors()) {
        auto meshComponent = actor->getComponent<MeshComponent>();
        if (!meshComponent || !meshComponent->isVisible()) continue;

        auto it = m_meshBufferMap.find(actor->getID());
        if (it == m_meshBufferMap.end()) continue;

        const MeshBufferInfo& bufferInfo = it->second;
        if (bufferInfo.indexCount == 0) continue;

        // Setup push constants
        ObjectPushConstants pc{};
        pc.model = actor->getTransform()->getWorldMatrix();
        pc.viewProj = m_camera->getViewProjectionMatrix();
        pc.prevMVP = pc.viewProj * pc.model; // Fallback: use current if no motion history

        // Material defaults
        glm::vec3 baseColor(0.8f);
        float metallic = 0.0f;
        float roughness = 0.5f;

        // Default: no texture
        uint32_t albedoTexIdx = UINT32_MAX;
        uint32_t normalTexIdx = UINT32_MAX;
        uint32_t roughMetalTexIdx = UINT32_MAX;
        uint32_t emissiveTexIdx = UINT32_MAX;
        float emissiveStrength = 0.0f;

        // Get material properties from MaterialComponent
        auto materialComp = actor->getComponent<MaterialComponent>();
        if (materialComp) {
            const auto& mat = materialComp->getMaterial();
            baseColor = mat.baseColor;
            metallic = mat.metallic;
            roughness = mat.roughness;

            // Look up texture indices for bindless rendering
            if (m_textureManager) {
                if (mat.useAlbedoTexture && !mat.albedoTexture.empty()) {
                    auto handle = m_textureManager->getTextureByPath(mat.albedoTexture);
                    if (handle.valid()) albedoTexIdx = handle.index;
                }
                if (mat.useNormalTexture && !mat.normalTexture.empty()) {
                    auto handle = m_textureManager->getTextureByPath(mat.normalTexture);
                    if (handle.valid()) normalTexIdx = handle.index;
                }
                if (mat.useRoughnessTexture && !mat.roughnessTexture.empty()) {
                    auto handle = m_textureManager->getTextureByPath(mat.roughnessTexture);
                    if (handle.valid()) roughMetalTexIdx = handle.index;
                }
                if (mat.useEmissiveTexture && !mat.emissiveTexture.empty()) {
                    auto handle = m_textureManager->getTextureByPath(mat.emissiveTexture);
                    if (handle.valid()) emissiveTexIdx = handle.index;
                    emissiveStrength = glm::length(mat.emissive) > 0.01f ? 3.0f : 0.0f;
                }
            }
        }
        // Fallback: get from model materials
        else {
            auto model = meshComponent->getModel();
            if (model && !model->materials.empty()) {
                const auto& mat = model->materials.begin()->second;
                baseColor = mat.diffuse;
                // Add metallic/roughness from model if needed
            }
        }

        auto packIdx = [](uint32_t idx) -> float {
            float f; memcpy(&f, &idx, sizeof(float)); return f;
        };

        pc.materialParams = glm::vec4(metallic, roughness, packIdx(roughMetalTexIdx), packIdx(albedoTexIdx));
        pc.albedoColor = glm::vec4(baseColor, packIdx(normalTexIdx));
        pc.emissiveParams = glm::vec4(packIdx(emissiveTexIdx), emissiveStrength, 0.0f, 0.0f);

        vkCmdPushConstants(cmd, m_pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(ObjectPushConstants), &pc);

        // Draw indexed
        vkCmdDrawIndexed(cmd, bufferInfo.indexCount, 1, bufferInfo.indexOffset, 0, 0);
    }
}

void VulkanRenderer::renderShadowPass(VkCommandBuffer cmd, VkDescriptorSet descriptorSet) {
    static int shadowDebugCounter = 0;
    bool shouldPrintShadowDebug = (shadowDebugCounter++ % 120 == 0);

    if (!m_shadowsEnabled || !m_hasSceneMeshes || !m_scene || !m_shadowPipeline) {
        if (shouldPrintShadowDebug) {
            std::cout << "[ShadowPass] SKIPPED: shadowsEnabled=" << m_shadowsEnabled
                      << " hasSceneMeshes=" << m_hasSceneMeshes
                      << " scene=" << (m_scene != nullptr)
                      << " shadowPipeline=" << (m_shadowPipeline != VK_NULL_HANDLE) << std::endl;
        }
        return;
    }

    if (shouldPrintShadowDebug) {
        std::cout << "[ShadowPass] EXECUTING with " << m_meshBufferMap.size() << " meshes" << std::endl;
    }

    // Begin shadow render pass (render pass handles layout transitions via subpass dependencies)
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_shadowRenderPass;
    renderPassInfo.framebuffer = m_shadowFramebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};

    VkClearValue clearValue{};
    clearValue.depthStencil = {1.0f, 0};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Bind shadow pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);

    // Set viewport to shadow map size
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(SHADOW_MAP_SIZE);
    viewport.height = static_cast<float>(SHADOW_MAP_SIZE);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    // Set scissor
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Bind descriptor sets (for light buffer access in shadow vertex shader)
    // CRITICAL: Use the same descriptor set as the main pass for consistent light data!
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_shadowPipelineLayout, 0, 1, &descriptorSet, 0, nullptr);

    // Bind vertex and index buffers
    VkBuffer vertexBuffers[] = {m_vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    // Draw each actor (shadow casters only)
    for (const auto& [actorId, actor] : m_scene->getAllActors()) {
        auto meshComponent = actor->getComponent<MeshComponent>();
        if (!meshComponent || !meshComponent->isVisible()) continue;

        auto it = m_meshBufferMap.find(actor->getID());
        if (it == m_meshBufferMap.end()) continue;

        const MeshBufferInfo& bufferInfo = it->second;
        if (bufferInfo.indexCount == 0) continue;

        // Setup push constants (only model matrix matters for shadow pass)
        ObjectPushConstants pc{};
        pc.model = actor->getTransform()->getWorldMatrix();

        vkCmdPushConstants(cmd, m_shadowPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(ObjectPushConstants), &pc);

        // Draw indexed
        vkCmdDrawIndexed(cmd, bufferInfo.indexCount, 1, bufferInfo.indexOffset, 0, 0);
    }

    vkCmdEndRenderPass(cmd);
    // Render pass finalLayout transitions shadow image to SHADER_READ_ONLY_OPTIMAL
}

void VulkanRenderer::buildAccelerationStructures() {
    if (!m_rtAccel || !m_rtAccel->isSupported()) return;
    if (!m_hasSceneMeshes || !m_scene) return;
    if (m_vertexCount == 0 || m_indexCount == 0) return;

    createRTVertexIndexBuffers();
    createRTNormalUVBuffers();
    uploadRTMaterialBuffers();
    uploadRTTextureArray();
    uploadDeferredTextures();
    uploadLightBuffer();
    buildBLASTLAS();

    m_rtAccelDirty = false;
}

} // namespace ohao
