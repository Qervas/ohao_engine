#pragma once

#if !defined(OHAO_HAS_JOLT) || OHAO_HAS_JOLT

/**
 * Jolt Physics <-> GLM type conversions
 * Header-only, zero overhead inline functions.
 */

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Quat.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ohao {
namespace physics {
namespace backend {

// GLM -> Jolt
inline JPH::Vec3 toJolt(const glm::vec3& v) {
    return JPH::Vec3(v.x, v.y, v.z);
}

inline JPH::Quat toJolt(const glm::quat& q) {
    // GLM quat: constructor (w,x,y,z), storage {x,y,z,w}
    // Jolt Quat: constructor (x,y,z,w)
    return JPH::Quat(q.x, q.y, q.z, q.w);
}

inline JPH::RVec3 toJoltR(const glm::vec3& v) {
    return JPH::RVec3(v.x, v.y, v.z);
}

// Jolt -> GLM
inline glm::vec3 toGLM(const JPH::Vec3& v) {
    return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
}

// RVec3 overload only needed in double precision mode (where RVec3 != Vec3)
#ifdef JPH_DOUBLE_PRECISION
inline glm::vec3 toGLM(const JPH::RVec3& v) {
    return glm::vec3(
        static_cast<float>(v.GetX()),
        static_cast<float>(v.GetY()),
        static_cast<float>(v.GetZ())
    );
}
#endif

inline glm::quat toGLM(const JPH::Quat& q) {
    // GLM quat constructor: (w, x, y, z)
    return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
}

} // namespace backend
} // namespace physics
} // namespace ohao

#endif // OHAO_HAS_JOLT
