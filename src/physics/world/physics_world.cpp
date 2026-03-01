#include "physics_world.hpp"
#include "physics/components/physics_component.hpp"
#include "engine/actor/actor.hpp"
#include "physics/material/physics_material.hpp"
#include "physics/forces/forces.hpp"
#include "physics/debug/force_debugger.hpp"
#include "physics/collision/shapes/box_shape.hpp"
#include "physics/collision/shapes/sphere_shape.hpp"
#include "physics/collision/shapes/capsule_shape.hpp"
#include "physics/collision/shapes/cylinder_shape.hpp"
#include "ui/components/console_widget.hpp"
#include <chrono>
#include <algorithm>
#include <iostream>

namespace ohao {
namespace physics {

// Default configuration constructor
PhysicsWorldConfig::PhysicsWorldConfig() {
    // Initialize with sensible defaults for real-time simulation
    gravity = glm::vec3(0.0f, -9.81f, 0.0f);
    timeStep = 1.0f / 60.0f;
    maxSubSteps = 4;
    
    enableMultithreading = true;
    workerThreads = 0; // Auto-detect
    enableSleeping = true;
    enableCCD = false;
    
    enableDebugVisualization = false;
    enableStatistics = true;
    enableProfiler = false;
    
    maxBodies = 10000;
    maxConstraints = 50000;
    initialBodyCapacity = 100;
}

// Constructor
PhysicsWorld::PhysicsWorld(const PhysicsWorldConfig& config) 
    : m_config(config) {
    initialize();
}

// Destructor
PhysicsWorld::~PhysicsWorld() {
    shutdown();
}

void PhysicsWorld::initialize() {
    if (m_state != SimulationState::STOPPED) {
        return; // Already initialized
    }

    // Try to create the physics backend (Jolt preferred)
    m_backend = backend::createPhysicsBackend("jolt");
    if (m_backend) {
        if (m_backend->initialize(m_config)) {
            std::cout << "[PhysicsWorld] Using backend: " << m_backend->getName() << std::endl;
        } else {
            std::cerr << "[PhysicsWorld] Backend '" << m_backend->getName()
                      << "' failed to initialize, falling back to null" << std::endl;
            m_backend = std::make_unique<backend::NullPhysicsBackend>();
            m_backend->initialize(m_config);
        }
    }

    m_state = SimulationState::STOPPED;

    // Reserve capacity for performance
    m_rigidBodies.reserve(m_config.initialBodyCapacity);

    // Initialize profile manager
    m_profileManager = std::make_unique<ProfileManager>();

    // Initialize force debugger
    m_forceDebugger = std::make_unique<debug::ForceDebugger>();
}

void PhysicsWorld::shutdown() {
    stop();

    // Clear all bodies and constraints
    m_rigidBodies.clear();
    m_activeBodyPointers.clear();
    m_componentToBody.clear();

    // Shutdown backend
    if (m_backend) {
        m_backend->shutdown();
        m_backend.reset();
    }

    m_forceDebugger.reset();

    m_state = SimulationState::STOPPED;
}

void PhysicsWorld::reset() {
    OHAO_LOG("Resetting physics world");

    // Stop simulation
    m_state = SimulationState::STOPPED;

    // Reset timestep accumulator
    m_timestepAccumulator = 0.0f;

    // If we have an active profile, restore from it
    if (m_profileManager && m_profileManager->hasActiveProfile()) {
        m_profileManager->restoreFromActive(m_rigidBodies);
        OHAO_LOG("Physics world reset to active profile");
    } else {
        // No profile - clear all velocities and forces
        for (auto& body : m_rigidBodies) {
            if (body) {
                body->setLinearVelocity(glm::vec3(0.0f));
                body->setAngularVelocity(glm::vec3(0.0f));
                body->clearForces();
                body->updateTransformComponent();
            }
        }
        OHAO_LOG("Physics world reset (no profile, cleared velocities)");
    }

}

void PhysicsWorld::step(float variableDeltaTime) {
    if (m_state != SimulationState::RUNNING) {
        return;  // No simulation when paused/stopped
    }

    // Accumulate variable frame time
    m_timestepAccumulator += variableDeltaTime;

    // Step physics in fixed chunks
    int stepsThisFrame = 0;
    const int maxStepsPerFrame = 4;  // Prevent spiral of death

    while (m_timestepAccumulator >= m_fixedTimestep && stepsThisFrame < maxStepsPerFrame) {
        // Fixed timestep physics tick
        stepFixed(m_fixedTimestep);

        m_timestepAccumulator -= m_fixedTimestep;
        stepsThisFrame++;
    }

    // If we hit max steps, discard remaining time to prevent catch-up spiral
    if (stepsThisFrame >= maxStepsPerFrame) {
        m_timestepAccumulator = 0.0f;
    }

    updateStatistics();

    if (m_config.enableDebugVisualization) {
        updateDebugVisualization();
    }
}

void PhysicsWorld::stepOnce() {
    if (m_state == SimulationState::RUNNING) {
        // Already running, don't interfere
        return;
    }

    // Single fixed timestep
    stepFixed(m_fixedTimestep);

    updateStatistics();

    if (m_config.enableDebugVisualization) {
        updateDebugVisualization();
    }
}

void PhysicsWorld::pause() {
    if (m_state == SimulationState::RUNNING) {
        m_state = SimulationState::PAUSED;
    }
}

void PhysicsWorld::resume() {
    if (m_state == SimulationState::PAUSED) {
        m_state = SimulationState::RUNNING;
    }
}

void PhysicsWorld::stop() {
    m_state = SimulationState::STOPPED;
}

std::shared_ptr<dynamics::RigidBody> PhysicsWorld::createRigidBody(PhysicsComponent* component) {
    if (!component) {
        return nullptr;
    }
    
    auto rigidBody = std::make_shared<dynamics::RigidBody>(component);
    
    std::lock_guard<std::mutex> lock(m_bodiesMutex);
    m_rigidBodies.push_back(rigidBody);
    m_componentToBody[component] = rigidBody;
    
    updateActiveBodyPointers();
    return rigidBody;
}

void PhysicsWorld::removeRigidBody(std::shared_ptr<dynamics::RigidBody> body) {
    if (!body) return;

    // Remove from backend first
    if (hasBackend() && body->hasBackendBody()) {
        m_backend->destroyBody(body->getBackendHandle());
        body->setBackendHandle(backend::INVALID_BODY);
    }

    std::lock_guard<std::mutex> lock(m_bodiesMutex);

    // Remove from component mapping
    for (auto it = m_componentToBody.begin(); it != m_componentToBody.end(); ++it) {
        if (it->second == body) {
            m_componentToBody.erase(it);
            break;
        }
    }

    // Remove from bodies list
    m_rigidBodies.erase(
        std::remove(m_rigidBodies.begin(), m_rigidBodies.end(), body),
        m_rigidBodies.end()
    );

    updateActiveBodyPointers();
}

void PhysicsWorld::removeRigidBody(dynamics::RigidBody* body) {
    if (!body) return;

    // Remove from backend first
    if (hasBackend() && body->hasBackendBody()) {
        m_backend->destroyBody(body->getBackendHandle());
        body->setBackendHandle(backend::INVALID_BODY);
    }

    // Find the shared_ptr and remove it
    std::lock_guard<std::mutex> lock(m_bodiesMutex);
    for (auto it = m_rigidBodies.begin(); it != m_rigidBodies.end(); ++it) {
        if (it->get() == body) {
            // Remove from component mapping first
            for (auto mapIt = m_componentToBody.begin(); mapIt != m_componentToBody.end(); ++mapIt) {
                if (mapIt->second == *it) {
                    m_componentToBody.erase(mapIt);
                    break;
                }
            }

            m_rigidBodies.erase(it);
            updateActiveBodyPointers();
            break;
        }
    }
}

void PhysicsWorld::addRigidBodyForTesting(std::shared_ptr<dynamics::RigidBody> body) {
    if (!body) return;

    std::lock_guard<std::mutex> lock(m_bodiesMutex);
    m_rigidBodies.push_back(body);
    updateActiveBodyPointers();
}

void PhysicsWorld::setConfig(const PhysicsWorldConfig& config) {
    m_config = config;
}

void PhysicsWorld::setGravity(const glm::vec3& gravity) {
    m_config.gravity = gravity;
    if (hasBackend()) {
        m_backend->setGravity(gravity);
    }
}

void PhysicsWorld::setTimeStep(float timeStep) {
    m_config.timeStep = timeStep;
}

void PhysicsWorld::updateActiveBodyPointers() {
    m_activeBodyPointers.clear();
    for (auto& body : m_rigidBodies) {
        if (body && body->isAwake()) {
            m_activeBodyPointers.push_back(body.get());
        }
    }
}

void PhysicsWorld::stepFixed(float fixedDt) {
    if (!hasBackend()) return;

    // 1. Ensure all bodies have backend representations
    syncPendingBodiesToBackend();

    // 2. Apply custom forces from force registry (gameplay forces, wind, etc.)
    if (m_forceRegistry.getForceCount() > 0) {
        std::vector<dynamics::RigidBody*> bodyPtrs;
        bodyPtrs.reserve(m_rigidBodies.size());
        for (auto& body : m_rigidBodies) {
            if (body) bodyPtrs.push_back(body.get());
        }
        m_forceRegistry.applyForces(bodyPtrs, fixedDt);
    }

    // 3. Push kinematic positions and accumulated forces to backend
    for (auto& body : m_rigidBodies) {
        if (!body || !body->hasBackendBody()) continue;
        auto h = body->getBackendHandle();

        // For kinematic bodies, application controls position
        if (body->isKinematic()) {
            m_backend->setPosition(h, body->getPosition());
            m_backend->setRotation(h, body->getRotation());
        }

        // Push accumulated forces to backend
        auto force = body->getAccumulatedForce();
        if (glm::length2(force) > 0.0001f) {
            m_backend->applyForce(h, force, glm::vec3(0.0f));
        }
        auto torque = body->getAccumulatedTorque();
        if (glm::length2(torque) > 0.0001f) {
            m_backend->applyTorque(h, torque);
        }
    }

    // 4. Step the backend (Jolt handles broadphase + narrowphase + solving)
    m_backend->step(fixedDt);

    // 5. Pull results back from backend
    syncBodiesFromBackend();
}

void PhysicsWorld::updateDebugVisualization() {
    // Update debug visualization data
}

void PhysicsWorld::updateStatistics() {
    m_stats.totalBodies = m_rigidBodies.size();
    m_stats.activeBodies = m_activeBodyPointers.size();
    m_stats.sleepingBodies = m_stats.totalBodies - m_stats.activeBodies;
}

void PhysicsWorld::resetStats() {
    m_stats = PhysicsStats{};
}

void PhysicsWorld::enableDebugVisualization(bool enable) {
    m_config.enableDebugVisualization = enable;
}

void PhysicsWorld::enableForceDebugging(bool enable) {
    m_forceDebuggingEnabled = enable;
    
    if (enable && !m_forceDebugger) {
        m_forceDebugger = std::make_unique<debug::ForceDebugger>();
    }
}

bool PhysicsWorld::isForceDebuggingEnabled() const {
    return m_forceDebuggingEnabled && m_forceDebugger != nullptr;
}

size_t PhysicsWorld::getMemoryUsage() const {
    size_t usage = 0;
    usage += sizeof(PhysicsWorld);
    usage += m_rigidBodies.capacity() * sizeof(std::shared_ptr<dynamics::RigidBody>);
    usage += m_activeBodyPointers.capacity() * sizeof(dynamics::RigidBody*);
    return usage;
}

void PhysicsWorld::compactMemory() {
    // Remove null pointers and optimize memory layout
    m_rigidBodies.erase(
        std::remove_if(m_rigidBodies.begin(), m_rigidBodies.end(), 
                       [](const std::weak_ptr<dynamics::RigidBody>& weak) {
                           return weak.expired();
                       }),
        m_rigidBodies.end()
    );
    
    m_rigidBodies.shrink_to_fit();
    m_activeBodyPointers.shrink_to_fit();
}

// === FORCE SYSTEM INTEGRATION ===
size_t PhysicsWorld::registerForce(std::unique_ptr<forces::ForceGenerator> generator, 
                                  const std::string& name,
                                  const std::vector<dynamics::RigidBody*>& targetBodies) {
    return m_forceRegistry.registerForce(std::move(generator), name, targetBodies);
}

bool PhysicsWorld::unregisterForce(size_t forceId) {
    return m_forceRegistry.unregisterForce(forceId);
}

void PhysicsWorld::clearAllForces() {
    m_forceRegistry.clear();
}

void PhysicsWorld::setupEarthEnvironment() {
    // Clear existing forces
    clearAllForces();
    
    // Convert to raw pointers
    std::vector<dynamics::RigidBody*> bodyPtrs;
    bodyPtrs.reserve(m_rigidBodies.size());
    for (auto& body : m_rigidBodies) {
        if (body) {
            bodyPtrs.push_back(body.get());
        }
    }
    
    forces::ForcePresets::setupEarthEnvironment(m_forceRegistry, bodyPtrs);
}

void PhysicsWorld::setupSpaceEnvironment() {
    // Clear existing forces
    clearAllForces();
    
    // Convert to raw pointers
    std::vector<dynamics::RigidBody*> bodyPtrs;
    bodyPtrs.reserve(m_rigidBodies.size());
    for (auto& body : m_rigidBodies) {
        if (body) {
            bodyPtrs.push_back(body.get());
        }
    }
    
    forces::ForcePresets::setupSpaceEnvironment(m_forceRegistry, bodyPtrs);
}

void PhysicsWorld::setupUnderwaterEnvironment() {
    // Clear existing forces
    clearAllForces();
    
    // Convert to raw pointers
    std::vector<dynamics::RigidBody*> bodyPtrs;
    bodyPtrs.reserve(m_rigidBodies.size());
    for (auto& body : m_rigidBodies) {
        if (body) {
            bodyPtrs.push_back(body.get());
        }
    }
    
    forces::ForcePresets::setupUnderwaterEnvironment(m_forceRegistry, bodyPtrs);
}

void PhysicsWorld::setupGamePhysics() {
    // Clear existing forces
    clearAllForces();

    // Convert to raw pointers
    std::vector<dynamics::RigidBody*> bodyPtrs;
    bodyPtrs.reserve(m_rigidBodies.size());
    for (auto& body : m_rigidBodies) {
        if (body) {
            bodyPtrs.push_back(body.get());
        }
    }

    forces::ForcePresets::setupGamePhysics(m_forceRegistry, bodyPtrs);
}

// ============================================================================
// Backend integration
// ============================================================================

void PhysicsWorld::registerBodyWithBackend(dynamics::RigidBody* body) {
    if (!body || !hasBackend()) return;
    if (body->hasBackendBody()) return; // Already registered

    auto info = buildCreationInfo(body);
    auto handle = m_backend->createBody(info);

    if (handle != backend::INVALID_BODY) {
        body->setBackendHandle(handle);
    }
}

void PhysicsWorld::syncPendingBodiesToBackend() {
    if (!hasBackend()) return;

    for (auto& body : m_rigidBodies) {
        if (!body) continue;
        if (body->hasBackendBody()) continue;

        // Only create backend body if we have a collision shape
        if (!body->getCollisionShape()) continue;

        // Sync position/rotation from TransformComponent → RigidBody before creating
        // the backend body. This fixes the timing issue where createRigidBody() runs
        // before the caller sets the transform (e.g. addCube sets position AFTER
        // createActorWithComponents returns).
        auto* comp = body->getComponent();
        if (comp) {
            comp->updateRigidBodyFromTransform();
        }

        auto info = buildCreationInfo(body.get());
        auto handle = m_backend->createBody(info);
        if (handle != backend::INVALID_BODY) {
            body->setBackendHandle(handle);

            // Push any pending velocity set before the backend body existed
            auto linVel = body->getLinearVelocity();
            if (glm::length2(linVel) > 0.0001f) {
                m_backend->setLinearVelocity(handle, linVel);
            }
            auto angVel = body->getAngularVelocity();
            if (glm::length2(angVel) > 0.0001f) {
                m_backend->setAngularVelocity(handle, angVel);
            }
        }
    }
}

void PhysicsWorld::syncBodiesFromBackend() {
    if (!hasBackend()) return;

    for (auto& body : m_rigidBodies) {
        if (!body || !body->hasBackendBody()) continue;
        auto h = body->getBackendHandle();

        // For dynamic bodies, backend is the authority
        if (body->isDynamic()) {
            body->setPosition(m_backend->getPosition(h));
            body->setRotation(m_backend->getRotation(h));
            glm::vec3 lv = m_backend->getLinearVelocity(h);
            glm::vec3 av = m_backend->getAngularVelocity(h);

            // Axis lock: zero out frozen velocity axes
            uint8_t fl = body->getFreezeLinearAxes();
            uint8_t fr = body->getFreezeRotationalAxes();
            if (fl) {
                if (fl & 1) lv.x = 0.0f;
                if (fl & 2) lv.y = 0.0f;
                if (fl & 4) lv.z = 0.0f;
                m_backend->setLinearVelocity(h, lv);
            }
            if (fr) {
                if (fr & 1) av.x = 0.0f;
                if (fr & 2) av.y = 0.0f;
                if (fr & 4) av.z = 0.0f;
                m_backend->setAngularVelocity(h, av);
            }
            body->setLinearVelocity(lv);
            body->setAngularVelocity(av);
        }

        // Sync to transform component (visual representation)
        body->updateTransformComponent();

        // Clear local force accumulators
        body->clearForces();
    }

    updateActiveBodyPointers();
}

backend::BodyCreationInfo PhysicsWorld::buildCreationInfo(const dynamics::RigidBody* body) const {
    backend::BodyCreationInfo info;
    info.position = body->getPosition();
    info.rotation = body->getRotation();
    info.mass = body->getMass();
    info.friction = body->getStaticFriction();
    info.restitution = body->getRestitution();
    info.linearDamping = body->getLinearDamping();
    info.angularDamping = body->getAngularDamping();
    info.gravityEnabled = body->isGravityEnabled();
    info.gravityScale = body->getGravityScale();

    // Map body type
    switch (body->getType()) {
        case dynamics::RigidBodyType::STATIC:
            info.motionType = backend::MotionType::STATIC;
            break;
        case dynamics::RigidBodyType::KINEMATIC:
            info.motionType = backend::MotionType::KINEMATIC;
            break;
        case dynamics::RigidBodyType::DYNAMIC:
        default:
            info.motionType = backend::MotionType::DYNAMIC;
            break;
    }

    // Convert collision shape to ShapeInfo
    auto shape = body->getCollisionShape();
    if (shape) {
        switch (shape->getType()) {
            case collision::ShapeType::BOX: {
                auto* box = static_cast<const collision::BoxShape*>(shape.get());
                info.shape.type = backend::ShapeInfo::BOX;
                info.shape.halfExtents = box->getHalfExtents();
                break;
            }
            case collision::ShapeType::SPHERE: {
                auto* sphere = static_cast<const collision::SphereShape*>(shape.get());
                info.shape.type = backend::ShapeInfo::SPHERE;
                info.shape.radius = sphere->getRadius();
                break;
            }
            case collision::ShapeType::CAPSULE: {
                auto* capsule = static_cast<const collision::CapsuleShape*>(shape.get());
                info.shape.type = backend::ShapeInfo::CAPSULE;
                info.shape.radius = capsule->getRadius();
                info.shape.height = capsule->getHeight();
                break;
            }
            case collision::ShapeType::CYLINDER: {
                auto* cylinder = static_cast<const collision::CylinderShape*>(shape.get());
                info.shape.type = backend::ShapeInfo::CYLINDER;
                info.shape.radius = cylinder->getRadius();
                info.shape.height = cylinder->getHeight();
                break;
            }
            case collision::ShapeType::PLANE: {
                info.shape.type = backend::ShapeInfo::PLANE;
                info.shape.planeNormal = glm::vec3(0, 1, 0);
                break;
            }
            case collision::ShapeType::TRIANGLE_MESH: {
                info.shape.type = backend::ShapeInfo::MESH;
                // Mesh data is transient - backend must copy during createBody
                // For now, fall back to box
                info.shape.type = backend::ShapeInfo::BOX;
                info.shape.halfExtents = shape->getSize() * 0.5f;
                break;
            }
            default: {
                // Default to box with the shape's size
                info.shape.type = backend::ShapeInfo::BOX;
                info.shape.halfExtents = shape->getSize() * 0.5f;
                break;
            }
        }
    }

    return info;
}

// ============================================================================
// Raycasting & Queries (delegate to backend)
// ============================================================================

bool PhysicsWorld::castRay(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                            backend::RaycastHit& outHit, uint16_t layerMask) const {
    if (!hasBackend()) return false;
    return m_backend->castRay(origin, direction, maxDistance, outHit, layerMask);
}

std::vector<backend::RaycastHit> PhysicsWorld::castRayAll(const glm::vec3& origin, const glm::vec3& direction,
                                                           float maxDistance, uint16_t layerMask) const {
    if (!hasBackend()) return {};
    return m_backend->castRayAll(origin, direction, maxDistance, layerMask);
}

bool PhysicsWorld::castSphere(const glm::vec3& origin, const glm::vec3& direction, float radius,
                               float maxDistance, backend::ShapeCastResult& outHit, uint16_t layerMask) const {
    if (!hasBackend()) return false;
    return m_backend->castSphere(origin, direction, radius, maxDistance, outHit, layerMask);
}

bool PhysicsWorld::castBox(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& halfExtents,
                            const glm::quat& rotation, float maxDistance, backend::ShapeCastResult& outHit,
                            uint16_t layerMask) const {
    if (!hasBackend()) return false;
    return m_backend->castBox(origin, direction, halfExtents, rotation, maxDistance, outHit, layerMask);
}

std::vector<backend::BodyHandle> PhysicsWorld::overlapSphere(const glm::vec3& center, float radius,
                                                              uint16_t layerMask) const {
    if (!hasBackend()) return {};
    return m_backend->overlapSphere(center, radius, layerMask);
}

std::vector<backend::BodyHandle> PhysicsWorld::overlapBox(const glm::vec3& center, const glm::vec3& halfExtents,
                                                           const glm::quat& rotation, uint16_t layerMask) const {
    if (!hasBackend()) return {};
    return m_backend->overlapBox(center, halfExtents, rotation, layerMask);
}

// ============================================================================
// Contact Callbacks
// ============================================================================

void PhysicsWorld::setContactListener(backend::IContactListener* listener) {
    if (hasBackend()) m_backend->setContactListener(listener);
}

std::vector<backend::ContactEvent> PhysicsWorld::getContactEvents() {
    if (!hasBackend()) return {};
    return m_backend->getContactEvents();
}

// ============================================================================
// Collision Layers
// ============================================================================

void PhysicsWorld::setLayerCollision(uint16_t layer1, uint16_t layer2, bool shouldCollide) {
    if (hasBackend()) m_backend->setLayerCollision(layer1, layer2, shouldCollide);
}

void PhysicsWorld::setBodyLayer(backend::BodyHandle handle, uint16_t layer) {
    if (hasBackend()) m_backend->setBodyLayer(handle, layer);
}

std::string PhysicsWorld::resolveHandleName(backend::BodyHandle handle) const {
    if (handle == backend::INVALID_BODY) return "";
    for (auto& body : m_rigidBodies) {
        if (body && body->getBackendHandle() == handle) {
            auto* comp = body->getComponent();
            if (!comp) return "";
            auto* actor = comp->getOwner();
            return actor ? actor->getName() : "";
        }
    }
    return "";
}

// ============================================================================
// Force Utilities
// ============================================================================

void PhysicsWorld::applyRadialImpulse(const glm::vec3& center, float strength, float radius, int falloff) {
    if (!hasBackend() || radius <= 0.0f) return;
    for (auto& body : m_rigidBodies) {
        if (!body || !body->isDynamic() || !body->hasBackendBody()) continue;
        glm::vec3 delta = body->getPosition() - center;
        float dist = glm::length(delta);
        if (dist >= radius || dist < 1e-6f) continue;
        float t = 1.0f - dist / radius;
        float factor = (falloff == 1) ? t * t : (falloff == 2) ? 1.0f : t;
        glm::vec3 impulse = glm::normalize(delta) * (strength * factor);
        m_backend->applyImpulse(body->getBackendHandle(), impulse, glm::vec3(0.0f));
    }
}

// ============================================================================
// Springs
// ============================================================================

int PhysicsWorld::createSpring(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                               float stiffness, float restLength, float damping) {
    if (!bodyA || !bodyB) return -1;
    auto spring = std::make_unique<forces::SpringForce>(bodyA, bodyB, stiffness, restLength, damping);
    size_t regId = m_forceRegistry.registerForce(std::move(spring), "spring");
    int handle = m_nextSpringHandle++;
    m_springMap[handle] = regId;
    return handle;
}

int PhysicsWorld::createAnchorSpring(dynamics::RigidBody* body, const glm::vec3& anchor,
                                     float stiffness, float restLength, float damping) {
    if (!body) return -1;
    auto spring = std::make_unique<forces::AnchorSpringForce>(body, anchor, stiffness, restLength, damping);
    size_t regId = m_forceRegistry.registerForce(std::move(spring), "anchor_spring");
    int handle = m_nextSpringHandle++;
    m_springMap[handle] = regId;
    return handle;
}

void PhysicsWorld::destroySpring(int handle) {
    auto it = m_springMap.find(handle);
    if (it == m_springMap.end()) return;
    m_forceRegistry.unregisterForce(it->second);
    m_springMap.erase(it);
}

void PhysicsWorld::setSpringEnabled(int handle, bool enabled) {
    auto it = m_springMap.find(handle);
    if (it == m_springMap.end()) return;
    m_forceRegistry.setForceEnabled(it->second, enabled);
}

// ============================================================================
// Global Wind / Water
// ============================================================================

void PhysicsWorld::setWind(const glm::vec3& direction, float strength, float turbulence) {
    if (m_windForceId != 0) {
        m_forceRegistry.unregisterForce(m_windForceId);
        m_windForceId = 0;
    }
    auto wind = std::make_unique<forces::WindForce>(direction, strength);
    wind->setTurbulence(turbulence);
    m_windForceId = m_forceRegistry.registerForce(std::move(wind), "world_wind");
}

void PhysicsWorld::clearWind() {
    if (m_windForceId != 0) {
        m_forceRegistry.unregisterForce(m_windForceId);
        m_windForceId = 0;
    }
}

void PhysicsWorld::setWater(float liquidLevel, float density, float drag) {
    if (m_waterForceId != 0) {
        m_forceRegistry.unregisterForce(m_waterForceId);
        m_waterForceId = 0;
    }
    auto buoyancy = std::make_unique<forces::BuoyancyForce>(density, liquidLevel);
    buoyancy->setFluidDrag(drag);
    m_waterForceId = m_forceRegistry.registerForce(std::move(buoyancy), "world_water");
}

void PhysicsWorld::clearWater() {
    if (m_waterForceId != 0) {
        m_forceRegistry.unregisterForce(m_waterForceId);
        m_waterForceId = 0;
    }
}

// Force Volumes

int PhysicsWorld::createForceVolumeBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::vec3& force) {
    auto vol = std::make_unique<forces::ForceVolume>(center, halfExtents, force);
    size_t regId = m_forceRegistry.registerForce(std::move(vol), "force_volume");
    int handle = m_nextForceVolumeHandle++;
    m_forceVolumeMap[handle] = regId;
    return handle;
}

int PhysicsWorld::createForceVolumeSphere(const glm::vec3& center, float radius, const glm::vec3& force) {
    auto vol = std::make_unique<forces::ForceVolume>(center, radius, force);
    size_t regId = m_forceRegistry.registerForce(std::move(vol), "force_volume");
    int handle = m_nextForceVolumeHandle++;
    m_forceVolumeMap[handle] = regId;
    return handle;
}

void PhysicsWorld::destroyForceVolume(int handle) {
    auto it = m_forceVolumeMap.find(handle);
    if (it == m_forceVolumeMap.end()) return;
    m_forceRegistry.unregisterForce(it->second);
    m_forceVolumeMap.erase(it);
}

void PhysicsWorld::setForceVolumeEnabled(int handle, bool enabled) {
    auto it = m_forceVolumeMap.find(handle);
    if (it == m_forceVolumeMap.end()) return;
    m_forceRegistry.setForceEnabled(it->second, enabled);
}

// ============================================================================
// Constraints
// ============================================================================

backend::ConstraintHandle PhysicsWorld::createConstraint(const backend::ConstraintSettings& settings) {
    if (!hasBackend()) return backend::INVALID_CONSTRAINT;
    return m_backend->createConstraint(settings);
}

void PhysicsWorld::destroyConstraint(backend::ConstraintHandle handle) {
    if (hasBackend()) m_backend->destroyConstraint(handle);
}

void PhysicsWorld::setConstraintEnabled(backend::ConstraintHandle handle, bool enabled) {
    if (hasBackend()) m_backend->setConstraintEnabled(handle, enabled);
}

void PhysicsWorld::setConstraintMotorState(backend::ConstraintHandle handle, bool enabled, float speed, float maxForce) {
    if (hasBackend()) m_backend->setConstraintMotorState(handle, enabled, speed, maxForce);
}

void PhysicsWorld::setConstraintLimits(backend::ConstraintHandle handle, float min, float max) {
    if (hasBackend()) m_backend->setConstraintLimits(handle, min, max);
}

void PhysicsWorld::setConstraintBreaking(backend::ConstraintHandle handle, float maxForce, float maxTorque) {
    if (hasBackend()) m_backend->setConstraintBreaking(handle, maxForce, maxTorque);
}

std::vector<backend::ConstraintHandle> PhysicsWorld::getAndClearBrokenConstraints() {
    if (!hasBackend()) return {};
    return m_backend->getAndClearBrokenConstraints();
}

// ============================================================================
// Character Controller
// ============================================================================

backend::CharacterHandle PhysicsWorld::createCharacter(const backend::CharacterCreationInfo& info) {
    if (!hasBackend()) return backend::INVALID_CHARACTER;
    return m_backend->createCharacter(info);
}

void PhysicsWorld::destroyCharacter(backend::CharacterHandle handle) {
    if (hasBackend()) m_backend->destroyCharacter(handle);
}

backend::CharacterState PhysicsWorld::getCharacterState(backend::CharacterHandle handle) const {
    if (!hasBackend()) return {};
    return m_backend->getCharacterState(handle);
}

void PhysicsWorld::setCharacterPosition(backend::CharacterHandle handle, const glm::vec3& pos) {
    if (hasBackend()) m_backend->setCharacterPosition(handle, pos);
}

void PhysicsWorld::setCharacterRotation(backend::CharacterHandle handle, const glm::quat& rot) {
    if (hasBackend()) m_backend->setCharacterRotation(handle, rot);
}

void PhysicsWorld::setCharacterLinearVelocity(backend::CharacterHandle handle, const glm::vec3& vel) {
    if (hasBackend()) m_backend->setCharacterLinearVelocity(handle, vel);
}

void PhysicsWorld::updateCharacter(backend::CharacterHandle handle, float deltaTime,
                                    const glm::vec3& gravity, const glm::vec3& movementInput) {
    if (hasBackend()) m_backend->updateCharacter(handle, deltaTime, gravity, movementInput);
}

} // namespace physics
} // namespace ohao