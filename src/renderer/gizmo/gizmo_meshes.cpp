#include "gizmo_meshes.hpp"
#include <cmath>
#include <glm/gtc/constants.hpp>

namespace ohao {

void GizmoMeshes::addLine(std::vector<GizmoVertex>& vertices, std::vector<uint32_t>& indices,
                           const glm::vec3& start, const glm::vec3& end, const glm::vec3& color) {
    uint32_t base = static_cast<uint32_t>(vertices.size());
    vertices.push_back({start, color});
    vertices.push_back({end, color});
    indices.push_back(base);
    indices.push_back(base + 1);
}

void GizmoMeshes::addCone(std::vector<GizmoVertex>& vertices, std::vector<uint32_t>& indices,
                           const glm::vec3& tip, const glm::vec3& base, float radius,
                           const glm::vec3& color, int segments) {
    // Compute basis vectors perpendicular to cone axis
    glm::vec3 axis = glm::normalize(tip - base);
    glm::vec3 perp1, perp2;
    if (std::abs(axis.y) < 0.99f) {
        perp1 = glm::normalize(glm::cross(axis, glm::vec3(0, 1, 0)));
    } else {
        perp1 = glm::normalize(glm::cross(axis, glm::vec3(1, 0, 0)));
    }
    perp2 = glm::cross(axis, perp1);

    // Generate cone lines from tip to base circle
    for (int i = 0; i < segments; ++i) {
        float angle = 2.0f * glm::pi<float>() * static_cast<float>(i) / static_cast<float>(segments);
        glm::vec3 circlePoint = base + (perp1 * std::cos(angle) + perp2 * std::sin(angle)) * radius;
        addLine(vertices, indices, tip, circlePoint, color);

        // Connect base circle
        float nextAngle = 2.0f * glm::pi<float>() * static_cast<float>(i + 1) / static_cast<float>(segments);
        glm::vec3 nextPoint = base + (perp1 * std::cos(nextAngle) + perp2 * std::sin(nextAngle)) * radius;
        addLine(vertices, indices, circlePoint, nextPoint, color);
    }
}

void GizmoMeshes::addCircle(std::vector<GizmoVertex>& vertices, std::vector<uint32_t>& indices,
                              const glm::vec3& center, const glm::vec3& normal, float radius,
                              const glm::vec3& color, int segments) {
    // Compute basis vectors in the circle plane
    glm::vec3 n = glm::normalize(normal);
    glm::vec3 tangent, bitangent;
    if (std::abs(n.y) < 0.99f) {
        tangent = glm::normalize(glm::cross(n, glm::vec3(0, 1, 0)));
    } else {
        tangent = glm::normalize(glm::cross(n, glm::vec3(1, 0, 0)));
    }
    bitangent = glm::cross(n, tangent);

    for (int i = 0; i < segments; ++i) {
        float angle1 = 2.0f * glm::pi<float>() * static_cast<float>(i) / static_cast<float>(segments);
        float angle2 = 2.0f * glm::pi<float>() * static_cast<float>(i + 1) / static_cast<float>(segments);
        glm::vec3 p1 = center + (tangent * std::cos(angle1) + bitangent * std::sin(angle1)) * radius;
        glm::vec3 p2 = center + (tangent * std::cos(angle2) + bitangent * std::sin(angle2)) * radius;
        addLine(vertices, indices, p1, p2, color);
    }
}

void GizmoMeshes::addCube(std::vector<GizmoVertex>& vertices, std::vector<uint32_t>& indices,
                           const glm::vec3& center, float size, const glm::vec3& color) {
    float h = size * 0.5f;
    glm::vec3 corners[8] = {
        center + glm::vec3(-h, -h, -h), center + glm::vec3( h, -h, -h),
        center + glm::vec3( h,  h, -h), center + glm::vec3(-h,  h, -h),
        center + glm::vec3(-h, -h,  h), center + glm::vec3( h, -h,  h),
        center + glm::vec3( h,  h,  h), center + glm::vec3(-h,  h,  h),
    };

    // 12 edges of a cube
    int edges[][2] = {
        {0,1},{1,2},{2,3},{3,0},  // front face
        {4,5},{5,6},{6,7},{7,4},  // back face
        {0,4},{1,5},{2,6},{3,7}   // connecting edges
    };
    for (auto& e : edges) {
        addLine(vertices, indices, corners[e[0]], corners[e[1]], color);
    }
}

void GizmoMeshes::generateTranslationGizmo(std::vector<GizmoVertex>& vertices,
                                             std::vector<uint32_t>& indices) {
    vertices.clear();
    indices.clear();

    float shaftLen = 1.0f;
    float coneLen = 0.2f;
    float coneRadius = 0.06f;

    // X axis (red)
    addLine(vertices, indices, glm::vec3(0), glm::vec3(shaftLen, 0, 0), X_COLOR);
    addCone(vertices, indices, glm::vec3(shaftLen + coneLen, 0, 0),
            glm::vec3(shaftLen, 0, 0), coneRadius, X_COLOR);

    // Y axis (green)
    addLine(vertices, indices, glm::vec3(0), glm::vec3(0, shaftLen, 0), Y_COLOR);
    addCone(vertices, indices, glm::vec3(0, shaftLen + coneLen, 0),
            glm::vec3(0, shaftLen, 0), coneRadius, Y_COLOR);

    // Z axis (blue)
    addLine(vertices, indices, glm::vec3(0), glm::vec3(0, 0, shaftLen), Z_COLOR);
    addCone(vertices, indices, glm::vec3(0, 0, shaftLen + coneLen),
            glm::vec3(0, 0, shaftLen), coneRadius, Z_COLOR);
}

void GizmoMeshes::generateRotationGizmo(std::vector<GizmoVertex>& vertices,
                                          std::vector<uint32_t>& indices) {
    vertices.clear();
    indices.clear();

    float radius = 1.0f;

    // X rotation circle (in YZ plane, normal = X)
    addCircle(vertices, indices, glm::vec3(0), glm::vec3(1, 0, 0), radius, X_COLOR);

    // Y rotation circle (in XZ plane, normal = Y)
    addCircle(vertices, indices, glm::vec3(0), glm::vec3(0, 1, 0), radius, Y_COLOR);

    // Z rotation circle (in XY plane, normal = Z)
    addCircle(vertices, indices, glm::vec3(0), glm::vec3(0, 0, 1), radius, Z_COLOR);
}

void GizmoMeshes::generateScaleGizmo(std::vector<GizmoVertex>& vertices,
                                       std::vector<uint32_t>& indices) {
    vertices.clear();
    indices.clear();

    float shaftLen = 1.0f;
    float cubeSize = 0.1f;

    // X axis (red)
    addLine(vertices, indices, glm::vec3(0), glm::vec3(shaftLen, 0, 0), X_COLOR);
    addCube(vertices, indices, glm::vec3(shaftLen, 0, 0), cubeSize, X_COLOR);

    // Y axis (green)
    addLine(vertices, indices, glm::vec3(0), glm::vec3(0, shaftLen, 0), Y_COLOR);
    addCube(vertices, indices, glm::vec3(0, shaftLen, 0), cubeSize, Y_COLOR);

    // Z axis (blue)
    addLine(vertices, indices, glm::vec3(0), glm::vec3(0, 0, shaftLen), Z_COLOR);
    addCube(vertices, indices, glm::vec3(0, 0, shaftLen), cubeSize, Z_COLOR);
}

} // namespace ohao
