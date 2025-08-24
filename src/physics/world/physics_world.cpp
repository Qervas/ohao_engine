#include "physics_world.hpp"
#include "physics/components/physics_component.hpp"
#include "physics/material/physics_material.hpp"
#include "physics/collision/collision_system.hpp"
#include <chrono>

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
    solverConfig = constraints::ConstraintSolver::Config{};
    collisionConfig = collision::CollisionSystem::Config{};
    integratorConfig = dynamics::PhysicsIntegrator::Config{};
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
}

void PhysicsWorld::shutdown() {
    stop();
    
    // Clear all bodies and constraints
    m_rigidBodies.clear();
    m_activeBodyPointers.clear();
    m_componentToBody.clear();
    
    // Reset subsystems
    m_collisionSystem.reset();
    m_constraintManager.reset();
    m_integrator.reset();
    m_collisionQueries.reset();
    
    m_state = SimulationState::STOPPED;
}

void PhysicsWorld::reset() {
    shutdown();
    initialize();
}

void PhysicsWorld::step(float deltaTime) {
    // DEBUG: Log physics world step calls
    static int debugCount = 0;
    debugCount++;
    if (debugCount % 60 == 0) {
        printf("PHYSICS WORLD DEBUG: step() called - State=%d, deltaTime=%.4f\n", static_cast<int>(m_state), deltaTime);
    }
    
    if (m_state != SimulationState::RUNNING) {
        if (debugCount % 60 == 0) {
            printf("PHYSICS WORLD DEBUG: Skipping step - not running (state=%d)\n", static_cast<int>(m_state));
        }
        return;
    }
    
    if (debugCount % 60 == 0) {
        printf("PHYSICS WORLD DEBUG: Actually stepping physics!\n");
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
    if (m_collisionSystem) {
        m_collisionSystem->setConfig(config.collisionConfig);
    }
    if (m_constraintManager) {
        m_constraintManager->setSolverConfig(config.solverConfig);
    }
    if (m_integrator) {
        m_integrator->setConfig(config.integratorConfig);
    }
}

void PhysicsWorld::setGravity(const glm::vec3& gravity) {
    m_config.gravity = gravity;
    m_config.integratorConfig.gravity = gravity;
    if (m_integrator) {
        m_integrator->setConfig(m_config.integratorConfig);
    }
}

void PhysicsWorld::setTimeStep(float timeStep) {
    m_config.timeStep = timeStep;
    m_config.integratorConfig.maxTimeStep = timeStep;
    if (m_integrator) {
        m_integrator->setConfig(m_config.integratorConfig);
    }
}

// Private implementation methods (stubs for now)
void PhysicsWorld::initializeSubsystems() {
    // Initialize collision system
    m_collisionSystem = std::make_unique<collision::CollisionSystem>(m_config.collisionConfig);
    
    // TODO: Initialize other subsystems (integrator, constraint manager, etc.)
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
    
    // Apply gravity and integrate
    static int debugIntegrateCount = 0;
    debugIntegrateCount++;
    
    for (auto* body : m_activeBodyPointers) {
        if (body && !body->isStatic()) {
            // CRITICAL FIX: Apply gravity force before integration
            glm::vec3 gravityForce = m_config.gravity * body->getMass();
            body->applyForce(gravityForce);
            
            // DEBUG: Log integration calls occasionally
            if (debugIntegrateCount % 60 == 0) {
                glm::vec3 pos = body->getPosition();
                glm::vec3 vel = body->getLinearVelocity();
                printf("PHYSICS INTEGRATE DEBUG: Integrating body at (%.3f,%.3f,%.3f), mass=%.3f, vel=(%.3f,%.3f,%.3f), gravity_force=(%.3f,%.3f,%.3f)\n",
                       pos.x, pos.y, pos.z, body->getMass(), vel.x, vel.y, vel.z, gravityForce.x, gravityForce.y, gravityForce.z);
            }
            body->integrate(deltaTime);
        } else if (debugIntegrateCount % 60 == 0) {
            if (body) {
                printf("PHYSICS INTEGRATE DEBUG: Skipping static body at (%.3f,%.3f,%.3f)\n",
                       body->getPosition().x, body->getPosition().y, body->getPosition().z);
            }
        }
    }
    
    // COLLISION DETECTION AND RESOLUTION
    if (m_collisionSystem && !m_rigidBodies.empty()) {
        if (debugIntegrateCount % 60 == 0) {
            printf("COLLISION DEBUG: Running collision detection on %zu bodies\n", m_rigidBodies.size());
        }
        
        // Convert to raw pointers for collision system
        std::vector<dynamics::RigidBody*> bodyPtrs;
        bodyPtrs.reserve(m_rigidBodies.size());
        for (auto& body : m_rigidBodies) {
            if (body) {
                bodyPtrs.push_back(body.get());
            }
        }
        
        m_collisionSystem->detectAndResolveCollisions(bodyPtrs, deltaTime);
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

} // namespace physics
} // namespace ohao