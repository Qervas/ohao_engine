#pragma once

#include "core/concepts.hpp"

#include <glm/glm.hpp>
#include <array>
#include <cmath>

namespace ohao {

struct FrustumPlane {
    glm::vec3 normal{0.0f};
    float d{0.0f};

    [[nodiscard]] float distanceTo(const glm::vec3& point) const noexcept {
        return glm::dot(normal, point) + d;
    }

    void normalize() noexcept {
        const float len = glm::length(normal);
        if (len > 0.0f) {
            normal /= len;
            d /= len;
        }
    }
};

struct AABB {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};

    AABB() = default;
    AABB(const glm::vec3& minPt, const glm::vec3& maxPt) noexcept : min(minPt), max(maxPt) {}

    [[nodiscard]] glm::vec3 center() const noexcept { return (min + max) * 0.5f; }
    [[nodiscard]] glm::vec3 extents() const noexcept { return (max - min) * 0.5f; }
    [[nodiscard]] glm::vec3 size() const noexcept { return max - min; }
    [[nodiscard]] glm::vec3 halfExtents() const noexcept { return size() * 0.5f; }

    [[nodiscard]] bool isValid() const noexcept {
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }

    [[nodiscard]] bool empty() const noexcept { return !isValid(); }

    void expand(const glm::vec3& point) noexcept {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }

    void expand(const AABB& other) noexcept {
        min = glm::min(min, other.min);
        max = glm::max(max, other.max);
    }

    [[nodiscard]] AABB transformed(const glm::mat4& model) const {
        const glm::vec3 newCenter = glm::vec3(model * glm::vec4(center(), 1.0f));
        const glm::vec3 halfExt = extents();

        glm::vec3 newExtents(0.0f);
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                newExtents[i] += std::abs(model[j][i]) * halfExt[j];
            }
        }

        return {newCenter - newExtents, newCenter + newExtents};
    }
};

static_assert(sizeof(FrustumPlane) >= sizeof(float) * 4);
static_assert(sizeof(AABB) >= sizeof(float) * 6);

class Frustum {
public:
    enum Plane : int { LEFT = 0, RIGHT, BOTTOM, TOP, NEAR_PLANE, FAR_PLANE, COUNT };

    Frustum() = default;

    void extractFromViewProj(const glm::mat4& viewProj) {
        const glm::mat4& m = viewProj;

        m_planes[LEFT].normal = glm::vec3(m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0]);
        m_planes[LEFT].d = m[3][3] + m[3][0];

        m_planes[RIGHT].normal = glm::vec3(m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0]);
        m_planes[RIGHT].d = m[3][3] - m[3][0];

        m_planes[BOTTOM].normal = glm::vec3(m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1]);
        m_planes[BOTTOM].d = m[3][3] + m[3][1];

        m_planes[TOP].normal = glm::vec3(m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1]);
        m_planes[TOP].d = m[3][3] - m[3][1];

        m_planes[NEAR_PLANE].normal = glm::vec3(m[0][3] + m[0][2], m[1][3] + m[1][2], m[2][3] + m[2][2]);
        m_planes[NEAR_PLANE].d = m[3][3] + m[3][2];

        m_planes[FAR_PLANE].normal = glm::vec3(m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2]);
        m_planes[FAR_PLANE].d = m[3][3] - m[3][2];

        for (auto& plane : m_planes) {
            plane.normalize();
        }
    }

    [[nodiscard]] bool isAABBVisible(const AABB& aabb) const noexcept {
        const glm::vec3 c = aabb.center();
        const glm::vec3 e = aabb.extents();

        for (const auto& plane : m_planes) {
            const float r = e.x * std::abs(plane.normal.x) +
                            e.y * std::abs(plane.normal.y) +
                            e.z * std::abs(plane.normal.z);
            if (plane.distanceTo(c) < -r) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool isSphereVisible(const glm::vec3& center, float radius) const noexcept {
        for (const auto& plane : m_planes) {
            if (plane.distanceTo(center) < -radius) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool isPointVisible(const glm::vec3& point) const noexcept {
        for (const auto& plane : m_planes) {
            if (plane.distanceTo(point) < 0.0f) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] const FrustumPlane& getPlane(int index) const noexcept { return m_planes[index]; }

private:
    std::array<FrustumPlane, COUNT> m_planes{};
};

} // namespace ohao
