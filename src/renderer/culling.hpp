#pragma once

#include <glm/glm.hpp>
#include <array>

namespace ohao {

// A frustum plane defined by the equation: dot(normal, point) + d = 0
struct FrustumPlane {
    glm::vec3 normal{0.0f};
    float d{0.0f};

    // Signed distance from point to plane (positive = in front)
    float distanceTo(const glm::vec3& point) const {
        return glm::dot(normal, point) + d;
    }

    void normalize() {
        float len = glm::length(normal);
        if (len > 0.0f) {
            normal /= len;
            d /= len;
        }
    }
};

// Axis-Aligned Bounding Box
struct AABB {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};

    AABB() = default;
    AABB(const glm::vec3& minPt, const glm::vec3& maxPt) : min(minPt), max(maxPt) {}

    glm::vec3 center() const { return (min + max) * 0.5f; }
    glm::vec3 extents() const { return (max - min) * 0.5f; }
    glm::vec3 size() const { return max - min; }
    glm::vec3 halfExtents() const { return size() * 0.5f; }

    void expand(const glm::vec3& point) {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }

    void expand(const AABB& other) {
        min = glm::min(min, other.min);
        max = glm::max(max, other.max);
    }

    bool isValid() const {
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }

    // Transform AABB by a model matrix (produces a new AABB that encloses the transformed box)
    AABB transformed(const glm::mat4& model) const {
        glm::vec3 newCenter = glm::vec3(model * glm::vec4(center(), 1.0f));
        glm::vec3 halfExt = extents();

        // Compute the new extents by projecting each axis
        glm::vec3 newExtents(0.0f);
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                newExtents[i] += std::abs(model[j][i]) * halfExt[j];
            }
        }

        return {newCenter - newExtents, newCenter + newExtents};
    }
};

// View frustum extracted from a view-projection matrix
// Contains 6 planes: Left, Right, Bottom, Top, Near, Far
class Frustum {
public:
    enum Plane { LEFT = 0, RIGHT, BOTTOM, TOP, NEAR_PLANE, FAR_PLANE, COUNT };

    Frustum() = default;

    // Extract frustum planes from a view-projection matrix
    void extractFromViewProj(const glm::mat4& viewProj) {
        // Gribb/Hartmann method
        const glm::mat4& m = viewProj;

        // Left: row3 + row0
        m_planes[LEFT].normal = glm::vec3(m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0]);
        m_planes[LEFT].d = m[3][3] + m[3][0];

        // Right: row3 - row0
        m_planes[RIGHT].normal = glm::vec3(m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0]);
        m_planes[RIGHT].d = m[3][3] - m[3][0];

        // Bottom: row3 + row1
        m_planes[BOTTOM].normal = glm::vec3(m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1]);
        m_planes[BOTTOM].d = m[3][3] + m[3][1];

        // Top: row3 - row1
        m_planes[TOP].normal = glm::vec3(m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1]);
        m_planes[TOP].d = m[3][3] - m[3][1];

        // Near: row3 + row2
        m_planes[NEAR_PLANE].normal = glm::vec3(m[0][3] + m[0][2], m[1][3] + m[1][2], m[2][3] + m[2][2]);
        m_planes[NEAR_PLANE].d = m[3][3] + m[3][2];

        // Far: row3 - row2
        m_planes[FAR_PLANE].normal = glm::vec3(m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2]);
        m_planes[FAR_PLANE].d = m[3][3] - m[3][2];

        // Normalize all planes
        for (auto& plane : m_planes) {
            plane.normalize();
        }
    }

    // Test if an AABB is inside or intersects the frustum
    // Returns true if the AABB should be rendered (visible or partially visible)
    bool isAABBVisible(const AABB& aabb) const {
        glm::vec3 c = aabb.center();
        glm::vec3 e = aabb.extents();

        for (const auto& plane : m_planes) {
            // Compute the effective radius of the AABB projected onto the plane normal
            float r = e.x * std::abs(plane.normal.x) +
                      e.y * std::abs(plane.normal.y) +
                      e.z * std::abs(plane.normal.z);

            // If the center is further than radius behind the plane, AABB is outside
            if (plane.distanceTo(c) < -r) {
                return false;
            }
        }
        return true;
    }

    // Test if a sphere is inside or intersects the frustum
    bool isSphereVisible(const glm::vec3& center, float radius) const {
        for (const auto& plane : m_planes) {
            if (plane.distanceTo(center) < -radius) {
                return false;
            }
        }
        return true;
    }

    // Test a single point
    bool isPointVisible(const glm::vec3& point) const {
        for (const auto& plane : m_planes) {
            if (plane.distanceTo(point) < 0.0f) {
                return false;
            }
        }
        return true;
    }

    const FrustumPlane& getPlane(int index) const { return m_planes[index]; }

private:
    std::array<FrustumPlane, COUNT> m_planes;
};

} // namespace ohao
