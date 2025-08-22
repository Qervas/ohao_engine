#include "collision_detector.hpp"
#include "shapes/box_shape.hpp"
#include "shapes/sphere_shape.hpp"
#include "shapes/capsule_shape.hpp"
#include "shapes/cylinder_shape.hpp"
#include "shapes/plane_shape.hpp"
#include "shapes/triangle_mesh_shape.hpp"

namespace ohao {
namespace physics {
namespace collision {

ContactInfo CollisionDetector::detectCollision(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB) {
    ContactInfo contact;
    
    if (!bodyA || !bodyB) return contact;
    
    // Skip if both bodies are static
    if (bodyA->isStatic() && bodyB->isStatic()) {
        return contact;
    }
    
    // Broad phase check first (AABB overlap)
    if (!broadPhaseCheck(bodyA, bodyB)) {
        return contact;
    }
    
    // Narrow phase check (exact collision)
    auto shapeA = bodyA->getCollisionShape();
    auto shapeB = bodyB->getCollisionShape();
    
    if (!shapeA || !shapeB) {
        return contact;
    }
    
    
    contact = narrowPhaseCheck(
        shapeA.get(), bodyA->getPosition(), bodyA->getRotation(),
        shapeB.get(), bodyB->getPosition(), bodyB->getRotation()
    );
    
    // Set contact properties from body materials
    if (contact.hasContact) {
        contact.restitution = (bodyA->getRestitution() + bodyB->getRestitution()) * 0.5f;
        contact.friction = glm::sqrt(bodyA->getFriction() * bodyB->getFriction());
    }
    
    return contact;
}

bool CollisionDetector::broadPhaseCheck(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB) {
    math::AABB aabbA = bodyA->getAABB();
    math::AABB aabbB = bodyB->getAABB();
    
    return aabbA.intersects(aabbB);
}

ContactInfo CollisionDetector::narrowPhaseCheck(
    CollisionShape* shapeA, const glm::vec3& posA, const glm::quat& rotA,
    CollisionShape* shapeB, const glm::vec3& posB, const glm::quat& rotB) {
    
    ContactInfo contact;
    
    ShapeType typeA = shapeA->getType();
    ShapeType typeB = shapeB->getType();
    
    
    // Box vs Box
    if (typeA == ShapeType::BOX && typeB == ShapeType::BOX) {
        const BoxShape* boxA = static_cast<const BoxShape*>(shapeA);
        const BoxShape* boxB = static_cast<const BoxShape*>(shapeB);
        contact = testBoxVsBox(boxA, posA, rotA, boxB, posB, rotB);
        
    }
    // Sphere vs Sphere
    else if (typeA == ShapeType::SPHERE && typeB == ShapeType::SPHERE) {
        const SphereShape* sphereA = static_cast<const SphereShape*>(shapeA);
        const SphereShape* sphereB = static_cast<const SphereShape*>(shapeB);
        contact = testSphereVsSphere(sphereA, posA, sphereB, posB);
        
    }
    // Box vs Sphere (or Sphere vs Box)
    else if ((typeA == ShapeType::BOX && typeB == ShapeType::SPHERE) ||
             (typeA == ShapeType::SPHERE && typeB == ShapeType::BOX)) {
        
        if (typeA == ShapeType::BOX) {
            const BoxShape* box = static_cast<const BoxShape*>(shapeA);
            const SphereShape* sphere = static_cast<const SphereShape*>(shapeB);
            contact = testBoxVsSphere(box, posA, rotA, sphere, posB);
        } else {
            const SphereShape* sphere = static_cast<const SphereShape*>(shapeA);
            const BoxShape* box = static_cast<const BoxShape*>(shapeB);
            contact = testBoxVsSphere(box, posB, rotB, sphere, posA);
            if (contact.hasContact) {
                contact.flip(); // Reverse contact normal
            }
        }
        
    }
    // Sphere vs Capsule (or Capsule vs Sphere)
    else if ((typeA == ShapeType::SPHERE && typeB == ShapeType::CAPSULE) ||
             (typeA == ShapeType::CAPSULE && typeB == ShapeType::SPHERE)) {
        
        if (typeA == ShapeType::SPHERE) {
            const SphereShape* sphere = static_cast<const SphereShape*>(shapeA);
            const CapsuleShape* capsule = static_cast<const CapsuleShape*>(shapeB);
            contact = testSphereVsCapsule(sphere, posA, capsule, posB, rotB);
        } else {
            const CapsuleShape* capsule = static_cast<const CapsuleShape*>(shapeA);
            const SphereShape* sphere = static_cast<const SphereShape*>(shapeB);
            contact = testSphereVsCapsule(sphere, posB, capsule, posA, rotA);
            if (contact.hasContact) {
                contact.flip(); // Reverse contact normal
            }
        }
    }
    // Box vs Plane (or Plane vs Box)
    else if ((typeA == ShapeType::BOX && typeB == ShapeType::PLANE) ||
             (typeA == ShapeType::PLANE && typeB == ShapeType::BOX)) {
        
        if (typeA == ShapeType::BOX) {
            const BoxShape* box = static_cast<const BoxShape*>(shapeA);
            const PlaneShape* plane = static_cast<const PlaneShape*>(shapeB);
            contact = testBoxVsPlane(box, posA, rotA, plane, posB, rotB);
        } else {
            const PlaneShape* plane = static_cast<const PlaneShape*>(shapeA);
            const BoxShape* box = static_cast<const BoxShape*>(shapeB);
            contact = testBoxVsPlane(box, posB, rotB, plane, posA, rotA);
            if (contact.hasContact) {
                contact.flip(); // Reverse contact normal
            }
        }
    }
    // Sphere vs Plane (or Plane vs Sphere)
    else if ((typeA == ShapeType::SPHERE && typeB == ShapeType::PLANE) ||
             (typeA == ShapeType::PLANE && typeB == ShapeType::SPHERE)) {
        
        if (typeA == ShapeType::SPHERE) {
            const SphereShape* sphere = static_cast<const SphereShape*>(shapeA);
            const PlaneShape* plane = static_cast<const PlaneShape*>(shapeB);
            contact = testSphereVsPlane(sphere, posA, plane, posB, rotB);
        } else {
            const PlaneShape* plane = static_cast<const PlaneShape*>(shapeA);
            const SphereShape* sphere = static_cast<const SphereShape*>(shapeB);
            contact = testSphereVsPlane(sphere, posB, plane, posA, rotA);
            if (contact.hasContact) {
                contact.flip(); // Reverse contact normal
            }
        }
    }
    
    return contact;
}

ContactInfo CollisionDetector::testBoxVsBox(
    const BoxShape* boxA, const glm::vec3& posA, const glm::quat& rotA,
    const BoxShape* boxB, const glm::vec3& posB, const glm::quat& rotB) {
    
    // For now, assume axis-aligned boxes (no rotation)
    // TODO: Implement SAT (Separating Axis Theorem) for oriented boxes
    
    return createBoxBoxContact(posA, boxA->getHalfExtents(), posB, boxB->getHalfExtents());
}

ContactInfo CollisionDetector::testSphereVsSphere(
    const SphereShape* sphereA, const glm::vec3& posA,
    const SphereShape* sphereB, const glm::vec3& posB) {
    
    ContactInfo contact;
    
    glm::vec3 centerDistance = posB - posA;
    float distance = glm::length(centerDistance);
    float radiusSum = sphereA->getRadius() + sphereB->getRadius();
    
    if (distance < radiusSum && distance > math::constants::EPSILON) {
        contact.hasContact = true;
        contact.penetrationDepth = radiusSum - distance;
        contact.contactNormal = centerDistance / distance;
        contact.contactPoint = posA + contact.contactNormal * sphereA->getRadius();
    }
    
    return contact;
}

ContactInfo CollisionDetector::testBoxVsSphere(
    const BoxShape* box, const glm::vec3& boxPos, const glm::quat& boxRot,
    const SphereShape* sphere, const glm::vec3& spherePos) {
    
    ContactInfo contact;
    
    // Find closest point on box to sphere center
    glm::vec3 closestPoint = closestPointOnBox(spherePos, boxPos, box->getHalfExtents(), boxRot);
    
    glm::vec3 distance = spherePos - closestPoint;
    float distanceLength = glm::length(distance);
    
    if (distanceLength < sphere->getRadius() && distanceLength > math::constants::EPSILON) {
        contact.hasContact = true;
        contact.penetrationDepth = sphere->getRadius() - distanceLength;
        contact.contactNormal = distance / distanceLength;
        contact.contactPoint = closestPoint;
    }
    
    return contact;
}

glm::vec3 CollisionDetector::closestPointOnBox(
    const glm::vec3& point, 
    const glm::vec3& boxCenter, 
    const glm::vec3& boxHalfExtents,
    const glm::quat& boxRotation) {
    
    // Transform point to box local space
    glm::mat4 boxToWorld = math::createTransformMatrix(boxCenter, boxRotation);
    glm::mat4 worldToBox = glm::inverse(boxToWorld);
    glm::vec3 localPoint = math::transformPoint(point, worldToBox);
    
    // Clamp to box extents in local space
    glm::vec3 closestLocal = glm::clamp(localPoint, -boxHalfExtents, boxHalfExtents);
    
    // Transform back to world space
    return math::transformPoint(closestLocal, boxToWorld);
}

ContactInfo CollisionDetector::createBoxBoxContact(
    const glm::vec3& posA, const glm::vec3& halfExtentsA,
    const glm::vec3& posB, const glm::vec3& halfExtentsB) {
    
    ContactInfo contact;
    
    // Calculate overlap on each axis
    glm::vec3 distance = posB - posA;
    glm::vec3 absDistance = glm::abs(distance);
    glm::vec3 overlap = (halfExtentsA + halfExtentsB) - absDistance;
    
    // Check if there's overlap on all axes
    if (overlap.x <= 0.0f || overlap.y <= 0.0f || overlap.z <= 0.0f) {
        return contact; // No collision
    }
    
    contact.hasContact = true;
    
    // Find axis of minimum penetration
    if (overlap.x <= overlap.y && overlap.x <= overlap.z) {
        // X axis
        contact.penetrationDepth = overlap.x;
        contact.contactNormal = glm::vec3(distance.x > 0 ? 1.0f : -1.0f, 0.0f, 0.0f);
        contact.contactPoint = posA + glm::vec3(distance.x > 0 ? halfExtentsA.x : -halfExtentsA.x, 0.0f, 0.0f);
    }
    else if (overlap.y <= overlap.z) {
        // Y axis  
        contact.penetrationDepth = overlap.y;
        contact.contactNormal = glm::vec3(0.0f, distance.y > 0 ? 1.0f : -1.0f, 0.0f);
        contact.contactPoint = posA + glm::vec3(0.0f, distance.y > 0 ? halfExtentsA.y : -halfExtentsA.y, 0.0f);
    }
    else {
        // Z axis
        contact.penetrationDepth = overlap.z;
        contact.contactNormal = glm::vec3(0.0f, 0.0f, distance.z > 0 ? 1.0f : -1.0f);
        contact.contactPoint = posA + glm::vec3(0.0f, 0.0f, distance.z > 0 ? halfExtentsA.z : -halfExtentsA.z);
    }
    
    return contact;
}

// New collision test methods for additional shapes

ContactInfo CollisionDetector::testSphereVsCapsule(
    const SphereShape* sphere, const glm::vec3& spherePos,
    const CapsuleShape* capsule, const glm::vec3& capsulePos, const glm::quat& capsuleRot) {
    
    ContactInfo contact;
    
    // Get the capsule's line segment
    glm::vec3 capsuleStart, capsuleEnd;
    capsule->getLineSegment(capsulePos, capsuleRot, capsuleStart, capsuleEnd);
    
    // Find closest point on capsule's line segment to sphere center
    glm::vec3 closestPoint = capsule->closestPointOnLineSegment(spherePos, capsuleStart, capsuleEnd);
    
    // Calculate distance between sphere center and closest point on capsule
    glm::vec3 distance = spherePos - closestPoint;
    float distanceLength = glm::length(distance);
    float totalRadius = sphere->getRadius() + capsule->getRadius();
    
    if (distanceLength < totalRadius && distanceLength > math::constants::EPSILON) {
        contact.hasContact = true;
        contact.penetrationDepth = totalRadius - distanceLength;
        contact.contactNormal = distance / distanceLength;
        contact.contactPoint = closestPoint + contact.contactNormal * capsule->getRadius();
    }
    
    return contact;
}

ContactInfo CollisionDetector::testSphereVsPlane(
    const SphereShape* sphere, const glm::vec3& spherePos,
    const PlaneShape* plane, const glm::vec3& planePos, const glm::quat& planeRot) {
    
    ContactInfo contact;
    
    // Get signed distance from sphere center to plane
    float signedDistance = plane->getSignedDistanceToPoint(spherePos, planePos, planeRot);
    float sphereRadius = sphere->getRadius();
    
    // Check if sphere intersects plane (distance is less than radius)
    if (glm::abs(signedDistance) < sphereRadius) {
        contact.hasContact = true;
        
        // Get plane normal (always pointing "up" from the plane)
        glm::vec3 planeNormal = plane->getWorldNormal(planeRot);
        
        if (signedDistance >= 0.0f) {
            // Sphere is on the "positive" side of the plane
            contact.contactNormal = planeNormal;
            contact.penetrationDepth = sphereRadius - signedDistance;
        } else {
            // Sphere is on the "negative" side of the plane  
            contact.contactNormal = -planeNormal;
            contact.penetrationDepth = sphereRadius - glm::abs(signedDistance);
        }
        
        // Contact point is on the sphere surface closest to the plane
        contact.contactPoint = spherePos - contact.contactNormal * sphereRadius;
    }
    
    return contact;
}

ContactInfo CollisionDetector::testBoxVsPlane(
    const BoxShape* box, const glm::vec3& boxPos, const glm::quat& boxRot,
    const PlaneShape* plane, const glm::vec3& planePos, const glm::quat& planeRot) {
    
    ContactInfo contact;
    
    // Get plane normal in world space
    glm::vec3 planeNormal = plane->getWorldNormal(planeRot);
    
    // Get box half-extents
    glm::vec3 halfExtents = box->getHalfExtents();
    
    // Transform box axes to world space
    glm::vec3 boxAxisX = boxRot * glm::vec3(1, 0, 0);
    glm::vec3 boxAxisY = boxRot * glm::vec3(0, 1, 0);
    glm::vec3 boxAxisZ = boxRot * glm::vec3(0, 0, 1);
    
    // Calculate the projection of the box onto the plane normal
    float boxProjection = halfExtents.x * glm::abs(glm::dot(planeNormal, boxAxisX)) +
                         halfExtents.y * glm::abs(glm::dot(planeNormal, boxAxisY)) +
                         halfExtents.z * glm::abs(glm::dot(planeNormal, boxAxisZ));
    
    // Get distance from box center to plane
    float signedDistance = plane->getSignedDistanceToPoint(boxPos, planePos, planeRot);
    
    // Check if box intersects plane
    if (signedDistance < boxProjection) {
        contact.hasContact = true;
        contact.penetrationDepth = boxProjection - signedDistance;
        contact.contactNormal = planeNormal;
        
        // If box is behind the plane, flip the normal
        if (signedDistance < 0.0f) {
            contact.contactNormal = -contact.contactNormal;
            contact.penetrationDepth = boxProjection + glm::abs(signedDistance);
        }
        
        // Contact point is approximately at the box center projected onto the plane
        contact.contactPoint = plane->getClosestPointOnPlane(boxPos, planePos, planeRot);
    }
    
    return contact;
}

} // namespace collision
} // namespace physics
} // namespace ohao