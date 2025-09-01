#include "collision_system.hpp"
#include "physics/utils/physics_math.hpp"
#include "shapes/sphere_shape.hpp"
#include "shapes/box_shape.hpp"
#include <limits>

namespace ohao {
namespace physics {
namespace collision {

CollisionSystem::CollisionSystem() 
    : m_config{}
    , m_broadPhase(std::make_unique<BroadPhase>()) {
}

CollisionSystem::CollisionSystem(const Config& config)
    : m_config{config}
    , m_broadPhase(std::make_unique<BroadPhase>()) {
}

void CollisionSystem::setConfig(const Config& config) {
    m_config = config;
    
    // Update broad phase configuration
    if (m_broadPhase) {
        m_broadPhase->setAlgorithm(config.broadPhaseAlgorithm);
        m_broadPhase->setSpatialHashCellSize(config.spatialHashCellSize);
    }
}

void CollisionSystem::detectAndResolveCollisions(std::vector<dynamics::RigidBody*>& bodies, float deltaTime) {
    // Initialize contact cache if not exists
    if (!m_contactCache) {
        m_contactCache = std::make_unique<ContactCache>();
    }
    
    // Update contact cache (age manifolds, remove stale ones)
    m_contactCache->update(deltaTime);
    
    // === BROAD PHASE ===
    updateBroadPhase(bodies);
    
    // === NARROW PHASE ===  
    performNarrowPhase();
    
    // === CONSTRAINT SOLVING ===
    solveContactConstraints(deltaTime);
}

void CollisionSystem::resolveCollision(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                                     const glm::vec3& normal, float penetration,
                                     const glm::vec3& contactPoint, float deltaTime) {
    if (!bodyA || !bodyB) return;
    
    // === POSITION CORRECTION ===
    // Separate bodies to prevent overlap
    const float percent = 0.8f; // Position correction percentage
    const float slop = 0.01f;   // Allowed penetration
    
    if (penetration > slop) {
        float totalInvMass = bodyA->getInverseMass() + bodyB->getInverseMass();
        if (totalInvMass > 0.0f) {
            glm::vec3 correction = normal * (percent * (penetration - slop) / totalInvMass);
            
            // Move bodies apart based on their inverse masses
            if (!bodyA->isStatic()) {
                bodyA->setPosition(bodyA->getPosition() - correction * bodyA->getInverseMass());
            }
            if (!bodyB->isStatic()) {
                bodyB->setPosition(bodyB->getPosition() + correction * bodyB->getInverseMass());
            }
        }
    }
    
    // === VELOCITY RESOLUTION ===
    // Calculate relative velocity
    glm::vec3 relativeVelocity = bodyB->getLinearVelocity() - bodyA->getLinearVelocity();
    
    // Calculate relative velocity along collision normal
    float velocityAlongNormal = glm::dot(relativeVelocity, normal);
    
    // Do not resolve if velocities are separating
    if (velocityAlongNormal > 0) return;
    
    // Calculate restitution (bounciness)
    float restitution = 0.3f; // Default fallback value
    if (bodyA->getPhysicsMaterial() && bodyB->getPhysicsMaterial()) {
        restitution = (bodyA->getRestitution() + bodyB->getRestitution()) * 0.5f;
    }
    
    // Calculate impulse magnitude
    float impulseMagnitude = -(1 + restitution) * velocityAlongNormal;
    float totalInvMass = bodyA->getInverseMass() + bodyB->getInverseMass();
    
    if (totalInvMass > 0.0f) {
        impulseMagnitude /= totalInvMass;
        
        // Apply impulse
        glm::vec3 impulse = impulseMagnitude * normal;
        
        if (!bodyA->isStatic()) {
            bodyA->applyImpulse(-impulse, contactPoint - bodyA->getPosition());
        }
        if (!bodyB->isStatic()) {
            bodyB->applyImpulse(impulse, contactPoint - bodyB->getPosition());
        }
    }
    
    // === FRICTION ===
    // Apply friction force perpendicular to normal
    if (m_config.enableFriction) {
        // Recalculate relative velocity after normal impulse
        relativeVelocity = bodyB->getLinearVelocity() - bodyA->getLinearVelocity();
        
        // Get tangent vector (perpendicular to normal)
        glm::vec3 tangent = relativeVelocity - glm::dot(relativeVelocity, normal) * normal;
        
        if (glm::length(tangent) > 0.001f) {
            tangent = glm::normalize(tangent);
            
            // Calculate friction coefficients
            float staticFriction = 0.5f;
            float dynamicFriction = 0.3f;
            
            if (bodyA->getPhysicsMaterial() && bodyB->getPhysicsMaterial()) {
                staticFriction = (bodyA->getStaticFriction() + bodyB->getStaticFriction()) * 0.5f;
                dynamicFriction = (bodyA->getDynamicFriction() + bodyB->getDynamicFriction()) * 0.5f;
            }
            
            // Calculate friction impulse
            float frictionMagnitude = -glm::dot(relativeVelocity, tangent);
            if (totalInvMass > 0.0f) {
                frictionMagnitude /= totalInvMass;
            }
            
            // Apply Coulomb friction
            glm::vec3 frictionImpulse;
            if (std::abs(frictionMagnitude) < impulseMagnitude * staticFriction) {
                // Static friction
                frictionImpulse = frictionMagnitude * tangent;
            } else {
                // Dynamic friction
                frictionImpulse = -impulseMagnitude * dynamicFriction * tangent;
            }
            
            // Apply friction impulse
            if (!bodyA->isStatic()) {
                bodyA->applyImpulse(-frictionImpulse, contactPoint - bodyA->getPosition());
            }
            if (!bodyB->isStatic()) {
                bodyB->applyImpulse(frictionImpulse, contactPoint - bodyB->getPosition());
            }
        }
    }
}

void CollisionSystem::updateBroadPhase(const std::vector<dynamics::RigidBody*>& bodies) {
    if (!m_broadPhase) return;
    
    // Store bodies for ID mapping
    m_bodies = bodies;
    
    // Update the broad phase with all bodies
    m_broadPhase->update(bodies);
    
    // Ensure body IDs are assigned
    for (auto* body : bodies) {
        if (body) {
            getOrAssignBodyId(body);
        }
    }
}

void CollisionSystem::performNarrowPhase() {
    if (!m_broadPhase) return;
    
    // Initialize contact cache if not exists
    if (!m_contactCache) {
        m_contactCache = std::make_unique<ContactCache>();
    }
    
    // Clear previous frame data
    m_activeManifolds.clear();
    
    // Get collision pairs from broad phase
    const auto& pairs = m_broadPhase->getPotentialPairs();
    
    // Process each potential collision pair
    for (const auto& pair : pairs) {
        dynamics::RigidBody* bodyA = getBodyFromId(pair.bodyA);
        dynamics::RigidBody* bodyB = getBodyFromId(pair.bodyB);
        
        if (!bodyA || !bodyB) continue;
        
        // Get or create contact manifold for this pair
        ContactManifold* manifold = m_contactCache->getOrCreateManifold(bodyA, bodyB);
        if (!manifold) continue;
        
        // Store bodies and shapes in manifold
        manifold->bodyA = bodyA;
        manifold->bodyB = bodyB;
        manifold->shapeA = bodyA->getCollisionShape();
        manifold->shapeB = bodyB->getCollisionShape();
        
        if (!manifold->shapeA || !manifold->shapeB) continue;
        
        // Perform collision detection and generate contact points
        bool hasCollision = generateContactManifold(bodyA, bodyB, manifold);
        
        if (hasCollision && manifold->isValid()) {
            // Set material properties
            updateManifoldMaterialProperties(manifold);
            
            // Add to active manifolds for resolution
            m_activeManifolds.push_back(manifold);
            manifold->isActive = true;
            manifold->wasColliding = true;
        } else {
            manifold->isActive = false;
            manifold->wasColliding = false;
        }
    }
}

void CollisionSystem::resolveContacts(float deltaTime) {
    if (m_activeManifolds.empty()) return;
    
    // Resolve each contact manifold
    for (auto* manifold : m_activeManifolds) {
        if (!manifold || !manifold->isValid()) continue;
        
        // Warm start if enabled
        if (m_config.enableWarmStarting) {
            manifold->warmStart();
        }
        
        // Apply position correction (Baumgarte stabilization)
        if (manifold->penetration > m_config.slop) {
            const float correction = m_config.baumgarte * (manifold->penetration - m_config.slop);
            
            if (manifold->bodyA && manifold->bodyB) {
                const float totalInvMass = manifold->bodyA->getInverseMass() + manifold->bodyB->getInverseMass();
                if (totalInvMass > 0.0f) {
                    const glm::vec3 correctionVector = manifold->normal * (correction / totalInvMass);
                    
                    manifold->bodyA->setPosition(manifold->bodyA->getPosition() - correctionVector * manifold->bodyA->getInverseMass());
                    manifold->bodyB->setPosition(manifold->bodyB->getPosition() + correctionVector * manifold->bodyB->getInverseMass());
                }
            }
        }
        
        // Resolve velocity constraints for each contact point
        for (const auto& contact : manifold->points) {
            if (manifold->bodyA && manifold->bodyB) {
                // Calculate relative velocity at contact point
                glm::vec3 relVelocity = manifold->bodyB->getLinearVelocity() - manifold->bodyA->getLinearVelocity();
                
                // Add angular velocity contribution
                if (manifold->bodyA->getInverseInertiaTensor() != glm::mat3(0.0f)) {
                    glm::vec3 rA = contact.position - manifold->bodyA->getPosition();
                    relVelocity -= glm::cross(manifold->bodyA->getAngularVelocity(), rA);
                }
                
                if (manifold->bodyB->getInverseInertiaTensor() != glm::mat3(0.0f)) {
                    glm::vec3 rB = contact.position - manifold->bodyB->getPosition();
                    relVelocity += glm::cross(manifold->bodyB->getAngularVelocity(), rB);
                }
                
                // Calculate normal impulse
                float normalVelocity = glm::dot(relVelocity, manifold->normal);
                if (normalVelocity > 0.0f) { // Objects are separating
                    continue;
                }
                
                float restitution = manifold->restitution;
                float targetVelocity = -restitution * normalVelocity;
                
                float deltaVelocity = targetVelocity - normalVelocity;
                float totalInvMass = manifold->bodyA->getInverseMass() + manifold->bodyB->getInverseMass();
                
                if (totalInvMass > 0.0f) {
                    float impulse = deltaVelocity / totalInvMass;
                    glm::vec3 impulseVector = manifold->normal * impulse;
                    
                    // Apply impulse
                    manifold->bodyA->applyImpulse(-impulseVector);
                    manifold->bodyB->applyImpulse(impulseVector);
                }
            }
        }
    }
}

dynamics::RigidBody* CollisionSystem::getBodyFromId(uint32_t id) const {
    // Simple implementation - in a real system this would use a more efficient lookup
    for (const auto& pair : m_bodyIds) {
        if (pair.second == id) {
            return pair.first;
        }
    }
    return nullptr;
}

bool CollisionSystem::detectCollisionBetweenShapes(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                                                   CollisionShape* shapeA, CollisionShape* shapeB,
                                                   glm::vec3& normal, float& penetration, glm::vec3& contactPoint) {
    if (!bodyA || !bodyB || !shapeA || !shapeB) return false;
    
    ShapeType typeA = shapeA->getType();
    ShapeType typeB = shapeB->getType();
    
    // Handle different shape combinations
    if (typeA == ShapeType::SPHERE && typeB == ShapeType::SPHERE) {
        return detectSphereSphere(bodyA, bodyB, shapeA, shapeB, normal, penetration, contactPoint);
    }
    else if (typeA == ShapeType::BOX && typeB == ShapeType::BOX) {
        return detectBoxBox(bodyA, bodyB, shapeA, shapeB, normal, penetration, contactPoint);
    }
    else if ((typeA == ShapeType::SPHERE && typeB == ShapeType::BOX) ||
             (typeA == ShapeType::BOX && typeB == ShapeType::SPHERE)) {
        return detectSphereBox(bodyA, bodyB, shapeA, shapeB, normal, penetration, contactPoint);
    }
    else {
        // Fallback: try to get actual shape dimensions
        float radiusA = 0.5f; // Default fallback
        float radiusB = 0.5f; // Default fallback
        
        // Try to get actual dimensions from shapes
        auto* sphereA = dynamic_cast<SphereShape*>(shapeA);
        auto* sphereB = dynamic_cast<SphereShape*>(shapeB);
        auto* boxA = dynamic_cast<BoxShape*>(shapeA);
        auto* boxB = dynamic_cast<BoxShape*>(shapeB);
        
        if (sphereA) {
            radiusA = sphereA->getRadius();
        } else if (boxA) {
            // Use half the average extent as radius approximation
            glm::vec3 extents = boxA->getHalfExtents();
            radiusA = (extents.x + extents.y + extents.z) / 3.0f;
        }
        
        if (sphereB) {
            radiusB = sphereB->getRadius();
        } else if (boxB) {
            // Use half the average extent as radius approximation
            glm::vec3 extents = boxB->getHalfExtents();
            radiusB = (extents.x + extents.y + extents.z) / 3.0f;
        }
        
        glm::vec3 posA = bodyA->getPosition();
        glm::vec3 posB = bodyB->getPosition();
        glm::vec3 direction = posB - posA;
        float distance = glm::length(direction);
        float combinedRadius = radiusA + radiusB;
        
        if (distance < combinedRadius) {
            penetration = combinedRadius - distance;
            
            if (distance > 0.001f) {
                normal = glm::normalize(direction);
                contactPoint = posA + normal * radiusA;
            } else {
                normal = glm::vec3(1.0f, 0.0f, 0.0f);
                contactPoint = posA;
            }
            return true;
        }
    }
    
    return false;
}

bool CollisionSystem::detectSphereSphere(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                                         CollisionShape* shapeA, CollisionShape* shapeB,
                                         glm::vec3& normal, float& penetration, glm::vec3& contactPoint) {
    // Cast to sphere shapes to get actual radius
    auto* sphereA = dynamic_cast<SphereShape*>(shapeA);
    auto* sphereB = dynamic_cast<SphereShape*>(shapeB);
    
    float radiusA = sphereA ? sphereA->getRadius() : 0.5f;
    float radiusB = sphereB ? sphereB->getRadius() : 0.5f;
    
    glm::vec3 posA = bodyA->getPosition();
    glm::vec3 posB = bodyB->getPosition();
    
    glm::vec3 direction = posB - posA;
    float distance = glm::length(direction);
    float combinedRadius = radiusA + radiusB;
    
    if (distance < combinedRadius) {
        penetration = combinedRadius - distance;
        
        if (distance > 0.001f) {
            normal = glm::normalize(direction);
            contactPoint = posA + normal * radiusA;
        } else {
            normal = glm::vec3(1.0f, 0.0f, 0.0f);
            contactPoint = posA;
        }
        return true;
    }
    
    return false;
}

bool CollisionSystem::detectBoxBox(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                                  CollisionShape* shapeA, CollisionShape* shapeB,
                                  glm::vec3& normal, float& penetration, glm::vec3& contactPoint) {
    // Simplified box-box collision using AABB
    math::AABB aabbA = shapeA->getAABB(bodyA->getPosition(), bodyA->getRotation());
    math::AABB aabbB = shapeB->getAABB(bodyB->getPosition(), bodyB->getRotation());
    
    // Check if AABBs overlap
    if (!aabbA.intersects(aabbB)) {
        return false;
    }
    
    // Calculate overlap on each axis
    glm::vec3 overlapMin = aabbA.min - aabbB.max;
    glm::vec3 overlapMax = aabbA.max - aabbB.min;
    
    // Find minimum overlap axis
    float minOverlap = std::numeric_limits<float>::max();
    int minAxis = 0;
    
    for (int i = 0; i < 3; ++i) {
        float overlap = std::min(-overlapMin[i], overlapMax[i]);
        if (overlap < minOverlap) {
            minOverlap = overlap;
            minAxis = i;
        }
    }
    
    penetration = minOverlap;
    normal = glm::vec3(0.0f);
    normal[minAxis] = (overlapMax[minAxis] > -overlapMin[minAxis]) ? 1.0f : -1.0f;
    
    // Contact point is on the surface of the first box
    contactPoint = bodyA->getPosition();
    contactPoint[minAxis] += normal[minAxis] * (aabbA.getSize()[minAxis] * 0.5f);
    
    return true;
}

bool CollisionSystem::detectSphereBox(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                                     CollisionShape* shapeA, CollisionShape* shapeB,
                                     glm::vec3& normal, float& penetration, glm::vec3& contactPoint) {
    // Determine which is sphere and which is box
    dynamics::RigidBody* sphere = nullptr;
    dynamics::RigidBody* box = nullptr;
    CollisionShape* sphereShape = nullptr;
    CollisionShape* boxShape = nullptr;
    
    if (shapeA->getType() == ShapeType::SPHERE) {
        sphere = bodyA;
        box = bodyB;
        sphereShape = shapeA;
        boxShape = shapeB;
    } else {
        sphere = bodyB;
        box = bodyA;
        sphereShape = shapeB;
        boxShape = shapeA;
    }
    
    // Get sphere radius from actual shape
    auto* sphereCast = dynamic_cast<SphereShape*>(sphereShape);
    float sphereRadius = sphereCast ? sphereCast->getRadius() : 0.5f;
    
    // Get box AABB
    math::AABB boxAABB = boxShape->getAABB(box->getPosition(), box->getRotation());
    
    // Find closest point on box to sphere center
    glm::vec3 spherePos = sphere->getPosition();
    glm::vec3 closestPoint = boxAABB.getClosestPoint(spherePos);
    
    // Distance from sphere center to closest point on box
    glm::vec3 toSphere = spherePos - closestPoint;
    float distance = glm::length(toSphere);
    
    if (distance < sphereRadius) {
        penetration = sphereRadius - distance;
        
        if (distance > 0.001f) {
            normal = glm::normalize(toSphere);
        } else {
            // Sphere center is inside box, use upward normal as default
            normal = glm::vec3(0.0f, 1.0f, 0.0f);
        }
        
        // Adjust normal direction based on which body is which
        if (shapeB->getType() == ShapeType::SPHERE) {
            normal = -normal;
        }
        
        contactPoint = closestPoint;
        return true;
    }
    
    return false;
}

bool CollisionSystem::generateContactManifold(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB, ContactManifold* manifold) {
    if (!bodyA || !bodyB || !manifold) return false;
    
    ShapeType typeA = manifold->shapeA->getType();
    ShapeType typeB = manifold->shapeB->getType();
    
    // Clear old contact points - new ones will be generated
    manifold->points.clear();
    
    // Detect collision and generate contact points based on shape types
    bool hasCollision = false;
    
    if (typeA == ShapeType::SPHERE && typeB == ShapeType::SPHERE) {
        hasCollision = generateSphereSphereManifold(bodyA, bodyB, manifold);
    }
    else if (typeA == ShapeType::BOX && typeB == ShapeType::BOX) {
        hasCollision = generateBoxBoxManifold(bodyA, bodyB, manifold);
    }
    else if ((typeA == ShapeType::SPHERE && typeB == ShapeType::BOX) ||
             (typeA == ShapeType::BOX && typeB == ShapeType::SPHERE)) {
        hasCollision = generateSphereBoxManifold(bodyA, bodyB, manifold);
    }
    else {
        // Fallback to single contact point
        glm::vec3 normal, contactPoint;
        float penetration;
        hasCollision = detectCollisionBetweenShapes(
            bodyA, bodyB, manifold->shapeA.get(), manifold->shapeB.get(),
            normal, penetration, contactPoint
        );
        
        if (hasCollision) {
            manifold->normal = normal;
            manifold->penetration = penetration;
            
            ContactPoint point(contactPoint);
            point.localPositionA = contactPoint - bodyA->getPosition();
            point.localPositionB = contactPoint - bodyB->getPosition();
            manifold->addContactPoint(point);
        }
    }
    
    if (hasCollision && !manifold->points.empty()) {
        manifold->updateTangents();
        return true;
    }
    
    return false;
}

bool CollisionSystem::generateSphereSphereManifold(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB, ContactManifold* manifold) {
    // Cast to sphere shapes to get actual radius
    auto sphereA = std::dynamic_pointer_cast<SphereShape>(manifold->shapeA);
    auto sphereB = std::dynamic_pointer_cast<SphereShape>(manifold->shapeB);
    
    float radiusA = sphereA ? sphereA->getRadius() : 0.5f; // Fallback to default
    float radiusB = sphereB ? sphereB->getRadius() : 0.5f; // Fallback to default
    
    glm::vec3 posA = bodyA->getPosition();
    glm::vec3 posB = bodyB->getPosition();
    
    glm::vec3 direction = posB - posA;
    float distance = glm::length(direction);
    float combinedRadius = radiusA + radiusB;
    
    if (distance < combinedRadius) {
        float penetration = combinedRadius - distance;
        glm::vec3 normal = distance > 0.001f ? glm::normalize(direction) : glm::vec3(1.0f, 0.0f, 0.0f);
        
        manifold->normal = normal;
        manifold->penetration = penetration;
        
        glm::vec3 contactPoint = posA + normal * radiusA;
        ContactPoint point(contactPoint);
        point.localPositionA = contactPoint - posA;
        point.localPositionB = contactPoint - posB;
        manifold->addContactPoint(point);
        
        return true;
    }
    
    return false;
}

bool CollisionSystem::generateBoxBoxManifold(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB, ContactManifold* manifold) {
    math::AABB aabbA = manifold->shapeA->getAABB(bodyA->getPosition(), bodyA->getRotation());
    math::AABB aabbB = manifold->shapeB->getAABB(bodyB->getPosition(), bodyB->getRotation());
    
    if (!aabbA.intersects(aabbB)) {
        return false;
    }
    
    // Calculate overlap and find separation axis
    glm::vec3 overlapMin = aabbA.min - aabbB.max;
    glm::vec3 overlapMax = aabbA.max - aabbB.min;
    
    float minOverlap = std::numeric_limits<float>::max();
    int minAxis = 0;
    
    for (int i = 0; i < 3; ++i) {
        float overlap = std::min(-overlapMin[i], overlapMax[i]);
        if (overlap < minOverlap) {
            minOverlap = overlap;
            minAxis = i;
        }
    }
    
    manifold->penetration = minOverlap;
    manifold->normal = glm::vec3(0.0f);
    manifold->normal[minAxis] = (overlapMax[minAxis] > -overlapMin[minAxis]) ? 1.0f : -1.0f;
    
    // Generate contact points at intersection corners
    math::AABB intersection;
    intersection.min = glm::max(aabbA.min, aabbB.min);
    intersection.max = glm::min(aabbA.max, aabbB.max);
    
    // Create up to 4 contact points
    std::vector<glm::vec3> corners = {
        glm::vec3(intersection.min.x, intersection.min.y, intersection.min.z),
        glm::vec3(intersection.max.x, intersection.min.y, intersection.min.z),
        glm::vec3(intersection.min.x, intersection.max.y, intersection.min.z),
        glm::vec3(intersection.min.x, intersection.min.y, intersection.max.z)
    };
    
    for (const auto& corner : corners) {
        if (manifold->points.size() >= ContactManifold::MAX_CONTACT_POINTS) break;
        
        ContactPoint point(corner);
        point.localPositionA = corner - bodyA->getPosition();
        point.localPositionB = corner - bodyB->getPosition();
        manifold->addContactPoint(point);
    }
    
    return !manifold->points.empty();
}

bool CollisionSystem::generateSphereBoxManifold(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB, ContactManifold* manifold) {
    // Determine which is sphere and which is box
    dynamics::RigidBody* sphere = nullptr;
    dynamics::RigidBody* box = nullptr;
    bool sphereIsA = (manifold->shapeA->getType() == ShapeType::SPHERE);
    
    if (sphereIsA) {
        sphere = bodyA;
        box = bodyB;
    } else {
        sphere = bodyB;
        box = bodyA;
    }
    
    // Get sphere radius from actual shape
    auto sphereShapePtr = (sphereIsA ? manifold->shapeA : manifold->shapeB);
    auto* sphereCast = dynamic_cast<SphereShape*>(sphereShapePtr.get());
    float sphereRadius = sphereCast ? sphereCast->getRadius() : 0.5f;
    math::AABB boxAABB = (sphereIsA ? manifold->shapeB : manifold->shapeA)->getAABB(
        box->getPosition(), box->getRotation());
    
    glm::vec3 spherePos = sphere->getPosition();
    glm::vec3 closestPoint = boxAABB.getClosestPoint(spherePos);
    
    glm::vec3 toSphere = spherePos - closestPoint;
    float distance = glm::length(toSphere);
    
    if (distance < sphereRadius) {
        float penetration = sphereRadius - distance;
        glm::vec3 normal = distance > 0.001f ? glm::normalize(toSphere) : glm::vec3(0.0f, 1.0f, 0.0f);
        
        // Adjust normal direction
        if (!sphereIsA) {
            normal = -normal;
        }
        
        manifold->normal = normal;
        manifold->penetration = penetration;
        
        ContactPoint point(closestPoint);
        point.localPositionA = closestPoint - bodyA->getPosition();
        point.localPositionB = closestPoint - bodyB->getPosition();
        manifold->addContactPoint(point);
        
        return true;
    }
    
    return false;
}

void CollisionSystem::updateManifoldMaterialProperties(ContactManifold* manifold) {
    if (!manifold || !manifold->bodyA || !manifold->bodyB) return;
    
    // Get material properties from bodies
    float restitutionA = manifold->bodyA->getRestitution();
    float restitutionB = manifold->bodyB->getRestitution();
    float staticFrictionA = manifold->bodyA->getStaticFriction();
    float staticFrictionB = manifold->bodyB->getStaticFriction();
    float dynamicFrictionA = manifold->bodyA->getDynamicFriction();
    float dynamicFrictionB = manifold->bodyB->getDynamicFriction();
    
    // Combine material properties
    manifold->restitution = std::sqrt(restitutionA * restitutionB);
    manifold->staticFriction = std::min(staticFrictionA, staticFrictionB);
    manifold->dynamicFriction = std::min(dynamicFrictionA, dynamicFrictionB);
}

void CollisionSystem::solveContactConstraints(float deltaTime) {
    if (m_activeManifolds.empty()) return;
    
    const int maxIterations = m_config.maxIterations;
    
    // Warm start all manifolds
    for (auto* manifold : m_activeManifolds) {
        if (manifold && manifold->wasColliding) {
            manifold->warmStart();
        }
    }
    
    // Iterative constraint solver
    for (int iteration = 0; iteration < maxIterations; ++iteration) {
        for (auto* manifold : m_activeManifolds) {
            if (!manifold || !manifold->isValid()) continue;
            
            solveManifoldConstraints(manifold, deltaTime);
        }
    }
    
    // Apply position correction
    for (auto* manifold : m_activeManifolds) {
        if (!manifold || !manifold->isValid()) continue;
        
        applyPositionCorrection(manifold);
    }
}

void CollisionSystem::solveManifoldConstraints(ContactManifold* manifold, float deltaTime) {
    if (!manifold || !manifold->bodyA || !manifold->bodyB) return;
    
    dynamics::RigidBody* bodyA = manifold->bodyA;
    dynamics::RigidBody* bodyB = manifold->bodyB;
    
    for (auto& contact : manifold->points) {
        // Calculate relative velocity at contact point
        glm::vec3 relativeVelocity = bodyB->getLinearVelocity() - bodyA->getLinearVelocity();
        
        // Add angular velocity contributions
        if (!bodyA->isStatic()) {
            relativeVelocity -= glm::cross(bodyA->getAngularVelocity(), contact.localPositionA);
        }
        if (!bodyB->isStatic()) {
            relativeVelocity += glm::cross(bodyB->getAngularVelocity(), contact.localPositionB);
        }
        
        // Normal impulse
        float normalVelocity = glm::dot(relativeVelocity, manifold->normal);
        
        // Do not resolve if velocities are separating
        if (normalVelocity > 0.0f) {
            continue;
        }
        
        float restitution = manifold->restitution;
        float targetVelocity = -restitution * normalVelocity;
        float deltaVelocity = targetVelocity - normalVelocity;
        
        float totalInvMass = bodyA->getInverseMass() + bodyB->getInverseMass();
        if (totalInvMass > 0.0f) {
            float impulse = deltaVelocity / totalInvMass;
            glm::vec3 impulseVector = manifold->normal * impulse;
            
            // Apply impulse
            if (!bodyA->isStatic()) {
                bodyA->applyImpulse(-impulseVector, contact.localPositionA);
            }
            if (!bodyB->isStatic()) {
                bodyB->applyImpulse(impulseVector, contact.localPositionB);
            }
            
            contact.normalImpulse += impulse;
        }
        
        // Friction impulses
        if (m_config.enableFriction) {
            solveFrictionConstraints(manifold, contact, relativeVelocity);
        }
    }
}

void CollisionSystem::solveFrictionConstraints(ContactManifold* manifold, ContactPoint& contact, const glm::vec3& relativeVelocity) {
    dynamics::RigidBody* bodyA = manifold->bodyA;
    dynamics::RigidBody* bodyB = manifold->bodyB;
    
    float tangentVel1 = glm::dot(relativeVelocity, manifold->tangent1);
    float tangentVel2 = glm::dot(relativeVelocity, manifold->tangent2);
    
    float totalInvMass = bodyA->getInverseMass() + bodyB->getInverseMass();
    if (totalInvMass <= 0.0f) return;
    
    float friction1 = -tangentVel1 / totalInvMass;
    float friction2 = -tangentVel2 / totalInvMass;
    
    // Apply Coulomb friction limit
    float maxFriction = manifold->staticFriction * std::abs(contact.normalImpulse);
    float frictionMag = std::sqrt(friction1 * friction1 + friction2 * friction2);
    
    if (frictionMag > maxFriction) {
        friction1 *= maxFriction / frictionMag;
        friction2 *= maxFriction / frictionMag;
    }
    
    // Apply friction impulses
    glm::vec3 frictionImpulse = friction1 * manifold->tangent1 + friction2 * manifold->tangent2;
    
    if (!bodyA->isStatic()) {
        bodyA->applyImpulse(-frictionImpulse, contact.localPositionA);
    }
    if (!bodyB->isStatic()) {
        bodyB->applyImpulse(frictionImpulse, contact.localPositionB);
    }
    
    contact.tangentImpulse1 += friction1;
    contact.tangentImpulse2 += friction2;
}

void CollisionSystem::applyPositionCorrection(ContactManifold* manifold) {
    if (!manifold || manifold->penetration <= m_config.slop) return;
    
    dynamics::RigidBody* bodyA = manifold->bodyA;
    dynamics::RigidBody* bodyB = manifold->bodyB;
    
    const float correction = m_config.baumgarte * (manifold->penetration - m_config.slop);
    const float totalInvMass = bodyA->getInverseMass() + bodyB->getInverseMass();
    
    if (totalInvMass > 0.0f) {
        glm::vec3 correctionVector = manifold->normal * (correction / totalInvMass);
        
        if (!bodyA->isStatic()) {
            bodyA->setPosition(bodyA->getPosition() - correctionVector * bodyA->getInverseMass());
        }
        if (!bodyB->isStatic()) {
            bodyB->setPosition(bodyB->getPosition() + correctionVector * bodyB->getInverseMass());
        }
    }
}

uint32_t CollisionSystem::getOrAssignBodyId(dynamics::RigidBody* body) {
    auto it = m_bodyIds.find(body);
    if (it != m_bodyIds.end()) {
        return it->second;
    }
    
    uint32_t id = m_nextBodyId++;
    m_bodyIds[body] = id;
    return id;
}

} // namespace collision
} // namespace physics
} // namespace ohao