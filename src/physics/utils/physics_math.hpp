#pragma once

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/norm.hpp>
#include <glm/gtx/matrix_cross_product.hpp>

namespace ohao {
namespace physics {
namespace math {

// === PHYSICS CONSTANTS ===
namespace constants {
    // Mathematical constants
    constexpr float PI = 3.14159265359f;
    constexpr float TWO_PI = 2.0f * PI;
    constexpr float HALF_PI = PI * 0.5f;
    constexpr float EPSILON = 1e-6f;
    constexpr float LARGE_NUMBER = 1e6f;
    
    // Physics constants
    constexpr float GRAVITY_EARTH = 9.81f;
    constexpr float GRAVITY_MOON = 1.62f;
    constexpr float GRAVITY_MARS = 3.71f;
    
    // Simulation limits
    constexpr float MAX_LINEAR_VELOCITY = 100.0f;
    constexpr float MAX_ANGULAR_VELOCITY = 50.0f;
    constexpr float MIN_MASS = 1e-3f;
    constexpr float MAX_MASS = 1e6f;
}

// === VECTOR MATH ===
inline float lengthSquared(const glm::vec3& v) {
    return glm::dot(v, v);
}

inline float length(const glm::vec3& v) {
    return glm::sqrt(lengthSquared(v));
}

inline glm::vec3 safeNormalize(const glm::vec3& v, const glm::vec3& fallback = glm::vec3(0, 1, 0)) {
    float lenSq = lengthSquared(v);
    if (lenSq > constants::EPSILON * constants::EPSILON) {
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

// Cross product in matrix form (for inertia calculations)
inline glm::mat3 crossProductMatrix(const glm::vec3& v) {
    return glm::mat3(
        0.0f, -v.z, v.y,
        v.z, 0.0f, -v.x,
        -v.y, v.x, 0.0f
    );
}

// === TRANSFORM MATH ===
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

// Transform a vector by rotation only
inline glm::vec3 rotateVector(const glm::vec3& vector, const glm::quat& rotation) {
    return rotation * vector;
}

// === QUATERNION MATH ===
inline glm::quat safeNormalize(const glm::quat& q) {
    float len = glm::length(q);
    if (len > constants::EPSILON) {
        return q / len;
    }
    return glm::quat(1, 0, 0, 0); // Identity quaternion
}

inline glm::quat integrateAngularVelocity(const glm::quat& rotation, const glm::vec3& angularVelocity, float deltaTime) {
    if (lengthSquared(angularVelocity) < constants::EPSILON * constants::EPSILON) {
        return rotation;
    }
    
    glm::vec3 axis = safeNormalize(angularVelocity);
    float angle = glm::length(angularVelocity) * deltaTime;
    glm::quat deltaRotation = glm::angleAxis(angle, axis);
    
    return safeNormalize(rotation * deltaRotation);
}

// Convert angular velocity to quaternion derivative
inline glm::quat angularVelocityToQuaternionDerivative(const glm::quat& q, const glm::vec3& omega) {
    glm::quat omegaQuat(0, omega.x, omega.y, omega.z);
    return 0.5f * omegaQuat * q;
}

// === COLLISION MATH ===
struct AABB {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    
    AABB() = default;
    AABB(const glm::vec3& center, const glm::vec3& halfExtents) 
        : min(center - halfExtents), max(center + halfExtents) {}
    
    glm::vec3 getCenter() const { return (min + max) * 0.5f; }
    glm::vec3 getExtents() const { return (max - min) * 0.5f; }
    glm::vec3 getSize() const { return max - min; }
    float getVolume() const { 
        glm::vec3 size = getSize();
        return size.x * size.y * size.z;
    }
    
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
    
    AABB combine(const AABB& other) const {
        AABB result;
        result.min = glm::min(min, other.min);
        result.max = glm::max(max, other.max);
        return result;
    }
    
    float distanceSquared(const AABB& other) const {
        glm::vec3 distance = glm::max(glm::vec3(0.0f), glm::max(min - other.max, other.min - max));
        return lengthSquared(distance);
    }
};

// Plane representation for collision detection
struct Plane {
    glm::vec3 normal{0, 1, 0};
    float distance{0.0f};
    
    Plane() = default;
    Plane(const glm::vec3& n, float d) : normal(safeNormalize(n)), distance(d) {}
    Plane(const glm::vec3& n, const glm::vec3& point) : normal(safeNormalize(n)) {
        distance = glm::dot(normal, point);
    }
    
    float distanceToPoint(const glm::vec3& point) const {
        return glm::dot(normal, point) - distance;
    }
    
    glm::vec3 projectPoint(const glm::vec3& point) const {
        return point - normal * distanceToPoint(point);
    }
};

// === UTILITY FUNCTIONS ===
inline bool isNearZero(float value, float epsilon = constants::EPSILON) {
    return glm::abs(value) < epsilon;
}

inline bool isNearZero(const glm::vec3& v, float epsilon = constants::EPSILON) {
    return lengthSquared(v) < epsilon * epsilon;
}

inline bool isFinite(float value) {
    return std::isfinite(value);
}

inline bool isFinite(const glm::vec3& v) {
    return isFinite(v.x) && isFinite(v.y) && isFinite(v.z);
}

inline float clamp(float value, float min, float max) {
    return glm::clamp(value, min, max);
}

inline glm::vec3 clamp(const glm::vec3& v, const glm::vec3& min, const glm::vec3& max) {
    return glm::clamp(v, min, max);
}

inline float lerp(float a, float b, float t) {
    return a + t * (b - a);
}

inline glm::vec3 lerp(const glm::vec3& a, const glm::vec3& b, float t) {
    return a + t * (b - a);
}

// Smooth step function
inline float smoothstep(float edge0, float edge1, float x) {
    float t = clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace math
} // namespace physics  
} // namespace ohao