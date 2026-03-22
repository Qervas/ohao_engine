#include "offscreen_renderer_impl.hpp"
#include "engine/scene/scene.hpp"
#include "engine/actor/actor.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/material_component.hpp"
#include "engine/asset/model.hpp"
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include "renderer/passes/deferred_renderer.hpp"

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

    return true;
}

void OffscreenRenderer::renderSceneObjects(VkCommandBuffer cmd) {
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
        pc.baseColor = glm::vec3(0.8f, 0.8f, 0.8f);  // Default gray
        pc.metallic = 0.0f;
        pc.roughness = 0.5f;
        pc.ao = 1.0f;

        // Default: no texture
        uint32_t albedoTexIdx = UINT32_MAX;
        uint32_t normalTexIdx = UINT32_MAX;

        // Get material properties from MaterialComponent
        auto materialComp = actor->getComponent<MaterialComponent>();
        if (materialComp) {
            const auto& mat = materialComp->getMaterial();
            pc.baseColor = mat.baseColor;
            pc.metallic = mat.metallic;
            pc.roughness = mat.roughness;
            pc.ao = mat.ao;

            // Look up texture indices for bindless rendering
            if (m_textureManager && mat.useAlbedoTexture && !mat.albedoTexture.empty()) {
                auto handle = m_textureManager->getTextureByPath(mat.albedoTexture);
                if (handle.valid()) albedoTexIdx = handle.index;
            }
            if (m_textureManager && mat.useNormalTexture && !mat.normalTexture.empty()) {
                auto handle = m_textureManager->getTextureByPath(mat.normalTexture);
                if (handle.valid()) normalTexIdx = handle.index;
            }
        }
        // Fallback: get from model materials
        else {
            auto model = meshComponent->getModel();
            if (model && !model->materials.empty()) {
                const auto& mat = model->materials.begin()->second;
                pc.baseColor = mat.diffuse;
            }
        }

        // Pack texture indices as float bits
        memcpy(&pc.albedoTexIdx, &albedoTexIdx, sizeof(float));
        memcpy(&pc.normalTexIdx, &normalTexIdx, sizeof(float));

        vkCmdPushConstants(cmd, m_pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(ObjectPushConstants), &pc);

        // Draw indexed
        vkCmdDrawIndexed(cmd, bufferInfo.indexCount, 1, bufferInfo.indexOffset, 0, 0);
    }
}

void OffscreenRenderer::renderShadowPass(VkCommandBuffer cmd, VkDescriptorSet descriptorSet) {
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
        pc.baseColor = glm::vec3(0.0f);  // Not used in shadow pass
        pc.metallic = 0.0f;
        pc.roughness = 0.0f;
        pc.ao = 0.0f;

        vkCmdPushConstants(cmd, m_shadowPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(ObjectPushConstants), &pc);

        // Draw indexed
        vkCmdDrawIndexed(cmd, bufferInfo.indexCount, 1, bufferInfo.indexOffset, 0, 0);
    }

    vkCmdEndRenderPass(cmd);
    // Render pass finalLayout transitions shadow image to SHADER_READ_ONLY_OPTIMAL
}

void OffscreenRenderer::buildAccelerationStructures() {
    if (!m_rtAccel || !m_rtAccel->isSupported()) return;
    if (!m_hasSceneMeshes || !m_scene) return;
    if (m_vertexCount == 0 || m_indexCount == 0) return;

    // Destroy old RT buffers
    if (m_rtVertexBuffer) { vkDestroyBuffer(m_device, m_rtVertexBuffer, nullptr); m_rtVertexBuffer = VK_NULL_HANDLE; }
    if (m_rtVertexMemory) { vkFreeMemory(m_device, m_rtVertexMemory, nullptr); m_rtVertexMemory = VK_NULL_HANDLE; }
    if (m_rtIndexBuffer) { vkDestroyBuffer(m_device, m_rtIndexBuffer, nullptr); m_rtIndexBuffer = VK_NULL_HANDLE; }
    if (m_rtIndexMemory) { vkFreeMemory(m_device, m_rtIndexMemory, nullptr); m_rtIndexMemory = VK_NULL_HANDLE; }

    // Create device-local RT buffers with proper flags
    auto createRTBuffer = [&](VkDeviceSize size, VkBuffer& buf, VkDeviceMemory& mem,
                              VkBuffer srcBuf) -> bool {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = size;
        bufInfo.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(m_device, &bufInfo, nullptr, &buf) != VK_SUCCESS) return false;

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(m_device, buf, &memReqs);

        VkMemoryAllocateFlagsInfo allocFlags{};
        allocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        allocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.pNext = &allocFlags;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(m_physicalDevice, memReqs.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX) return false;
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &mem) != VK_SUCCESS) return false;
        vkBindBufferMemory(m_device, buf, mem, 0);

        // Copy data from raster buffer to RT buffer
        VkCommandBuffer cmd;
        VkCommandBufferAllocateInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdInfo.commandPool = m_commandPool;
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_device, &cmdInfo, &cmd);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        VkBufferCopy copyRegion{};
        copyRegion.size = size;
        vkCmdCopyBuffer(cmd, srcBuf, buf, 1, &copyRegion);

        vkEndCommandBuffer(cmd);
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_graphicsQueue);
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
        return true;
    };

    VkDeviceSize vertexSize = m_vertexCount * sizeof(Vertex);
    VkDeviceSize indexSize = m_indexCount * sizeof(uint32_t);

    if (!createRTBuffer(vertexSize, m_rtVertexBuffer, m_rtVertexMemory, m_vertexBuffer)) {
        std::cerr << "[RT] Failed to create RT vertex buffer" << std::endl;
        return;
    }
    if (!createRTBuffer(indexSize, m_rtIndexBuffer, m_rtIndexMemory, m_indexBuffer)) {
        std::cerr << "[RT] Failed to create RT index buffer" << std::endl;
        return;
    }

    // Create normal buffer — extract normals from host-visible vertex buffer
    {
        if (m_rtNormalBuffer) { vkDestroyBuffer(m_device, m_rtNormalBuffer, nullptr); m_rtNormalBuffer = VK_NULL_HANDLE; }
        if (m_rtNormalMemory) { vkFreeMemory(m_device, m_rtNormalMemory, nullptr); m_rtNormalMemory = VK_NULL_HANDLE; }

        // Read vertex data from host-visible buffer to extract normals
        VkDeviceSize normalBufSize = m_vertexCount * sizeof(glm::vec4);
        std::vector<glm::vec4> normals(m_vertexCount);

        void* vertMapped;
        vkMapMemory(m_device, m_vertexBufferMemory, 0, vertexSize, 0, &vertMapped);
        const Vertex* verts = static_cast<const Vertex*>(vertMapped);
        for (uint32_t i = 0; i < m_vertexCount; i++) {
            normals[i] = glm::vec4(verts[i].normal, 0.0f);
        }
        vkUnmapMemory(m_device, m_vertexBufferMemory);

        // Create device-local normal buffer
        VkBufferCreateInfo nbInfo{};
        nbInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        nbInfo.size = normalBufSize;
        nbInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCreateBuffer(m_device, &nbInfo, nullptr, &m_rtNormalBuffer);

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(m_device, m_rtNormalBuffer, &memReqs);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(m_physicalDevice, memReqs.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(m_device, &allocInfo, nullptr, &m_rtNormalMemory);
        vkBindBufferMemory(m_device, m_rtNormalBuffer, m_rtNormalMemory, 0);

        void* normalMapped;
        vkMapMemory(m_device, m_rtNormalMemory, 0, normalBufSize, 0, &normalMapped);
        memcpy(normalMapped, normals.data(), normalBufSize);
        vkUnmapMemory(m_device, m_rtNormalMemory);

        std::cout << "[RT] Normal buffer created: " << m_vertexCount << " normals" << std::endl;
    }

    // Rebuild acceleration structures using the RT buffers
    m_rtAccel->destroy();
    m_rtAccel->init(m_device, m_physicalDevice, m_graphicsQueue, m_graphicsQueueFamily, m_commandPool);

    std::unordered_map<uint64_t, BlasHandle> actorBlas;
    for (const auto& [actorId, actor] : m_scene->getAllActors()) {
        auto it = m_meshBufferMap.find(actorId);
        if (it == m_meshBufferMap.end()) continue;
        const MeshBufferInfo& meshInfo = it->second;
        if (meshInfo.indexCount == 0) continue;

        VkDeviceSize vertexByteOffset = meshInfo.vertexOffset * sizeof(Vertex);
        VkDeviceSize indexByteOffset = meshInfo.indexOffset * sizeof(uint32_t);

        std::cout << "[RT] BLAS for " << actor->getName()
                  << ": vOff=" << meshInfo.vertexOffset << " (" << vertexByteOffset << " bytes)"
                  << " iOff=" << meshInfo.indexOffset << " (" << indexByteOffset << " bytes)"
                  << " vCount=" << meshInfo.vertexCount << " iCount=" << meshInfo.indexCount << std::endl;

        BlasHandle blas = m_rtAccel->createBLAS(
            m_rtVertexBuffer, meshInfo.vertexCount, sizeof(Vertex),
            m_rtIndexBuffer, meshInfo.indexCount,
            vertexByteOffset, indexByteOffset, VK_NULL_HANDLE);

        if (blas != INVALID_BLAS) actorBlas[actorId] = blas;
    }

    // Build TLAS instances + collect materials in the SAME order
    m_rtAccel->clearInstances();
    std::vector<glm::vec3> materialAlbedos;
    std::vector<glm::vec4> materialFullData;
    uint32_t instanceIdx = 0;
    for (const auto& [actorId, actor] : m_scene->getAllActors()) {
        auto blasIt = actorBlas.find(actorId);
        if (blasIt == actorBlas.end()) continue;
        // Vertices are in LOCAL space (unit cube). TLAS transform places them in world space.
        m_rtAccel->addInstance(blasIt->second, actor->getTransform()->getWorldMatrix(), instanceIdx);

        // Collect albedo in same order
        auto matComp = actor->getComponent<MaterialComponent>();
        glm::vec3 albedo(0.73f);
        float roughness = 0.95f;
        float metallic = 0.0f;
        if (matComp) {
            albedo = matComp->getMaterial().baseColor;
            roughness = matComp->getMaterial().roughness;
            metallic = matComp->getMaterial().metallic;
        }
        // Detect sphere vs cube from mesh vertex count
        // Sphere meshes have many more vertices than 24 (cube = 24)
        bool isSphereShape = false;
        auto meshComp2 = actor->getComponent<MeshComponent>();
        if (meshComp2 && meshComp2->getModel()) {
            isSphereShape = meshComp2->getModel()->vertices.size() > 100;
        }
        // Pack into vec4: (r, g, b, packed)
        // packed encoding: sign = metallic, magnitude = roughness
        // Add 10.0 if sphere shape (roughness is always < 1, so 10+ means sphere)
        float packed = metallic > 0.5f ? -(roughness + 0.001f) : roughness;
        if (isSphereShape) packed += (packed >= 0 ? 10.0f : -10.0f);
        materialAlbedos.push_back(glm::vec3(albedo.r, albedo.g, albedo.b));
        // Store full material data for path tracer
        materialFullData.push_back(glm::vec4(albedo, packed));
        std::cout << "[RT] Instance " << instanceIdx << " (" << actor->getName()
                  << "): albedo=(" << albedo.r << "," << albedo.g << "," << albedo.b
                  << ") rough=" << roughness << " metal=" << metallic << std::endl;
        instanceIdx++;
    }

    if (m_rtAccel->getInstanceCount() > 0) {
        m_rtAccel->buildTLAS(VK_NULL_HANDLE);
        std::cout << "[RT] Acceleration structures built: "
                  << m_rtAccel->getBlasCount() << " BLAS, "
                  << m_rtAccel->getInstanceCount() << " instances" << std::endl;

        // Pass material albedos to RT techniques
        if (m_deferredRenderer) {
            auto* gi = m_deferredRenderer->getRT_GI();
            if (gi) gi->setMaterialAlbedos(materialAlbedos);
        }
        if (m_pathTracer) {
            m_pathTracer->setMaterialData(materialFullData);
            if (m_rtNormalBuffer != VK_NULL_HANDLE && m_rtIndexBuffer != VK_NULL_HANDLE) {
                m_pathTracer->setNormalBuffer(m_rtNormalBuffer, m_rtIndexBuffer, m_vertexCount);
            }
        }
    }
    m_rtAccelDirty = false;
}

} // namespace ohao
