#include "axis_gizmo.hpp"
#include "renderer/vulkan_context.hpp"
#include "rhi/vk/ohao_vk_buffer.hpp"
#include <stdexcept>

namespace ohao {

bool AxisGizmo::initialize(VulkanContext* contextPtr) {
    context = contextPtr;

    createGizmoGeometry();
    if (!createBuffers()) {
        return false;
    }

    return true;
}

void AxisGizmo::cleanup() {
    vertexBuffer.reset();
    indexBuffer.reset();
    vertices.clear();
    indices.clear();
}

void AxisGizmo::createGizmoGeometry() {
    // X axis (Red)
    vertices.push_back({{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0, 0, 1}, {0, 0}}); // Origin
    vertices.push_back({{AXIS_LENGTH, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0, 0, 1}, {1, 0}}); // X end

    // Y axis (Green)
    vertices.push_back({{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0, 0, 1}, {0, 0}}); // Origin
    vertices.push_back({{0.0f, AXIS_LENGTH, 0.0f}, {0.0f, 1.0f, 0.0f}, {0, 0, 1}, {1, 0}}); // Y end

    // Z axis (Blue)
    vertices.push_back({{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0, 1, 0}, {0, 0}}); // Origin
    vertices.push_back({{0.0f, 0.0f, AXIS_LENGTH}, {0.0f, 0.0f, 1.0f}, {0, 1, 0}, {1, 0}}); // Z end

    // Create line indices
    indices = {
        0, 1,  // X axis line
        2, 3,  // Y axis line
        4, 5   // Z axis line
    };
}

bool AxisGizmo::createBuffers() {
    try {
        // Create vertex buffer
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

        // Create index buffer
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

        return true;
    }
    catch (const std::exception& e) {
        cleanup();
        return false;
    }
}

void AxisGizmo::render(VkCommandBuffer cmdBuffer, const glm::mat4& viewProj) {
    if (!vertexBuffer || !indexBuffer) return;
    // Set line width if supported and enabled as dynamic state
    vkCmdSetLineWidth(cmdBuffer, 2.0f);
    // Draw axes as lines
    vkCmdDrawIndexed(cmdBuffer, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
}

VkBuffer AxisGizmo::getVertexBuffer() const {
    return vertexBuffer ? vertexBuffer->getBuffer() : VK_NULL_HANDLE;
}

VkBuffer AxisGizmo::getIndexBuffer() const {
    return indexBuffer ? indexBuffer->getBuffer() : VK_NULL_HANDLE;
}
} // namespace ohao
