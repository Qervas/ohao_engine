#include "axis_gizmo.hpp"
#include "renderer/vulkan_context.hpp"
#include "rhi/vk/ohao_vk_buffer.hpp"
#include <stdexcept>

namespace ohao {

bool AxisGizmo::initialize(VulkanContext* contextPtr) {
    context = contextPtr;

    createGizmoGeometry();
    createGridGeometry();
    if (!createBuffers()) {
        return false;
    }

    return true;
}

void AxisGizmo::cleanup() {
    vertexBuffer.reset();
    indexBuffer.reset();
    gridVertexBuffer.reset();
    gridIndexBuffer.reset();
    vertices.clear();
    indices.clear();
    gridVertices.clear();
    gridIndices.clear();
}

void AxisGizmo::createGizmoGeometry() {
    vertices.clear();
    indices.clear();
    
    uint32_t indexOffset = 0;
    
    // X axis (Red) - Main line
    vertices.push_back({{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0, 0, 1}, {0, 0}});
    vertices.push_back({{AXIS_LENGTH, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0, 0, 1}, {1, 0}});
    indices.push_back(indexOffset++);
    indices.push_back(indexOffset++);
    
    // X axis ruler markings
    for (int i = 1; i <= RULER_DIVISIONS; ++i) {
        float pos = (AXIS_LENGTH / RULER_DIVISIONS) * i;
        float markSize = (i % RULER_DIVISIONS == 0) ? 0.2f : 0.1f; // Larger mark at the end
        
        // Vertical mark
        vertices.push_back({{pos, -markSize, 0.0f}, {1.0f, 0.4f, 0.4f}, {0, 0, 1}, {0, 0}});
        vertices.push_back({{pos, markSize, 0.0f}, {1.0f, 0.4f, 0.4f}, {0, 0, 1}, {1, 0}});
        indices.push_back(indexOffset++);
        indices.push_back(indexOffset++);
    }

    // Y axis (Green) - Main line
    vertices.push_back({{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0, 0, 1}, {0, 0}});
    vertices.push_back({{0.0f, AXIS_LENGTH, 0.0f}, {0.0f, 1.0f, 0.0f}, {0, 0, 1}, {1, 0}});
    indices.push_back(indexOffset++);
    indices.push_back(indexOffset++);
    
    // Y axis ruler markings
    for (int i = 1; i <= RULER_DIVISIONS; ++i) {
        float pos = (AXIS_LENGTH / RULER_DIVISIONS) * i;
        float markSize = (i % RULER_DIVISIONS == 0) ? 0.2f : 0.1f;
        
        // Horizontal mark
        vertices.push_back({{-markSize, pos, 0.0f}, {0.4f, 1.0f, 0.4f}, {0, 0, 1}, {0, 0}});
        vertices.push_back({{markSize, pos, 0.0f}, {0.4f, 1.0f, 0.4f}, {0, 0, 1}, {1, 0}});
        indices.push_back(indexOffset++);
        indices.push_back(indexOffset++);
    }

    // Z axis (Blue) - Main line
    vertices.push_back({{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0, 1, 0}, {0, 0}});
    vertices.push_back({{0.0f, 0.0f, AXIS_LENGTH}, {0.0f, 0.0f, 1.0f}, {0, 1, 0}, {1, 0}});
    indices.push_back(indexOffset++);
    indices.push_back(indexOffset++);
    
    // Z axis ruler markings
    for (int i = 1; i <= RULER_DIVISIONS; ++i) {
        float pos = (AXIS_LENGTH / RULER_DIVISIONS) * i;
        float markSize = (i % RULER_DIVISIONS == 0) ? 0.2f : 0.1f;
        
        // Horizontal mark in XZ plane
        vertices.push_back({{-markSize, 0.0f, pos}, {0.4f, 0.4f, 1.0f}, {0, 1, 0}, {0, 0}});
        vertices.push_back({{markSize, 0.0f, pos}, {0.4f, 0.4f, 1.0f}, {0, 1, 0}, {1, 0}});
        indices.push_back(indexOffset++);
        indices.push_back(indexOffset++);
    }
}

void AxisGizmo::createGridGeometry() {
    gridVertices.clear();
    gridIndices.clear();
    
    uint32_t indexOffset = 0;
    
    // Create grid lines on XZ plane (Y=0)
    for (int i = 0; i <= (int)(GRID_SIZE * 2 / GRID_SPACING); ++i) {
        float pos = -GRID_SIZE + i * GRID_SPACING;
        float alpha = (i % 5 == 0) ? 0.4f : 0.15f; // Stronger lines every 5 units
        
        // Lines parallel to X axis (varying Z)
        gridVertices.push_back({{-GRID_SIZE, 0.0f, pos}, {0.6f, 0.6f, 0.6f}, {0, 1, 0}, {0, 0}});
        gridVertices.push_back({{GRID_SIZE, 0.0f, pos}, {0.6f, 0.6f, 0.6f}, {0, 1, 0}, {1, 0}});
        gridIndices.push_back(indexOffset++);
        gridIndices.push_back(indexOffset++);
        
        // Lines parallel to Z axis (varying X)
        gridVertices.push_back({{pos, 0.0f, -GRID_SIZE}, {0.6f, 0.6f, 0.6f}, {0, 1, 0}, {0, 0}});
        gridVertices.push_back({{pos, 0.0f, GRID_SIZE}, {0.6f, 0.6f, 0.6f}, {0, 1, 0}, {1, 0}});
        gridIndices.push_back(indexOffset++);
        gridIndices.push_back(indexOffset++);
    }
}

bool AxisGizmo::createBuffers() {
    try {
        // Create axis vertex buffer
        if (!vertices.empty()) {
            VkDeviceSize vertexBufferSize = sizeof(vertices[0]) * vertices.size();
            vertexBuffer = std::make_unique<OhaoVkBuffer>();
            vertexBuffer->initialize(context->getLogicalDevice());

            if (!OhaoVkBuffer::createWithStaging(
                context->getLogicalDevice(),
                context->getVkCommandPool(),
                vertices.data(),
                vertexBufferSize,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                *vertexBuffer)) {
                throw std::runtime_error("Failed to create axis gizmo vertex buffer");
            }
        }

        // Create axis index buffer
        if (!indices.empty()) {
            VkDeviceSize indexBufferSize = sizeof(indices[0]) * indices.size();
            indexBuffer = std::make_unique<OhaoVkBuffer>();
            indexBuffer->initialize(context->getLogicalDevice());

            if (!OhaoVkBuffer::createWithStaging(
                context->getLogicalDevice(),
                context->getVkCommandPool(),
                indices.data(),
                indexBufferSize,
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                *indexBuffer)) {
                throw std::runtime_error("Failed to create axis gizmo index buffer");
            }
        }
        
        // Create grid vertex buffer
        if (!gridVertices.empty()) {
            VkDeviceSize gridVertexBufferSize = sizeof(gridVertices[0]) * gridVertices.size();
            gridVertexBuffer = std::make_unique<OhaoVkBuffer>();
            gridVertexBuffer->initialize(context->getLogicalDevice());

            if (!OhaoVkBuffer::createWithStaging(
                context->getLogicalDevice(),
                context->getVkCommandPool(),
                gridVertices.data(),
                gridVertexBufferSize,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                *gridVertexBuffer)) {
                throw std::runtime_error("Failed to create grid vertex buffer");
            }
        }

        // Create grid index buffer
        if (!gridIndices.empty()) {
            VkDeviceSize gridIndexBufferSize = sizeof(gridIndices[0]) * gridIndices.size();
            gridIndexBuffer = std::make_unique<OhaoVkBuffer>();
            gridIndexBuffer->initialize(context->getLogicalDevice());

            if (!OhaoVkBuffer::createWithStaging(
                context->getLogicalDevice(),
                context->getVkCommandPool(),
                gridIndices.data(),
                gridIndexBufferSize,
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                *gridIndexBuffer)) {
                throw std::runtime_error("Failed to create grid index buffer");
            }
        }

        return true;
    }
    catch (const std::exception& e) {
        cleanup();
        return false;
    }
}

void AxisGizmo::render(VkCommandBuffer cmdBuffer, const glm::mat4& viewProj) {
    if (!isVisible && !isGridVisible) return;
    
    // Draw grid first (so it appears behind axes)
    if (isGridVisible && gridVertexBuffer && gridIndexBuffer) {
        VkBuffer gridVBuffer = gridVertexBuffer->getBuffer();
        VkDeviceSize gridOffsets[] = {0};
        vkCmdBindVertexBuffers(cmdBuffer, 0, 1, &gridVBuffer, gridOffsets);
        vkCmdBindIndexBuffer(cmdBuffer, gridIndexBuffer->getBuffer(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdSetLineWidth(cmdBuffer, 1.0f); // Thinner lines for grid
        vkCmdDrawIndexed(cmdBuffer, static_cast<uint32_t>(gridIndices.size()), 1, 0, 0, 0);
    }
    
    // Draw axes
    if (isVisible && vertexBuffer && indexBuffer) {
        VkBuffer axisVBuffer = vertexBuffer->getBuffer();
        VkDeviceSize axisOffsets[] = {0};
        vkCmdBindVertexBuffers(cmdBuffer, 0, 1, &axisVBuffer, axisOffsets);
        vkCmdBindIndexBuffer(cmdBuffer, indexBuffer->getBuffer(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdSetLineWidth(cmdBuffer, 3.0f); // Thicker lines for axes
        vkCmdDrawIndexed(cmdBuffer, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
    }
}

VkBuffer AxisGizmo::getVertexBuffer() const {
    return vertexBuffer ? vertexBuffer->getBuffer() : VK_NULL_HANDLE;
}

VkBuffer AxisGizmo::getIndexBuffer() const {
    return indexBuffer ? indexBuffer->getBuffer() : VK_NULL_HANDLE;
}

} // namespace ohao
