#include "narrow_phase.hpp"
#include "shapes/box_shape.hpp"
#include "shapes/sphere_shape.hpp"
#include "shapes/capsule_shape.hpp"
#include "shapes/plane_shape.hpp"
#include "physics/material/physics_material.hpp"
#include <algorithm>

namespace ohao {
namespace physics {
namespace collision {

// === NARROW PHASE DETECTOR IMPLEMENTATION ===

NarrowPhaseDetector::NarrowPhaseDetector() {
    setupCollisionFunctions();
}

ContactManifold NarrowPhaseDetector::detectCollision(
    dynamics::RigidBody* bodyA, 
    dynamics::RigidBody* bodyB) {
    
    if (!bodyA || !bodyB) {
        return ContactManifold();
    }
    
    // Skip static-static pairs
    if (bodyA->isStatic() && bodyB->isStatic()) {
        return ContactManifold();
    }
    
    auto shapeA = bodyA->getCollisionShape();
    auto shapeB = bodyB->getCollisionShape();
    
    if (!shapeA || !shapeB) {
        return ContactManifold();
    }
    
    ContactManifold manifold = detectShapeCollision(
        shapeA.get(), bodyA->getPosition(), bodyA->getRotation(),
        shapeB.get(), bodyB->getPosition(), bodyB->getRotation()
    );
    
    if (manifold.isValid()) {
        manifold.bodyA = bodyA;
        manifold.bodyB = bodyB;
        manifold.shapeA = shapeA;
        manifold.shapeB = shapeB;
        calculateMaterialProperties(manifold, bodyA, bodyB);
    }
    
    return manifold;
}

ContactManifold NarrowPhaseDetector::detectShapeCollision(
    const CollisionShape* shapeA, const glm::vec3& posA, const glm::quat& rotA,
    const CollisionShape* shapeB, const glm::vec3& posB, const glm::quat& rotB) {
    
    ShapeType typeA = shapeA->getType();
    ShapeType typeB = shapeB->getType();
    
    uint64_t key = makeShapeTypeKey(typeA, typeB);
    auto it = m_collisionFunctions.find(key);
    
    if (it != m_collisionFunctions.end()) {
        return it->second(shapeA, posA, rotA, shapeB, posB, rotB);
    }
    
    // Fallback: no collision function found
    return ContactManifold();
}

void NarrowPhaseDetector::registerCollisionFunction(ShapeType typeA, ShapeType typeB, CollisionFunction func) {
    uint64_t key = makeShapeTypeKey(typeA, typeB);
    m_collisionFunctions[key] = func;
}

void NarrowPhaseDetector::setupCollisionFunctions() {
    // Box vs Box
    registerCollisionFunction(ShapeType::BOX, ShapeType::BOX,
        [this](const CollisionShape* a, const glm::vec3& posA, const glm::quat& rotA,
               const CollisionShape* b, const glm::vec3& posB, const glm::quat& rotB) {
            if (m_useSAT) {
                return detectBoxVsBoxSAT(
                    static_cast<const BoxShape*>(a), posA, rotA,
                    static_cast<const BoxShape*>(b), posB, rotB);
            } else {
                return detectBoxVsBox(
                    static_cast<const BoxShape*>(a), posA, rotA,
                    static_cast<const BoxShape*>(b), posB, rotB);
            }
        });
    
    // Sphere vs Sphere
    registerCollisionFunction(ShapeType::SPHERE, ShapeType::SPHERE,
        [this](const CollisionShape* a, const glm::vec3& posA, const glm::quat& rotA,
               const CollisionShape* b, const glm::vec3& posB, const glm::quat& rotB) {
            return detectSphereVsSphere(
                static_cast<const SphereShape*>(a), posA,
                static_cast<const SphereShape*>(b), posB);
        });
    
    // Box vs Sphere (and Sphere vs Box)
    registerCollisionFunction(ShapeType::BOX, ShapeType::SPHERE,
        [this](const CollisionShape* a, const glm::vec3& posA, const glm::quat& rotA,
               const CollisionShape* b, const glm::vec3& posB, const glm::quat& rotB) {
            return detectBoxVsSphere(
                static_cast<const BoxShape*>(a), posA, rotA,
                static_cast<const SphereShape*>(b), posB);
        });
    
    registerCollisionFunction(ShapeType::SPHERE, ShapeType::BOX,
        [this](const CollisionShape* a, const glm::vec3& posA, const glm::quat& rotA,
               const CollisionShape* b, const glm::vec3& posB, const glm::quat& rotB) {
            ContactManifold manifold = detectBoxVsSphere(
                static_cast<const BoxShape*>(b), posB, rotB,
                static_cast<const SphereShape*>(a), posA);
            // Flip normal since we swapped the order
            manifold.normal = -manifold.normal;
            return manifold;
        });
    
    // Sphere vs Capsule (and Capsule vs Sphere)
    registerCollisionFunction(ShapeType::SPHERE, ShapeType::CAPSULE,
        [this](const CollisionShape* a, const glm::vec3& posA, const glm::quat& rotA,
               const CollisionShape* b, const glm::vec3& posB, const glm::quat& rotB) {
            return detectSphereVsCapsule(
                static_cast<const SphereShape*>(a), posA,
                static_cast<const CapsuleShape*>(b), posB, rotB);
        });
    
    registerCollisionFunction(ShapeType::CAPSULE, ShapeType::SPHERE,
        [this](const CollisionShape* a, const glm::vec3& posA, const glm::quat& rotA,
               const CollisionShape* b, const glm::vec3& posB, const glm::quat& rotB) {
            ContactManifold manifold = detectSphereVsCapsule(
                static_cast<const SphereShape*>(b), posB,
                static_cast<const CapsuleShape*>(a), posA, rotA);
            manifold.normal = -manifold.normal;
            return manifold;
        });
    
    // Sphere vs Plane (and Plane vs Sphere)
    registerCollisionFunction(ShapeType::SPHERE, ShapeType::PLANE,
        [this](const CollisionShape* a, const glm::vec3& posA, const glm::quat& rotA,
               const CollisionShape* b, const glm::vec3& posB, const glm::quat& rotB) {
            return detectSphereVsPlane(
                static_cast<const SphereShape*>(a), posA,
                static_cast<const PlaneShape*>(b), posB, rotB);
        });
    
    registerCollisionFunction(ShapeType::PLANE, ShapeType::SPHERE,
        [this](const CollisionShape* a, const glm::vec3& posA, const glm::quat& rotA,
               const CollisionShape* b, const glm::vec3& posB, const glm::quat& rotB) {
            ContactManifold manifold = detectSphereVsPlane(
                static_cast<const SphereShape*>(b), posB,
                static_cast<const PlaneShape*>(a), posA, rotA);
            manifold.normal = -manifold.normal;
            return manifold;
        });
    
    // Box vs Plane (and Plane vs Box)
    registerCollisionFunction(ShapeType::BOX, ShapeType::PLANE,
        [this](const CollisionShape* a, const glm::vec3& posA, const glm::quat& rotA,
               const CollisionShape* b, const glm::vec3& posB, const glm::quat& rotB) {
            return detectBoxVsPlane(
                static_cast<const BoxShape*>(a), posA, rotA,
                static_cast<const PlaneShape*>(b), posB, rotB);
        });
    
    registerCollisionFunction(ShapeType::PLANE, ShapeType::BOX,
        [this](const CollisionShape* a, const glm::vec3& posA, const glm::quat& rotA,
               const CollisionShape* b, const glm::vec3& posB, const glm::quat& rotB) {
            ContactManifold manifold = detectBoxVsPlane(
                static_cast<const BoxShape*>(b), posB, rotB,
                static_cast<const PlaneShape*>(a), posA, rotA);
            manifold.normal = -manifold.normal;
            return manifold;
        });
    
    // Capsule vs Capsule
    registerCollisionFunction(ShapeType::CAPSULE, ShapeType::CAPSULE,
        [this](const CollisionShape* a, const glm::vec3& posA, const glm::quat& rotA,
               const CollisionShape* b, const glm::vec3& posB, const glm::quat& rotB) {
            return detectCapsuleVsCapsule(
                static_cast<const CapsuleShape*>(a), posA, rotA,
                static_cast<const CapsuleShape*>(b), posB, rotB);
        });
}

ContactManifold NarrowPhaseDetector::detectSphereVsSphere(
    const SphereShape* sphereA, const glm::vec3& posA,
    const SphereShape* sphereB, const glm::vec3& posB) {
    
    glm::vec3 centerDistance = posB - posA;
    float distance = glm::length(centerDistance);
    float radiusSum = sphereA->getRadius() + sphereB->getRadius();
    
    if (distance >= radiusSum || distance < math::constants::EPSILON) {
        return ContactManifold(); // No collision or spheres are coincident
    }
    
    glm::vec3 normal = centerDistance / distance;
    float penetration = radiusSum - distance;
    
    ContactManifold manifold(normal, penetration);
    
    // Single contact point at the midpoint between sphere surfaces
    glm::vec3 contactPos = posA + normal * sphereA->getRadius();
    ContactPoint contact(contactPos);
    
    // Calculate local positions for warm starting
    contact.localPositionA = contactPos - posA;
    contact.localPositionB = contactPos - posB;
    
    manifold.addContactPoint(contact);
    return manifold;
}

ContactManifold NarrowPhaseDetector::detectBoxVsSphere(
    const BoxShape* box, const glm::vec3& boxPos, const glm::quat& boxRot,
    const SphereShape* sphere, const glm::vec3& spherePos) {
    
    // Transform sphere center to box local space
    glm::mat4 boxToWorld = math::createTransformMatrix(boxPos, boxRot);
    glm::mat4 worldToBox = glm::inverse(boxToWorld);
    glm::vec3 sphereLocalPos = math::transformPoint(spherePos, worldToBox);
    
    // Find closest point on box to sphere center (in local space)
    glm::vec3 halfExtents = box->getHalfExtents();
    glm::vec3 closestLocal = CollisionUtils::clampPointToBox(sphereLocalPos, halfExtents);
    
    // Calculate distance
    glm::vec3 localDistance = sphereLocalPos - closestLocal;
    float distanceLength = glm::length(localDistance);
    float sphereRadius = sphere->getRadius();
    
    if (distanceLength >= sphereRadius) {
        return ContactManifold(); // No collision
    }
    
    // Handle case where sphere center is inside the box
    glm::vec3 normal;
    float penetration;
    
    if (distanceLength < math::constants::EPSILON) {
        // Sphere center is inside box - find closest face
        glm::vec3 distToFaces = halfExtents - glm::abs(sphereLocalPos);
        
        if (distToFaces.x <= distToFaces.y && distToFaces.x <= distToFaces.z) {
            // Closest to X face
            normal = glm::vec3(sphereLocalPos.x > 0 ? 1.0f : -1.0f, 0.0f, 0.0f);
            penetration = sphereRadius + distToFaces.x;
        } else if (distToFaces.y <= distToFaces.z) {
            // Closest to Y face
            normal = glm::vec3(0.0f, sphereLocalPos.y > 0 ? 1.0f : -1.0f, 0.0f);
            penetration = sphereRadius + distToFaces.y;
        } else {
            // Closest to Z face
            normal = glm::vec3(0.0f, 0.0f, sphereLocalPos.z > 0 ? 1.0f : -1.0f);
            penetration = sphereRadius + distToFaces.z;
        }
    } else {
        // Sphere center is outside box
        normal = localDistance / distanceLength;
        penetration = sphereRadius - distanceLength;
    }
    
    // Transform normal back to world space
    glm::vec3 worldNormal = CollisionUtils::transformVector(normal, boxRot);
    
    ContactManifold manifold(worldNormal, penetration);
    
    // Contact point on box surface
    glm::vec3 worldClosest = math::transformPoint(closestLocal, boxToWorld);
    ContactPoint contact(worldClosest);
    
    contact.localPositionA = worldClosest - boxPos;
    contact.localPositionB = worldClosest - spherePos;
    
    manifold.addContactPoint(contact);
    return manifold;
}

// Simplified box vs box - for full SAT implementation, see detectBoxVsBoxSAT
ContactManifold NarrowPhaseDetector::detectBoxVsBox(
    const BoxShape* boxA, const glm::vec3& posA, const glm::quat& rotA,
    const BoxShape* boxB, const glm::vec3& posB, const glm::quat& rotB) {
    
    // For now, use axis-aligned approximation
    // Full SAT implementation would be in detectBoxVsBoxSAT
    
    glm::vec3 distance = posB - posA;
    glm::vec3 absDistance = glm::abs(distance);
    glm::vec3 halfExtentsA = boxA->getHalfExtents();
    glm::vec3 halfExtentsB = boxB->getHalfExtents();
    glm::vec3 overlap = (halfExtentsA + halfExtentsB) - absDistance;
    
    // Check if there's overlap on all axes
    if (overlap.x <= 0.0f || overlap.y <= 0.0f || overlap.z <= 0.0f) {
        return ContactManifold(); // No collision
    }
    
    // Find axis of minimum penetration
    glm::vec3 normal;
    float penetration;
    
    if (overlap.x <= overlap.y && overlap.x <= overlap.z) {
        // X axis
        penetration = overlap.x;
        normal = glm::vec3(distance.x > 0 ? 1.0f : -1.0f, 0.0f, 0.0f);
    } else if (overlap.y <= overlap.z) {
        // Y axis
        penetration = overlap.y;
        normal = glm::vec3(0.0f, distance.y > 0 ? 1.0f : -1.0f, 0.0f);
    } else {
        // Z axis
        penetration = overlap.z;
        normal = glm::vec3(0.0f, 0.0f, distance.z > 0 ? 1.0f : -1.0f);
    }
    
    ContactManifold manifold(normal, penetration);
    
    // Simple contact point at the center of overlap
    glm::vec3 contactPos = posA + normal * halfExtentsA.x; // Simplified
    ContactPoint contact(contactPos);
    
    contact.localPositionA = contactPos - posA;
    contact.localPositionB = contactPos - posB;
    
    manifold.addContactPoint(contact);
    return manifold;
}

// Additional collision detection methods would go here...
ContactManifold NarrowPhaseDetector::detectSphereVsCapsule(
    const SphereShape* sphere, const glm::vec3& spherePos,
    const CapsuleShape* capsule, const glm::vec3& capsulePos, const glm::quat& capsuleRot) {
    
    // This is a placeholder implementation
    // Full implementation would calculate closest point on capsule line segment
    return ContactManifold();
}

ContactManifold NarrowPhaseDetector::detectSphereVsPlane(
    const SphereShape* sphere, const glm::vec3& spherePos,
    const PlaneShape* plane, const glm::vec3& planePos, const glm::quat& planeRot) {
    
    // This is a placeholder implementation
    return ContactManifold();
}

ContactManifold NarrowPhaseDetector::detectBoxVsPlane(
    const BoxShape* box, const glm::vec3& boxPos, const glm::quat& boxRot,
    const PlaneShape* plane, const glm::vec3& planePos, const glm::quat& planeRot) {
    
    // This is a placeholder implementation
    return ContactManifold();
}

ContactManifold NarrowPhaseDetector::detectCapsuleVsCapsule(
    const CapsuleShape* capsuleA, const glm::vec3& posA, const glm::quat& rotA,
    const CapsuleShape* capsuleB, const glm::vec3& posB, const glm::quat& rotB) {
    
    // This is a placeholder implementation
    return ContactManifold();
}

ContactManifold NarrowPhaseDetector::detectBoxVsBoxSAT(
    const BoxShape* boxA, const glm::vec3& posA, const glm::quat& rotA,
    const BoxShape* boxB, const glm::vec3& posB, const glm::quat& rotB) {
    
    // Full SAT implementation would go here
    // For now, fall back to simple version
    return detectBoxVsBox(boxA, posA, rotA, boxB, posB, rotB);
}

uint64_t NarrowPhaseDetector::makeShapeTypeKey(ShapeType typeA, ShapeType typeB) const {
    uint32_t a = static_cast<uint32_t>(typeA);
    uint32_t b = static_cast<uint32_t>(typeB);
    
    // Ensure consistent ordering (smaller type first)
    if (a > b) std::swap(a, b);
    
    return (static_cast<uint64_t>(a) << 32) | b;
}

void NarrowPhaseDetector::calculateMaterialProperties(ContactManifold& manifold, 
                                                     dynamics::RigidBody* bodyA, 
                                                     dynamics::RigidBody* bodyB) {
    // Combine material properties using physics material combination rules
    manifold.restitution = PhysicsMaterial::combineRestitution(
        bodyA->getPhysicsMaterial().get(), 
        bodyB->getPhysicsMaterial().get()
    );
    
    manifold.staticFriction = PhysicsMaterial::combineStaticFriction(
        bodyA->getPhysicsMaterial().get(), 
        bodyB->getPhysicsMaterial().get()
    );
    
    manifold.dynamicFriction = PhysicsMaterial::combineDynamicFriction(
        bodyA->getPhysicsMaterial().get(), 
        bodyB->getPhysicsMaterial().get()
    );
}

// === COLLISION UTILITIES IMPLEMENTATION ===

namespace CollisionUtils {

glm::vec3 closestPointOnLineSegment(const glm::vec3& point, 
                                   const glm::vec3& lineStart, 
                                   const glm::vec3& lineEnd) {
    glm::vec3 lineDir = lineEnd - lineStart;
    float lineLength = glm::length(lineDir);
    
    if (lineLength < math::constants::EPSILON) {
        return lineStart; // Degenerate line
    }
    
    lineDir /= lineLength;
    
    glm::vec3 toPoint = point - lineStart;
    float projectionLength = glm::dot(toPoint, lineDir);
    
    // Clamp to line segment
    projectionLength = math::clamp(projectionLength, 0.0f, lineLength);
    
    return lineStart + lineDir * projectionLength;
}

glm::vec3 clampPointToBox(const glm::vec3& point, const glm::vec3& halfExtents) {
    return glm::clamp(point, -halfExtents, halfExtents);
}

glm::vec3 transformPoint(const glm::vec3& point, 
                        const glm::vec3& position, 
                        const glm::quat& rotation) {
    return position + rotation * point;
}

glm::vec3 transformVector(const glm::vec3& vector, const glm::quat& rotation) {
    return rotation * vector;
}

} // namespace CollisionUtils

} // namespace collision
} // namespace physics
} // namespace ohao