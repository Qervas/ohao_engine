#pragma once

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/norm.hpp>

namespace ohao {
namespace physics {
namespace math {

// === Vector Math ===
inline float lengthSquared(const glm::vec3& v) {
    return glm::dot(v, v);
}

inline glm::vec3 safeNormalize(const glm::vec3& v, const glm::vec3& fallback = glm::vec3(0, 1, 0)) {
    float lenSq = lengthSquared(v);
    if (lenSq > 1e-6f) {
        return v / glm::sqrt(lenSq);
    }
    return fallback;
}

inline glm::vec3 clampLength(const glm::vec3& v, float maxLength) {
    float lenSq = lengthSquared(v);
    if (lenSq > maxLength * maxLength) {
        return v * (maxLength / glm::sqrt(lenSq));
    }
    return v;
}

// === Transform Math ===
inline glm::mat4 createTransformMatrix(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale = glm::vec3(1.0f)) {
    glm::mat4 translation = glm::translate(glm::mat4(1.0f), position);
    glm::mat4 rotationMat = glm::mat4_cast(rotation);
    glm::mat4 scaleMat = glm::scale(glm::mat4(1.0f), scale);
    return translation * rotationMat * scaleMat;
}

inline glm::vec3 transformPoint(const glm::vec3& point, const glm::mat4& transform) {
    glm::vec4 result = transform * glm::vec4(point, 1.0f);
    return glm::vec3(result) / result.w;
}

inline glm::vec3 transformVector(const glm::vec3& vector, const glm::mat4& transform) {
    glm::vec4 result = transform * glm::vec4(vector, 0.0f);
    return glm::vec3(result);
}

// === Quaternion Math ===
inline glm::quat safeNormalize(const glm::quat& q) {
    float len = glm::length(q);
    if (len > 1e-6f) {
        return q / len;
    }
    return glm::quat(1, 0, 0, 0); // Identity quaternion
}

inline glm::quat integrateAngularVelocity(const glm::quat& rotation, const glm::vec3& angularVelocity, float deltaTime) {
    if (lengthSquared(angularVelocity) < 1e-6f) {
        return rotation;
    }
    
    glm::vec3 axis = safeNormalize(angularVelocity);
    float angle = glm::length(angularVelocity) * deltaTime;
    glm::quat deltaRotation = glm::angleAxis(angle, axis);
    
    return safeNormalize(rotation * deltaRotation);
}

// === Collision Math ===
struct AABB {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    
    AABB() = default;
    AABB(const glm::vec3& center, const glm::vec3& halfExtents) 
        : min(center - halfExtents), max(center + halfExtents) {}
    
    glm::vec3 getCenter() const { return (min + max) * 0.5f; }
    glm::vec3 getExtents() const { return (max - min) * 0.5f; }
    glm::vec3 getSize() const { return max - min; }
    
    bool contains(const glm::vec3& point) const {
        return point.x >= min.x && point.x <= max.x &&
               point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }
    
    bool intersects(const AABB& other) const {
        return min.x <= other.max.x && max.x >= other.min.x &&
               min.y <= other.max.y && max.y >= other.min.y &&
               min.z <= other.max.z && max.z >= other.min.z;
    }
    
    AABB expand(float amount) const {
        glm::vec3 expansion(amount);
        return AABB(getCenter(), getExtents() + expansion);
    }
};

// === Physics Constants ===
namespace constants {
    constexpr float EPSILON = 1e-6f;
    constexpr float GRAVITY_EARTH = 9.81f;
    constexpr float PI = 3.14159265359f;
    constexpr float TWO_PI = 2.0f * PI;
    constexpr float HALF_PI = PI * 0.5f;
}

// === Utility Functions ===
inline bool isNearZero(float value, float epsilon = constants::EPSILON) {
    return glm::abs(value) < epsilon;
}

inline bool isNearZero(const glm::vec3& v, float epsilon = constants::EPSILON) {
    return lengthSquared(v) < epsilon * epsilon;
}

inline float clamp(float value, float min, float max) {
    return glm::clamp(value, min, max);
}

inline glm::vec3 clamp(const glm::vec3& v, const glm::vec3& min, const glm::vec3& max) {
    return glm::clamp(v, min, max);
}

} // namespace math
} // namespace physics  
} // namespace ohao