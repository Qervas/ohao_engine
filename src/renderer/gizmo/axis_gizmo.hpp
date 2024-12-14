#pragma once
#include <glm/glm.hpp>
#include <vector>
#include "core/asset/model.hpp"
#include <memory>
#include <vulkan/vulkan_core.h>

namespace ohao {

class VulkanContext;
class OhaoVkBuffer;

class AxisGizmo {
public:
    AxisGizmo() = default;
    ~AxisGizmo() = default;

    bool initialize(VulkanContext* context);
    void cleanup();
    void render(VkCommandBuffer cmdBuffer, const glm::mat4& viewProj);
    VkBuffer getVertexBuffer() const;
    VkBuffer getIndexBuffer() const;

private:
    void createGizmoGeometry();
    bool createBuffers();

    VulkanContext* context{nullptr};
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    std::unique_ptr<OhaoVkBuffer> vertexBuffer;
    std::unique_ptr<OhaoVkBuffer> indexBuffer;

    const float AXIS_LENGTH = 1.0f;  // Length of each axis
    const float AXIS_THICKNESS = 0.02f;  // Thickness of the axis lines
};

} // namespace ohao
