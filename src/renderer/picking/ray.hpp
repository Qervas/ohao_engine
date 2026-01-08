#pragma once
#include <glm/glm.hpp>

namespace ohao {

struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};  // Normalized

    Ray() = default;
    Ray(const glm::vec3& orig, const glm::vec3& dir)
        : origin(orig), direction(glm::normalize(dir)) {}

    // Get point along ray at distance t
    glm::vec3 pointAt(float t) const {
        return origin + direction * t;
    }
};

// Result of a picking operation
struct PickResult {
    class Actor* actor = nullptr;
    float distance = std::numeric_limits<float>::max();
    glm::vec3 hitPoint{0.0f};
    glm::vec3 hitNormal{0.0f};
    bool hit = false;

    // For sorting by distance
    bool operator<(const PickResult& other) const {
        return distance < other.distance;
    }
};

// Axis-aligned bounding box for fast rejection
struct AABB {
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};

    AABB() = default;
    AABB(const glm::vec3& minPt, const glm::vec3& maxPt) : min(minPt), max(maxPt) {}

    void expand(const glm::vec3& point) {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }

    void expand(const AABB& other) {
        min = glm::min(min, other.min);
        max = glm::max(max, other.max);
    }

    glm::vec3 center() const { return (min + max) * 0.5f; }
    glm::vec3 size() const { return max - min; }
    glm::vec3 halfExtents() const { return size() * 0.5f; }

    bool isValid() const {
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }
};

} // namespace ohao
