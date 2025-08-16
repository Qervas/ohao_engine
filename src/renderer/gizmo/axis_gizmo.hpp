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
    
    // Visibility controls
    void setVisible(bool visible) { isVisible = visible; }
    bool getVisible() const { return isVisible; }
    
    void setGridVisible(bool visible) { isGridVisible = visible; }
    bool getGridVisible() const { return isGridVisible; }

private:
    void createGizmoGeometry();
    void createGridGeometry();
    bool createBuffers();

    VulkanContext* context{nullptr};
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    
    // Grid geometry
    std::vector<Vertex> gridVertices;
    std::vector<uint32_t> gridIndices;

    std::unique_ptr<OhaoVkBuffer> vertexBuffer;
    std::unique_ptr<OhaoVkBuffer> indexBuffer;
    std::unique_ptr<OhaoVkBuffer> gridVertexBuffer;
    std::unique_ptr<OhaoVkBuffer> gridIndexBuffer;

    // Configuration
    const float AXIS_LENGTH = 5.0f;  // Increased length for better visibility
    const float AXIS_THICKNESS = 0.03f;  // Slightly thicker
    const float GRID_SIZE = 20.0f;  // Grid extends 20 units in each direction
    const float GRID_SPACING = 1.0f;  // 1 unit between grid lines
    const int RULER_DIVISIONS = 5;  // Number of ruler marks per axis
    
    // Visibility state
    bool isVisible = true;
    bool isGridVisible = true;
};

} // namespace ohao
