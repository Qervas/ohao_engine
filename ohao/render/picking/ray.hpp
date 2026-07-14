#pragma once

#include "render/culling.hpp"

#include <glm/glm.hpp>
#include <limits>

namespace ohao {

struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};  // Normalized

    Ray() = default;
    Ray(const glm::vec3& orig, const glm::vec3& dir)
        : origin(orig), direction(glm::normalize(dir)) {}

    [[nodiscard]] glm::vec3 pointAt(float t) const noexcept {
        return origin + direction * t;
    }
};

struct PickResult {
    class Actor* actor = nullptr;
    float distance = std::numeric_limits<float>::max();
    glm::vec3 hitPoint{0.0f};
    glm::vec3 hitNormal{0.0f};
    bool hit = false;

    [[nodiscard]] explicit operator bool() const noexcept { return hit; }

    [[nodiscard]] bool operator<(const PickResult& other) const noexcept {
        return distance < other.distance;
    }
};

} // namespace ohao
