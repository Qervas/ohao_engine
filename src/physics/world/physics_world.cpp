#include "physics_world.hpp"
#include "physics/components/physics_component.hpp"
#include "physics/material/physics_material.hpp"
#include "physics/collision/collision_system.hpp"
#include "physics/forces/forces.hpp"
#include "physics/debug/force_debugger.hpp"
#include <chrono>
#include <algorithm>

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

    // Use default configurations for subsystems
    solverConfig = constraints::SolverConfig{};
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
    
    initializeSubsystems();
    m_state = SimulationState::STOPPED;
    
    // Reserve capacity for performance
    m_rigidBodies.reserve(m_config.initialBodyCapacity);
    
    // Initialize force debugger
    m_forceDebugger = std::make_unique<debug::ForceDebugger>();
}

void PhysicsWorld::shutdown() {
    stop();
    
    // Clear all bodies and constraints
    m_rigidBodies.clear();
    m_activeBodyPointers.clear();
    m_componentToBody.clear();
    
    // Reset subsystems
    m_collisionSystem.reset();
    m_constraintSolver.reset();
    m_forceDebugger.reset();
    
    m_state = SimulationState::STOPPED;
}

void PhysicsWorld::reset() {
    shutdown();
    initialize();
}

void PhysicsWorld::step(float deltaTime) {
    if (m_state != SimulationState::RUNNING) {
        return;
    }
    
    m_stepStartTime = std::chrono::high_resolution_clock::now();
    
    // Use appropriate threading model based on configuration
    if (m_config.enableMultithreading) {
        stepMultithreaded(deltaTime);
    } else {
        stepSinglethreaded(deltaTime);
    }
    
    updateStatistics();
    
    if (m_config.enableDebugVisualization) {
        updateDebugVisualization();
    }
}

void PhysicsWorld::stepOnce() {
    SimulationState oldState = m_state;
    m_state = SimulationState::RUNNING;
    step(m_config.timeStep);
    m_state = oldState;
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

void PhysicsWorld::setConfig(const PhysicsWorldConfig& config) {
    m_config = config;

    // Update subsystem configurations
    if (m_constraintSolver) {
        m_constraintSolver->setConfig(config.solverConfig);
    }
}

void PhysicsWorld::setGravity(const glm::vec3& gravity) {
    m_config.gravity = gravity;
}

void PhysicsWorld::setTimeStep(float timeStep) {
    m_config.timeStep = timeStep;
}

// Private implementation methods
void PhysicsWorld::initializeSubsystems() {
    // Initialize collision system (GJK/EPA + Dynamic BVH)
    m_collisionSystem = std::make_unique<collision::CollisionSystem>();

    // Initialize constraint solver (PGS+XPBD)
    m_constraintSolver = std::make_unique<constraints::ConstraintSolver>(m_config.solverConfig);
}

void PhysicsWorld::updateActiveBodyPointers() {
    m_activeBodyPointers.clear();
    for (auto& body : m_rigidBodies) {
        if (body && body->isAwake()) {
            m_activeBodyPointers.push_back(body.get());
        }
    }
}

void PhysicsWorld::stepSinglethreaded(float deltaTime) {
    // Simplified single-threaded step
    updateActiveBodyPointers();
    
    // Convert to raw pointers for force application
    std::vector<dynamics::RigidBody*> bodyPtrs;
    bodyPtrs.reserve(m_rigidBodies.size());
    for (auto& body : m_rigidBodies) {
        if (body) {
            bodyPtrs.push_back(body.get());
        }
    }
    
    // Start force debugging frame if enabled
    if (m_forceDebuggingEnabled && m_forceDebugger) {
        m_forceDebugger->startFrame();
    }
    
    // Apply forces from force registry (includes gravity, drag, etc.)
    m_forceRegistry.applyForces(bodyPtrs, deltaTime);
    
    // Apply legacy gravity for backward compatibility if no gravity forces are registered
    if (m_forceRegistry.getForceCount() == 0) {
        for (auto* body : m_activeBodyPointers) {
            if (body && !body->isStatic()) {
                glm::vec3 gravityForce = m_config.gravity * body->getMass();
                body->applyForce(gravityForce);
                
                // Record legacy gravity in force debugger
                if (m_forceDebuggingEnabled && m_forceDebugger) {
                    m_forceDebugger->recordForceApplication(body, gravityForce, body->getPosition(), "legacy_gravity");
                }
            }
        }
    }
    
    // Analyze forces if debugging is enabled
    if (m_forceDebuggingEnabled && m_forceDebugger) {
        m_forceDebugger->analyzeForceRegistry(m_forceRegistry, bodyPtrs);
    }
    
    // Integrate physics
    for (auto* body : m_activeBodyPointers) {
        if (body && !body->isStatic()) {
            body->integrate(deltaTime);
        }
    }
    
    // COLLISION DETECTION AND RESOLUTION
    // New modern system: Dynamic BVH + GJK/EPA + PGS+XPBD
    if (m_collisionSystem && m_constraintSolver && !m_rigidBodies.empty()) {
        // Broad phase: Update AABB tree with all bodies
        m_collisionSystem->updateBroadPhase(bodyPtrs);

        // Narrow phase: Detect collisions with GJK/EPA
        auto manifolds = m_collisionSystem->performNarrowPhase();

        // Constraint solving: Resolve contacts with PGS+XPBD
        m_constraintSolver->solve(manifolds, deltaTime);
    }
    
    // Clear accumulated forces after integration
    for (auto* body : m_activeBodyPointers) {
        if (body) {
            body->clearForces();
        }
    }
    
    // End force debugging frame if enabled
    if (m_forceDebuggingEnabled && m_forceDebugger) {
        m_forceDebugger->endFrame();
    }
}

void PhysicsWorld::stepMultithreaded(float deltaTime) {
    // For now, fall back to single-threaded
    stepSinglethreaded(deltaTime);
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

} // namespace physics
} // namespace ohao