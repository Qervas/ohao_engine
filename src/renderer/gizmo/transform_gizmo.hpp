#pragma once
#include <glm/glm.hpp>
#include <vector>
#include "engine/asset/model.hpp"
#include "renderer/gizmo/gizmo_types.hpp"
#include <memory>
#include <vulkan/vulkan_core.h>

namespace ohao {

class VulkanContext;
class OhaoVkBuffer;
class Actor;

class TransformGizmo {
public:
    TransformGizmo() = default;
    ~TransformGizmo() = default;

    bool initialize(VulkanContext* context);
    void cleanup();

    // Render the gizmo at the given position with view-projection matrix
    void render(VkCommandBuffer cmdBuffer, const glm::mat4& viewProj, const glm::vec3& position);

    // Visibility
    void setVisible(bool visible) { isVisible = visible; }
    bool getVisible() const { return isVisible; }

    // Gizmo mode
    void setMode(GizmoMode mode) { currentMode = mode; }
    GizmoMode getMode() const { return currentMode; }

    // Interaction - returns which axis was hit by a ray
    GizmoAxis testRayHit(const glm::vec3& rayOrigin, const glm::vec3& rayDir, const glm::vec3& gizmoPos) const;

    // Drag handling - camera position needed for proper ray-plane intersection
    void beginDrag(GizmoAxis axis, const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                   const glm::vec3& objectPos, const glm::vec3& cameraPos);
    glm::vec3 updateDrag(const glm::vec3& rayOrigin, const glm::vec3& rayDir, const glm::vec3& cameraPos);
    void endDrag();
    bool isDragging() const { return dragging; }
    GizmoAxis getDragAxis() const { return dragAxis; }

    // Visual highlight when hovering
    void setHighlightedAxis(GizmoAxis axis) { highlightedAxis = axis; }
    GizmoAxis getHighlightedAxis() const { return highlightedAxis; }

    // Buffer accessors for rendering
    VkBuffer getVertexBuffer() const;
    VkBuffer getIndexBuffer() const;

    // Per-axis index info for separate rendering with highlighting
    struct AxisIndexInfo {
        uint32_t offset;
        uint32_t count;
    };
    AxisIndexInfo getXAxisInfo() const { return {xAxisIndexOffset, xAxisIndexCount}; }
    AxisIndexInfo getYAxisInfo() const { return {yAxisIndexOffset, yAxisIndexCount}; }
    AxisIndexInfo getZAxisInfo() const { return {zAxisIndexOffset, zAxisIndexCount}; }
    uint32_t getTotalIndexCount() const { return static_cast<uint32_t>(indices.size()); }

private:
    void createArrowGeometry();
    bool createBuffers();

    // Ray-cylinder intersection for axis detection
    bool rayIntersectsCylinder(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                               const glm::vec3& cylinderStart, const glm::vec3& cylinderEnd,
                               float radius, float& t) const;

    VulkanContext* context{nullptr};

    // Geometry for each axis arrow
    std::vector<Vertex> xAxisVertices, yAxisVertices, zAxisVertices;
    std::vector<uint32_t> xAxisIndices, yAxisIndices, zAxisIndices;

    // Combined geometry for efficient rendering
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    std::unique_ptr<OhaoVkBuffer> vertexBuffer;
    std::unique_ptr<OhaoVkBuffer> indexBuffer;

    // Configuration
    float arrowLength{1.5f};      // Length of each axis arrow
    float arrowThickness{0.05f};  // Thickness of arrow shaft
    float coneLength{0.3f};       // Length of arrow cone tip
    float coneRadius{0.12f};      // Radius of cone base
    float hitRadius{0.15f};       // Radius for click detection

    // State
    bool isVisible{true};
    GizmoMode currentMode{GizmoMode::Translate};
    GizmoAxis highlightedAxis{GizmoAxis::None};

    // Drag state
    bool dragging{false};
    GizmoAxis dragAxis{GizmoAxis::None};
    glm::vec3 dragStartPos{0.0f};           // Object position at drag start
    glm::vec3 dragPlaneNormal{0.0f};        // Constraint plane normal
    float dragStartOffset{0.0f};            // Initial click offset along axis
    glm::vec3 currentDragPos{0.0f};         // Current position during drag

    // Index offsets for each axis in the combined buffer
    uint32_t xAxisIndexOffset{0};
    uint32_t xAxisIndexCount{0};
    uint32_t yAxisIndexOffset{0};
    uint32_t yAxisIndexCount{0};
    uint32_t zAxisIndexOffset{0};
    uint32_t zAxisIndexCount{0};
};

} // namespace ohao
