#pragma once

#include <glm/glm.hpp>
#include <vector>

namespace ohao {

// Simple colored vertex for gizmo rendering (no lighting)
struct GizmoVertex {
    glm::vec3 position;
    glm::vec3 color;
};

// Gizmo axis identifier
enum class GizmoAxis {
    NONE = 0,
    X,
    Y,
    Z
};

// Gizmo mode
enum class GizmoMode {
    TRANSLATE = 0,
    ROTATE,
    SCALE
};

// Generates line-based gizmo geometry for transform handles
class GizmoMeshes {
public:
    // Generate translation gizmo (3 arrows: cone + shaft along X/Y/Z)
    static void generateTranslationGizmo(std::vector<GizmoVertex>& vertices,
                                          std::vector<uint32_t>& indices);

    // Generate rotation gizmo (3 circles in XY/XZ/YZ planes)
    static void generateRotationGizmo(std::vector<GizmoVertex>& vertices,
                                       std::vector<uint32_t>& indices);

    // Generate scale gizmo (3 axis lines with cube endpoints)
    static void generateScaleGizmo(std::vector<GizmoVertex>& vertices,
                                    std::vector<uint32_t>& indices);

    // Axis colors
    static constexpr glm::vec3 X_COLOR{0.9f, 0.2f, 0.2f};  // Red
    static constexpr glm::vec3 Y_COLOR{0.2f, 0.9f, 0.2f};   // Green
    static constexpr glm::vec3 Z_COLOR{0.2f, 0.2f, 0.9f};   // Blue
    static constexpr glm::vec3 HIGHLIGHT_COLOR{1.0f, 1.0f, 0.0f};  // Yellow

private:
    // Helper to add a line segment
    static void addLine(std::vector<GizmoVertex>& vertices, std::vector<uint32_t>& indices,
                        const glm::vec3& start, const glm::vec3& end, const glm::vec3& color);

    // Helper to add a cone (approximated with lines)
    static void addCone(std::vector<GizmoVertex>& vertices, std::vector<uint32_t>& indices,
                        const glm::vec3& tip, const glm::vec3& base, float radius,
                        const glm::vec3& color, int segments = 8);

    // Helper to add a circle (line loop)
    static void addCircle(std::vector<GizmoVertex>& vertices, std::vector<uint32_t>& indices,
                          const glm::vec3& center, const glm::vec3& normal, float radius,
                          const glm::vec3& color, int segments = 32);

    // Helper to add a small cube
    static void addCube(std::vector<GizmoVertex>& vertices, std::vector<uint32_t>& indices,
                        const glm::vec3& center, float size, const glm::vec3& color);
};

} // namespace ohao
