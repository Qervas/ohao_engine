#include "physics_world.hpp"
#include "rigid_body.hpp"
#include "../../ui/components/console_widget.hpp"
#include "../../ui/panels/viewport/viewport_toolbar.hpp"
#include <memory>
#include <string>

namespace ohao {

PhysicsWorld::PhysicsWorld() {
    // Initialize simulation state to STOPPED
    m_simulationState = PhysicsSimulationState::STOPPED;
}

PhysicsWorld::~PhysicsWorld() {
    cleanup();
}

bool PhysicsWorld::initialize(const PhysicsSettings& settings) {
    if (m_initialized) {
        OHAO_LOG_WARNING("Physics world already initialized");
        return true;
    }
    
    m_settings = settings;
    
    // TODO: Initialize Bullet Physics
    // Example structure:
    // initializeBulletPhysics();
    
    m_initialized = true;
    OHAO_LOG("Physics world initialized");
    return true;
}

void PhysicsWorld::cleanup() {
    if (!m_initialized) {
        return;
    }
    
    // TODO: Cleanup rigid bodies
    m_rigidBodies.clear();
    
    // TODO: Cleanup Bullet Physics
    // cleanupBulletPhysics();
    
    m_initialized = false;
    OHAO_LOG("Physics world cleaned up");
}

void PhysicsWorld::stepSimulation(float deltaTime) {
    if (!m_initialized) {
        return;

    }

    // Debug: Log simulation step
    static int stepCount = 0;
    if (stepCount % 60 == 0) {
        printf("=== Physics Step %d - Bodies: %zu, Gravity: (%.2f, %.2f, %.2f) ===\n", 
               stepCount, m_rigidBodies.size(), m_settings.gravity.x, m_settings.gravity.y, m_settings.gravity.z);
        
        for (size_t i = 0; i < m_rigidBodies.size(); ++i) {
            auto& body = m_rigidBodies[i];
            if (body) {
                glm::vec3 pos = body->getPosition();
                glm::vec3 vel = body->getLinearVelocity();
                printf("  Body %zu: pos(%.3f, %.3f, %.3f) vel(%.3f, %.3f, %.3f) mass:%.2f type:%d\n", 
                       i, pos.x, pos.y, pos.z, vel.x, vel.y, vel.z, body->getMass(), (int)body->getType());
            }
        }
    }
    stepCount++;

    for (auto& rigidBody : m_rigidBodies){
        if(!rigidBody) continue;

        // skip static objects
        if(rigidBody->getType() == RigidBodyType::STATIC){
            continue;
        }
    
        // Apply gravity as a force
        if(rigidBody->getType() == RigidBodyType::DYNAMIC){
            glm::vec3 gravityForce = rigidBody->getMass() * m_settings.gravity;
            rigidBody->applyForce(gravityForce);
        }

        glm::vec3 totalForce = rigidBody->getAccumulatedForce();
        
        // acceleration
        float mass = rigidBody->getMass();
        if (mass > 0.0f){// division by non-zero
            glm::vec3 acceleration = totalForce / mass;
            
            glm::vec3 currentVelocity = rigidBody->getLinearVelocity();
            glm::vec3 newVelocity = currentVelocity + acceleration * deltaTime;
            rigidBody->setLinearVelocity(newVelocity);
            
            glm::vec3 currentPosition = rigidBody->getPosition();
            glm::vec3 newPosition = currentPosition + newVelocity * deltaTime;
            rigidBody->setPosition(newPosition);
        }

        // clear force for next frame
        rigidBody->clearForces();
    }
    
    // NEW: Collision detection and response
    auto collisions = detectCollisions();
    resolveCollisions(collisions);
    updateRigidBodies();
}

void PhysicsWorld::setGravity(const glm::vec3& gravity) {
    m_settings.gravity = gravity;
    
    // TODO: Update Bullet Physics gravity
    // if (m_dynamicsWorld) {
    //     m_dynamicsWorld->setGravity(btVector3(gravity.x, gravity.y, gravity.z));
    // }
}

glm::vec3 PhysicsWorld::getGravity() const {
    return m_settings.gravity;
}

std::shared_ptr<RigidBody> PhysicsWorld::createRigidBody(PhysicsComponent* component) {
    auto rigidBody = std::make_shared<RigidBody>(component);
    m_rigidBodies.push_back(rigidBody);

    OHAO_LOG("Created RigidBody, total bodies: " + std::to_string(m_rigidBodies.size()));
    return rigidBody;
}

void PhysicsWorld::removeRigidBody(std::shared_ptr<RigidBody> body) {
    // TODO: Remove from Bullet Physics world and container
    // auto it = std::find(m_rigidBodies.begin(), m_rigidBodies.end(), body);
    // if (it != m_rigidBodies.end()) {
    //     m_rigidBodies.erase(it);
    // }
    
    OHAO_LOG_WARNING("RigidBody removal not implemented");
}

void PhysicsWorld::removeRigidBody(PhysicsComponent* component) {
    // TODO: Find and remove by component
    OHAO_LOG_WARNING("RigidBody removal by component not implemented");
}

PhysicsWorld::RaycastResult PhysicsWorld::raycast(const glm::vec3& from, const glm::vec3& to) {
    RaycastResult result;
    
    // TODO: Implement raycast using Bullet Physics
    // btVector3 btFrom(from.x, from.y, from.z);
    // btVector3 btTo(to.x, to.y, to.z);
    // btCollisionWorld::ClosestRayResultCallback rayCallback(btFrom, btTo);
    // m_dynamicsWorld->rayTest(btFrom, btTo, rayCallback);
    
    OHAO_LOG_WARNING("Raycast not implemented");
    return result;
}

void PhysicsWorld::setSettings(const PhysicsSettings& settings) {
    m_settings = settings;
    setGravity(settings.gravity);
}

const PhysicsSettings& PhysicsWorld::getSettings() const {
    return m_settings;
}

void PhysicsWorld::setDebugDrawEnabled(bool enabled) {
    m_debugDrawEnabled = enabled;
}

bool PhysicsWorld::isDebugDrawEnabled() const {
    return m_debugDrawEnabled;
}

void PhysicsWorld::debugDraw() {
    if (!m_debugDrawEnabled || !m_initialized) {
        return;
    }
    
    // TODO: Implement debug drawing
    // if (m_dynamicsWorld) {
    //     m_dynamicsWorld->debugDrawWorld();
    // }
}

void PhysicsWorld::initializeBulletPhysics() {
    // TODO: Initialize Bullet Physics components
    // m_collisionConfig = new btDefaultCollisionConfiguration();
    // m_dispatcher = new btCollisionDispatcher(m_collisionConfig);
    // m_broadphase = new btDbvtBroadphase();
    // m_solver = new btSequentialImpulseConstraintSolver();
    // m_dynamicsWorld = new btDiscreteDynamicsWorld(m_dispatcher, m_broadphase, m_solver, m_collisionConfig);
}

void PhysicsWorld::cleanupBulletPhysics() {
    // TODO: Cleanup Bullet Physics in reverse order
    // delete m_dynamicsWorld;
    // delete m_solver;
    // delete m_broadphase;
    // delete m_dispatcher;
    // delete m_collisionConfig;
}

void PhysicsWorld::updateRigidBodies() {
    for (auto& rigidBody : m_rigidBodies){
        if(rigidBody){
            rigidBody->updateTransform();
        }
    }
}

size_t PhysicsWorld::getRigidBodyCount() const {
    return m_rigidBodies.size();
}

void PhysicsWorld::setSimulationState(PhysicsSimulationState state) {
    m_simulationState = state;
    OHAO_LOG_DEBUG("Physics simulation state changed to: " + std::to_string(static_cast<int>(state)));
}

PhysicsSimulationState PhysicsWorld::getSimulationState() const {
    return m_simulationState;
}

// ============= COLLISION DETECTION IMPLEMENTATION =============

std::vector<PhysicsWorld::CollisionInfo> PhysicsWorld::detectCollisions() {
    std::vector<CollisionInfo> collisions;
    
    // Check all pairs of rigid bodies
    for (size_t i = 0; i < m_rigidBodies.size(); ++i) {
        for (size_t j = i + 1; j < m_rigidBodies.size(); ++j) {
            auto bodyA = m_rigidBodies[i];
            auto bodyB = m_rigidBodies[j];
            
            if (!bodyA || !bodyB) continue;
            
            // Skip collision if both are static
            if (bodyA->getType() == RigidBodyType::STATIC && 
                bodyB->getType() == RigidBodyType::STATIC) {
                continue;
            }
            
            auto collision = checkCollision(bodyA, bodyB);
            if (collision.hasCollision) {
                collisions.push_back(collision);
                
                // Debug logging
                printf("COLLISION: Body %zu (%.2f,%.2f,%.2f) vs Body %zu (%.2f,%.2f,%.2f) - penetration: %.3f\n",
                       i, bodyA->getPosition().x, bodyA->getPosition().y, bodyA->getPosition().z,
                       j, bodyB->getPosition().x, bodyB->getPosition().y, bodyB->getPosition().z,
                       collision.penetrationDepth);
            }
        }
    }
    
    return collisions;
}

PhysicsWorld::CollisionInfo PhysicsWorld::checkCollision(std::shared_ptr<RigidBody> bodyA, 
                                                        std::shared_ptr<RigidBody> bodyB) {
    CollisionInfo info;
    info.bodyA = bodyA;
    info.bodyB = bodyB;
    
    // Get collision shapes
    auto shapeA = bodyA->getCollisionShape();
    auto shapeB = bodyB->getCollisionShape();
    
    if (!shapeA || !shapeB) {
        return info; // No collision if no shapes
    }
    
    // Get positions and sizes
    glm::vec3 posA = bodyA->getPosition();
    glm::vec3 posB = bodyB->getPosition();
    glm::vec3 sizeA = getCollisionShapeSize(shapeA);
    glm::vec3 sizeB = getCollisionShapeSize(shapeB);
    
    // Perform AABB collision check
    info.hasCollision = checkAABBCollision(posA, sizeA, posB, sizeB, info);
    
    return info;
}

bool PhysicsWorld::checkAABBCollision(const glm::vec3& posA, const glm::vec3& sizeA,
                                     const glm::vec3& posB, const glm::vec3& sizeB,
                                     CollisionInfo& info) {
    // Calculate half-extents for both boxes
    glm::vec3 halfA = sizeA * 0.5f;
    glm::vec3 halfB = sizeB * 0.5f;
    
    // Calculate distance between centers
    glm::vec3 distance = posB - posA;
    glm::vec3 absDistance = glm::abs(distance);
    
    // Check overlap on all axes
    glm::vec3 overlap = (halfA + halfB) - absDistance;
    
    // If negative on any axis, no collision
    if (overlap.x <= 0.0f || overlap.y <= 0.0f || overlap.z <= 0.0f) {
        return false;
    }
    
    // Find the axis of least penetration (contact normal)
    if (overlap.x <= overlap.y && overlap.x <= overlap.z) {
        // X axis has least penetration
        info.contactNormal = glm::vec3(distance.x > 0 ? 1.0f : -1.0f, 0.0f, 0.0f);
        info.penetrationDepth = overlap.x;
        info.contactPoint = posA + glm::vec3(distance.x > 0 ? halfA.x : -halfA.x, 0.0f, 0.0f);
    }
    else if (overlap.y <= overlap.z) {
        // Y axis has least penetration
        info.contactNormal = glm::vec3(0.0f, distance.y > 0 ? 1.0f : -1.0f, 0.0f);
        info.penetrationDepth = overlap.y;
        info.contactPoint = posA + glm::vec3(0.0f, distance.y > 0 ? halfA.y : -halfA.y, 0.0f);
    }
    else {
        // Z axis has least penetration
        info.contactNormal = glm::vec3(0.0f, 0.0f, distance.z > 0 ? 1.0f : -1.0f);
        info.penetrationDepth = overlap.z;
        info.contactPoint = posA + glm::vec3(0.0f, 0.0f, distance.z > 0 ? halfA.z : -halfA.z);
    }
    
    return true;
}

void PhysicsWorld::resolveCollisions(const std::vector<CollisionInfo>& collisions) {
    for (const auto& collision : collisions) {
        resolveCollision(collision);
    }
}

void PhysicsWorld::resolveCollision(const CollisionInfo& collision) {
    auto bodyA = collision.bodyA;
    auto bodyB = collision.bodyB;
    
    // Calculate mass ratio for separation
    float massA = bodyA->getMass();
    float massB = bodyB->getMass();
    float totalMass = massA + massB;
    
    // Handle static objects (infinite mass)
    bool isAStatic = (bodyA->getType() == RigidBodyType::STATIC);
    bool isBStatic = (bodyB->getType() == RigidBodyType::STATIC);
    
    float ratioA, ratioB;
    if (isAStatic && !isBStatic) {
        ratioA = 0.0f; ratioB = 1.0f; // Only move B
    } else if (!isAStatic && isBStatic) {
        ratioA = 1.0f; ratioB = 0.0f; // Only move A
    } else if (!isAStatic && !isBStatic) {
        ratioA = massB / totalMass; // Lighter object moves more
        ratioB = massA / totalMass;
    } else {
        return; // Both static, no resolution
    }
    
    // Separate objects along contact normal
    glm::vec3 separation = collision.contactNormal * collision.penetrationDepth;
    
    if (!isAStatic) {
        glm::vec3 newPosA = bodyA->getPosition() - separation * ratioA;
        bodyA->setPosition(newPosA);
    }
    
    if (!isBStatic) {
        glm::vec3 newPosB = bodyB->getPosition() + separation * ratioB;
        bodyB->setPosition(newPosB);
    }
    
    // Apply collision response to velocities
    if (!isAStatic) {
        glm::vec3 velA = bodyA->getLinearVelocity();
        float velAlongNormal = glm::dot(velA, collision.contactNormal);
        if (velAlongNormal < 0) { // Moving towards collision
            // Simple response: remove velocity component along normal
            glm::vec3 newVelA = velA - collision.contactNormal * velAlongNormal;
            bodyA->setLinearVelocity(newVelA * 0.8f); // Add some damping
        }
    }
    
    if (!isBStatic) {
        glm::vec3 velB = bodyB->getLinearVelocity();
        float velAlongNormal = glm::dot(velB, -collision.contactNormal);
        if (velAlongNormal < 0) { // Moving towards collision
            glm::vec3 newVelB = velB + collision.contactNormal * velAlongNormal;
            bodyB->setLinearVelocity(newVelB * 0.8f); // Add some damping
        }
    }
}

glm::vec3 PhysicsWorld::getCollisionShapeSize(std::shared_ptr<CollisionShape> shape) {
    if (!shape) {
        return glm::vec3(1.0f); // Default size
    }
    
    // For now, assume all shapes are boxes with size 1.0f
    // TODO: Implement proper shape size extraction based on shape type
    switch (shape->getType()) {
        case CollisionShapeType::BOX:
            // TODO: Extract actual box half-extents
            return glm::vec3(1.0f);
        case CollisionShapeType::SPHERE:
            // TODO: Extract actual sphere radius
            return glm::vec3(1.0f);
        default:
            return glm::vec3(1.0f);
    }
}

} // namespace ohao
