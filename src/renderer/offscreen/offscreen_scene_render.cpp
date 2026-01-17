#include "offscreen_renderer_impl.hpp"
#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/material_component.hpp"
#include "engine/asset/model.hpp"

namespace ohao {

bool OffscreenRenderer::updateSceneBuffers() {
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
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
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
        bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
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

    return true;
}

void OffscreenRenderer::renderSceneObjects() {
    if (!m_hasSceneMeshes || !m_scene || !m_pipeline) {
        return;
    }

    // Bind pipeline
    vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    // Set viewport
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_width);
    viewport.height = static_cast<float>(m_height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(m_commandBuffer, 0, 1, &viewport);

    // Set scissor
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {m_width, m_height};
    vkCmdSetScissor(m_commandBuffer, 0, 1, &scissor);

    // Bind descriptor sets
    vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    // Bind vertex and index buffers
    VkBuffer vertexBuffers[] = {m_vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(m_commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(m_commandBuffer, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    // Draw each actor
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
        pc.baseColor = glm::vec3(0.8f, 0.8f, 0.8f);  // Default gray
        pc.metallic = 0.0f;
        pc.roughness = 0.5f;
        pc.ao = 1.0f;

        // Get material properties from MaterialComponent
        auto materialComp = actor->getComponent<MaterialComponent>();
        if (materialComp) {
            const auto& mat = materialComp->getMaterial();
            pc.baseColor = mat.baseColor;
            pc.metallic = mat.metallic;
            pc.roughness = mat.roughness;
            pc.ao = mat.ao;
        }
        // Fallback: get from model materials
        else {
            auto model = meshComponent->getModel();
            if (model && !model->materials.empty()) {
                const auto& mat = model->materials.begin()->second;
                pc.baseColor = mat.diffuse;
            }
        }

        vkCmdPushConstants(m_commandBuffer, m_pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(ObjectPushConstants), &pc);

        // Draw indexed
        vkCmdDrawIndexed(m_commandBuffer, bufferInfo.indexCount, 1, bufferInfo.indexOffset, 0, 0);
    }
}

void OffscreenRenderer::renderShadowPass() {
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

    vkCmdBeginRenderPass(m_commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Bind shadow pipeline
    vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);

    // Set viewport to shadow map size
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(SHADOW_MAP_SIZE);
    viewport.height = static_cast<float>(SHADOW_MAP_SIZE);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(m_commandBuffer, 0, 1, &viewport);

    // Set scissor
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};
    vkCmdSetScissor(m_commandBuffer, 0, 1, &scissor);

    // Bind descriptor sets (for light buffer access in shadow vertex shader)
    vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_shadowPipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    // Bind vertex and index buffers
    VkBuffer vertexBuffers[] = {m_vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(m_commandBuffer, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(m_commandBuffer, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);

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
        pc.baseColor = glm::vec3(0.0f);  // Not used in shadow pass
        pc.metallic = 0.0f;
        pc.roughness = 0.0f;
        pc.ao = 0.0f;

        vkCmdPushConstants(m_commandBuffer, m_shadowPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(ObjectPushConstants), &pc);

        // Draw indexed
        vkCmdDrawIndexed(m_commandBuffer, bufferInfo.indexCount, 1, bufferInfo.indexOffset, 0, 0);
    }

    vkCmdEndRenderPass(m_commandBuffer);
    // Render pass finalLayout transitions shadow image to SHADER_READ_ONLY_OPTIMAL
}

} // namespace ohao
