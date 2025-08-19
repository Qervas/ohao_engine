#include "physics_world.hpp"
#include "../dynamics/integrator.hpp"
#include "../../component/physics_component.hpp"
#include "../../../ui/components/console_widget.hpp"
#include <algorithm>
#include <chrono>

namespace ohao {
namespace physics {

PhysicsWorld::PhysicsWorld() {
    // Constructor
}

PhysicsWorld::~PhysicsWorld() {
    cleanup();
}

// === INITIALIZATION ===
bool PhysicsWorld::initialize(const PhysicsSettings& settings) {
    if (m_initialized) {
        OHAO_LOG_WARNING("PhysicsWorld already initialized");
        return true;
    }
    
    m_settings = settings;
    m_simulationState = SimulationState::STOPPED;
    
    // Clear any existing data
    m_rigidBodies.clear();
    m_contacts.clear();
    m_contactPairs.clear();
    
    // Initialize debug stats
    m_debugStats = DebugStats{};
    m_stepTimeAccumulator = 0.0f;
    m_stepCount = 0;
    
    m_initialized = true;
    
    OHAO_LOG("PhysicsWorld initialized with gravity: (" + 
             std::to_string(m_settings.gravity.x) + ", " + 
             std::to_string(m_settings.gravity.y) + ", " + 
             std::to_string(m_settings.gravity.z) + ")");
    
    return true;
}

void PhysicsWorld::cleanup() {
    if (!m_initialized) return;
    
    // Clear all rigid bodies
    m_rigidBodies.clear();
    m_contacts.clear();
    m_contactPairs.clear();
    
    m_simulationState = SimulationState::STOPPED;
    m_initialized = false;
    
    OHAO_LOG("PhysicsWorld cleaned up");
}

// === SIMULATION CONTROL ===
void PhysicsWorld::stepSimulation(float deltaTime) {
    static bool hasLoggedOnce = false;
    static int skipCount = 0;
    
    if (!m_initialized || m_simulationState != SimulationState::RUNNING) {
        skipCount++;
        if (!hasLoggedOnce) {
            printf("PhysicsWorld::stepSimulation SKIPPED - initialized=%d, state=%d (need %d=RUNNING)\n", 
                   m_initialized, static_cast<int>(m_simulationState), static_cast<int>(SimulationState::RUNNING));
            hasLoggedOnce = true;
        }
        return;
    }
    
    if (skipCount > 0) {
        printf("PhysicsWorld::stepSimulation NOW RUNNING after %d skips - deltaTime=%f, rigidBodies=%zu\n", 
               skipCount, deltaTime, m_rigidBodies.size());
        skipCount = 0;
        hasLoggedOnce = false; // Reset so we can log skip again if needed
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Remove any invalid/destroyed bodies
    removeInvalidBodies();
    
    // Clear previous frame's collision data
    m_contacts.clear();
    m_contactPairs.clear();
    
    // Main simulation phases
    applyGravity();
    integrateForces(deltaTime);
    detectCollisions();
    resolveCollisions();
    integrateVelocities(deltaTime);
    updateSleepStates(deltaTime);
    syncWithComponents();
    
    // Update debug statistics
    auto endTime = std::chrono::high_resolution_clock::now();
    float stepTime = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    updateDebugStats(stepTime);
}

// === RIGID BODY MANAGEMENT ===
std::shared_ptr<dynamics::RigidBody> PhysicsWorld::createRigidBody(PhysicsComponent* component) {
    if (!m_initialized || !component) {
        return nullptr;
    }
    
    auto rigidBody = std::make_shared<dynamics::RigidBody>(component);
    m_rigidBodies.push_back(rigidBody);
    
    OHAO_LOG("Created rigid body in PhysicsWorld. Total count: " + std::to_string(m_rigidBodies.size()));
    
    return rigidBody;
}

void PhysicsWorld::removeRigidBody(std::shared_ptr<dynamics::RigidBody> body) {
    if (!body) return;
    
    auto it = std::find(m_rigidBodies.begin(), m_rigidBodies.end(), body);
    if (it != m_rigidBodies.end()) {
        m_rigidBodies.erase(it);
        OHAO_LOG("Removed rigid body from PhysicsWorld. Total count: " + std::to_string(m_rigidBodies.size()));
    }
}

void PhysicsWorld::removeRigidBody(PhysicsComponent* component) {
    if (!component) return;
    
    auto it = std::remove_if(m_rigidBodies.begin(), m_rigidBodies.end(), 
        [component](const std::weak_ptr<dynamics::RigidBody>& weakBody) {
            if (auto body = weakBody.lock()) {
                return body->getComponent() == component;
            }
            return true; // Remove expired weak_ptrs
        });
    
    if (it != m_rigidBodies.end()) {
        m_rigidBodies.erase(it, m_rigidBodies.end());
        OHAO_LOG("Removed rigid body by component from PhysicsWorld. Total count: " + std::to_string(m_rigidBodies.size()));
    }
}

// === RAYCASTING ===
PhysicsWorld::RaycastResult PhysicsWorld::raycast(const glm::vec3& from, const glm::vec3& to) {
    RaycastResult result;
    
    // TODO: Implement proper raycasting
    // For now, return empty result
    
    return result;
}

std::vector<PhysicsWorld::RaycastResult> PhysicsWorld::raycastAll(const glm::vec3& from, const glm::vec3& to) {
    std::vector<RaycastResult> results;
    
    // TODO: Implement proper raycasting for all hits
    
    return results;
}

// === SIMULATION PHASES ===
void PhysicsWorld::applyGravity() {
    for (auto& body : m_rigidBodies) {
        if (body && !body->isStatic() && body->isAwake()) {
            // Apply gravitational force: F = m * g
            glm::vec3 gravityForce = m_settings.gravity * body->getMass();
            body->applyForce(gravityForce);
        }
    }
}

void PhysicsWorld::integrateForces(float deltaTime) {
    for (auto& body : m_rigidBodies) {
        if (body && !body->isStatic() && body->isAwake()) {
            // Integrate velocity from forces
            dynamics::Integrator::integrateVelocity(body.get(), deltaTime);
        }
    }
}

void PhysicsWorld::detectCollisions() {
    // Clear previous contacts
    m_contacts.clear();
    m_contactPairs.clear();
    
    // Broad phase: check all pairs of rigid bodies
    for (size_t i = 0; i < m_rigidBodies.size(); ++i) {
        for (size_t j = i + 1; j < m_rigidBodies.size(); ++j) {
            auto bodyA = m_rigidBodies[i];
            auto bodyB = m_rigidBodies[j];
            
            if (!bodyA || !bodyB) continue;
            
            // Check for collision
            collision::ContactInfo contact = collision::CollisionDetector::detectCollision(
                bodyA.get(), bodyB.get()
            );
            
            if (contact.hasContact) {
                m_contacts.push_back(contact);
                m_contactPairs.push_back({bodyA.get(), bodyB.get()});
            }
        }
    }
    
    m_debugStats.numCollisionPairs = (m_rigidBodies.size() * (m_rigidBodies.size() - 1)) / 2;
    m_debugStats.numContacts = m_contacts.size();
}

void PhysicsWorld::resolveCollisions() {
    // Resolve all detected collisions
    collision::CollisionResolver::resolveContacts(m_contacts, m_contactPairs);
}

void PhysicsWorld::integrateVelocities(float deltaTime) {
    for (auto& body : m_rigidBodies) {
        if (body && !body->isStatic() && body->isAwake()) {
            // Integrate position from velocity
            dynamics::Integrator::integratePosition(body.get(), deltaTime);
            
            // Apply damping and clear forces
            dynamics::Integrator::applyDamping(body.get(), deltaTime);
            body->clearForces();
        }
    }
}

void PhysicsWorld::updateSleepStates(float deltaTime) {
    // TODO: Implement sleep/wake system for performance optimization
    // For now, keep all bodies awake
    
    size_t activeCount = 0;
    for (auto& body : m_rigidBodies) {
        if (body && body->isAwake()) {
            activeCount++;
        }
    }
    
    m_debugStats.numActiveRigidBodies = activeCount;
}

void PhysicsWorld::syncWithComponents() {
    // Update physics components with latest rigid body state
    for (auto& body : m_rigidBodies) {
        if (body && body->getComponent()) {
            body->updateTransformComponent();
        }
    }
}

// === UTILITY ===
void PhysicsWorld::updateDebugStats(float stepTime) {
    m_debugStats.numRigidBodies = m_rigidBodies.size();
    m_debugStats.lastStepTime = stepTime;
    
    // Calculate rolling average
    m_stepTimeAccumulator += stepTime;
    m_stepCount++;
    
    if (m_stepCount >= 60) { // Update average every 60 steps
        m_debugStats.averageStepTime = m_stepTimeAccumulator / m_stepCount;
        m_stepTimeAccumulator = 0.0f;
        m_stepCount = 0;
    }
}

void PhysicsWorld::removeInvalidBodies() {
    // Remove any null or invalid rigid bodies
    auto it = std::remove_if(m_rigidBodies.begin(), m_rigidBodies.end(),
        [](const std::shared_ptr<dynamics::RigidBody>& body) {
            return !body || !body->getComponent();
        });
    
    if (it != m_rigidBodies.end()) {
        size_t removedCount = std::distance(it, m_rigidBodies.end());
        m_rigidBodies.erase(it, m_rigidBodies.end());
        
        if (removedCount > 0) {
            OHAO_LOG("Removed " + std::to_string(removedCount) + " invalid rigid bodies");
        }
    }
}

} // namespace physics
} // namespace ohao