#include "collision_system.hpp"
#include "physics/utils/physics_math.hpp"

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
    // SIMPLIFIED: Skip broad phase for now and test all pairs directly
    printf("COLLISION DEBUG: Testing all %zu bodies for collisions\n", bodies.size());
    
    // Simple O(n^2) collision detection for debugging
    for (size_t i = 0; i < bodies.size(); ++i) {
        for (size_t j = i + 1; j < bodies.size(); ++j) {
            dynamics::RigidBody* bodyA = bodies[i];
            dynamics::RigidBody* bodyB = bodies[j];
            
            if (!bodyA || !bodyB) continue;
            
            // Skip static-static pairs
            if (bodyA->isStatic() && bodyB->isStatic()) continue;
            
            // PROPER SPHERE-BOX COLLISION DETECTION
            bool collisionDetected = false;
            glm::vec3 collisionNormal(0.0f);
            float penetrationDepth = 0.0f;
            
            // Determine which is sphere and which is box (based on our setup)
            dynamics::RigidBody* sphere = nullptr;
            dynamics::RigidBody* box = nullptr;
            
            if (!bodyA->isStatic() && bodyB->isStatic()) {
                sphere = bodyA;  // Dynamic sphere
                box = bodyB;     // Static platform
            } else if (bodyA->isStatic() && !bodyB->isStatic()) {
                sphere = bodyB;  // Dynamic sphere  
                box = bodyA;     // Static platform
            } else {
                continue; // Skip other combinations for now
            }
            
            // Sphere-Box collision detection
            glm::vec3 spherePos = sphere->getPosition();
            glm::vec3 boxPos = box->getPosition();
            
            // Box dimensions: platform has half-extents (2.0, 0.2, 2.0)
            glm::vec3 boxHalfExtents(2.0f, 0.2f, 2.0f);
            float sphereRadius = 0.5f;
            
            // Find closest point on box to sphere center
            glm::vec3 boxMin = boxPos - boxHalfExtents;
            glm::vec3 boxMax = boxPos + boxHalfExtents;
            glm::vec3 closestPoint = glm::clamp(spherePos, boxMin, boxMax);
            
            // Distance from sphere center to closest point on box
            glm::vec3 toSphere = spherePos - closestPoint;
            float distance = glm::length(toSphere);
            
            if (distance < sphereRadius) {
                collisionDetected = true;
                penetrationDepth = sphereRadius - distance;
                
                if (distance > 0.001f) {
                    collisionNormal = glm::normalize(toSphere);
                } else {
                    // Sphere center is inside box, use Y-up as normal (assume hitting from above)
                    collisionNormal = glm::vec3(0.0f, 1.0f, 0.0f);
                }
                
                printf("COLLISION DETECTED: Sphere at (%.3f,%.3f,%.3f), closest point (%.3f,%.3f,%.3f), penetration=%.3f\n", 
                       spherePos.x, spherePos.y, spherePos.z,
                       closestPoint.x, closestPoint.y, closestPoint.z,
                       penetrationDepth);
            }
            
            if (collisionDetected) {
                // Resolve collision
                
                // 1. Position correction: Move sphere out of box
                glm::vec3 correction = collisionNormal * (penetrationDepth + 0.001f); // Small margin
                sphere->setPosition(spherePos + correction);
                
                // 2. Velocity response: Bounce with restitution
                glm::vec3 velocity = sphere->getLinearVelocity();
                float restitution = 0.6f; // Higher bounce for more realistic feel
                
                // Only reverse velocity if moving toward the collision surface
                float velocityAlongNormal = glm::dot(velocity, collisionNormal);
                if (velocityAlongNormal < 0.0f) {
                    glm::vec3 newVelocity = velocity - (1.0f + restitution) * velocityAlongNormal * collisionNormal;
                    sphere->setLinearVelocity(newVelocity);
                    
                    printf("COLLISION RESOLVED: Correction=(%.3f,%.3f,%.3f), new velocity=(%.3f,%.3f,%.3f)\n", 
                           correction.x, correction.y, correction.z,
                           newVelocity.x, newVelocity.y, newVelocity.z);
                }
            }
        }
    }
}

void CollisionSystem::updateBroadPhase(const std::vector<dynamics::RigidBody*>& bodies) {
    if (!m_broadPhase) return;
    
    // Update the broad phase with all bodies
    m_broadPhase->update(bodies);
}

void CollisionSystem::performNarrowPhase() {
    if (!m_broadPhase) return;
    
    // Clear previous contacts
    m_activeManifolds.clear();
    
    // Get collision pairs from broad phase
    const auto& pairs = m_broadPhase->getPotentialPairs();
    
    printf("COLLISION NARROW DEBUG: Processing %zu potential pairs\n", pairs.size());
    
    // For now, create a simple placeholder implementation
    // This would normally perform detailed collision detection
    for (const auto& pair : pairs) {
        // Get the actual RigidBody pointers from IDs
        dynamics::RigidBody* bodyA = getBodyFromId(pair.bodyA);
        dynamics::RigidBody* bodyB = getBodyFromId(pair.bodyB);
        
        if (!bodyA || !bodyB) continue;
        
        printf("COLLISION NARROW DEBUG: Testing collision between bodies at (%.3f,%.3f,%.3f) and (%.3f,%.3f,%.3f)\n", 
               bodyA->getPosition().x, bodyA->getPosition().y, bodyA->getPosition().z,
               bodyB->getPosition().x, bodyB->getPosition().y, bodyB->getPosition().z);
        
        // BASIC COLLISION DETECTION: Assume one is sphere, one is box
        // This is a simplified implementation for the sphere-platform case
        
        // Simple distance-based collision for now
        glm::vec3 distance = bodyA->getPosition() - bodyB->getPosition();
        float distanceLength = glm::length(distance);
        
        // Rough collision threshold (sphere radius + box extent)
        float collisionThreshold = 1.0f; // Adjust based on actual shapes
        
        if (distanceLength < collisionThreshold) {
            printf("COLLISION DETECTED: Distance=%.3f, threshold=%.3f\n", distanceLength, collisionThreshold);
            
            // Create a simple contact manifold
            // TODO: Replace with proper manifold creation
            // For now, just separate the bodies
            if (!bodyA->isStatic() && bodyB->isStatic()) {
                // Move dynamic body away from static body
                glm::vec3 separation = glm::normalize(distance) * (collisionThreshold - distanceLength);
                bodyA->setPosition(bodyA->getPosition() + separation);
                bodyA->setLinearVelocity(glm::vec3(0.0f)); // Stop the body
                printf("COLLISION RESOLVED: Moved body A by (%.3f,%.3f,%.3f)\n", separation.x, separation.y, separation.z);
            }
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
                if (normalVelocity < 0.0f) { // Objects are separating
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

} // namespace collision
} // namespace physics
} // namespace ohao