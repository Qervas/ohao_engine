#include "renderer_impl.hpp"
#include <array>
#include <deque>
#include "scene/scene.hpp"
#include "scene/component/light_component.hpp"
#include "render/rt/gpu_light.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "stb_image.h"
#include "scene/actor/actor.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/component/material_component.hpp"
#include "scene/component/transform_component.hpp"
#include "render/camera/camera.hpp"
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

        // Store buffer info (designated MeshBufferInfo — core GpuPod)
        m_meshBufferMap[actor->getID()] = MeshBufferInfo{
            .vertexOffset = vertexOffset,
            .indexOffset = indexOffset,
            .indexCount = indexCount,
            .vertexCount = static_cast<uint32_t>(model->vertices.size()),
        };

        // Add vertices (span-friendly Model API)
        const auto verts = model->vertexSpan();
        combinedVertices.insert(combinedVertices.end(), verts.begin(), verts.end());

        // Add indices with offset
        for (uint32_t index : model->indexSpan()) {
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
