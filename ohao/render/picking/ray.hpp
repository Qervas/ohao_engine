#pragma once
#include <glm/glm.hpp>
#include "render/culling.hpp"

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

// AABB is now defined in renderer/culling.hpp

} // namespace ohao
