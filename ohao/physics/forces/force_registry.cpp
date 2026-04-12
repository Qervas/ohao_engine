#include "force_registry.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>

namespace ohao {
namespace physics {
namespace forces {

size_t ForceRegistry::registerForce(std::unique_ptr<ForceGenerator> generator, 
                                   const std::string& name,
                                   const std::vector<dynamics::RigidBody*>& targetBodies) {
    if (!generator) {
        return 0; // Invalid registration
    }
    
    size_t registrationId = m_nextRegistrationId++;
    
    auto registration = std::make_unique<ForceRegistration>(std::move(generator), name);
    
    // Add target bodies to the set
    for (auto* body : targetBodies) {
        if (body) {
            registration->targetBodies.insert(body);
        }
    }
    
    m_forceRegistrations[registrationId] = std::move(registration);
    invalidateSortedCache();
    
    return registrationId;
}

bool ForceRegistry::unregisterForce(size_t registrationId) {
    auto it = m_forceRegistrations.find(registrationId);
    if (it == m_forceRegistrations.end()) {
        return false;
    }
    
    m_forceRegistrations.erase(it);
    invalidateSortedCache();
    return true;
}

void ForceRegistry::unregisterForcesByName(const std::string& name) {
    auto it = m_forceRegistrations.begin();
    while (it != m_forceRegistrations.end()) {
        if (it->second->name == name) {
            it = m_forceRegistrations.erase(it);
        } else {
            ++it;
        }
    }
    invalidateSortedCache();
}

void ForceRegistry::clear() {
    m_forceRegistrations.clear();
    invalidateSortedCache();
}

void ForceRegistry::applyForces(const std::vector<dynamics::RigidBody*>& allBodies, float deltaTime) {
    if (m_forceRegistrations.empty()) {
        return;
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Update sorted cache if needed
    updateSortedCache();
    
    // Apply forces in priority order
    for (auto* registration : m_sortedForces) {
        if (registration && registration->enabled && registration->generator && registration->generator->isEnabled()) {
            applyForceRegistration(*registration, allBodies, deltaTime);
        }
    }
    
    // Update timing statistics
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    m_stats.totalApplicationTimeMs = duration.count() * 0.001f;
}

void ForceRegistry::applyForcesToBodies(const std::vector<dynamics::RigidBody*>& bodies, float deltaTime) {
    applyForces(bodies, deltaTime);
}

bool ForceRegistry::setForceEnabled(size_t registrationId, bool enabled) {
    auto it = m_forceRegistrations.find(registrationId);
    if (it == m_forceRegistrations.end()) {
        return false;
    }
    
    it->second->enabled = enabled;
    return true;
}

void ForceRegistry::setForcesEnabledByName(const std::string& name, bool enabled) {
    for (auto& [id, registration] : m_forceRegistrations) {
        if (registration->name == name) {
            registration->enabled = enabled;
        }
    }
}

size_t ForceRegistry::getActiveForceCount() const {
    size_t count = 0;
    for (const auto& [id, registration] : m_forceRegistrations) {
        if (registration && registration->enabled && registration->generator && registration->generator->isEnabled()) {
            ++count;
        }
    }
    return count;
}

bool ForceRegistry::addBodyToForce(size_t registrationId, dynamics::RigidBody* body) {
    auto it = m_forceRegistrations.find(registrationId);
    if (it == m_forceRegistrations.end() || !body) {
        return false;
    }
    
    it->second->targetBodies.insert(body);
    return true;
}

bool ForceRegistry::removeBodyFromForce(size_t registrationId, dynamics::RigidBody* body) {
    auto it = m_forceRegistrations.find(registrationId);
    if (it == m_forceRegistrations.end() || !body) {
        return false;
    }
    
    it->second->targetBodies.erase(body);
    return true;
}

void ForceRegistry::removeBodyFromAllForces(dynamics::RigidBody* body) {
    if (!body) return;
    
    for (auto& [id, registration] : m_forceRegistrations) {
        registration->targetBodies.erase(body);
    }
}

ForceRegistration* ForceRegistry::getForceRegistration(size_t registrationId) {
    auto it = m_forceRegistrations.find(registrationId);
    return (it != m_forceRegistrations.end()) ? it->second.get() : nullptr;
}

const ForceRegistration* ForceRegistry::getForceRegistration(size_t registrationId) const {
    auto it = m_forceRegistrations.find(registrationId);
    return (it != m_forceRegistrations.end()) ? it->second.get() : nullptr;
}

std::vector<ForceRegistration*> ForceRegistry::getForceRegistrationsByName(const std::string& name) {
    std::vector<ForceRegistration*> result;
    for (auto& [id, registration] : m_forceRegistrations) {
        if (registration->name == name) {
            result.push_back(registration.get());
        }
    }
    return result;
}

void ForceRegistry::resetStats() {
    m_stats = ForceStats{};
}

void ForceRegistry::logForceRegistrations() const {
    std::cout << "=== Force Registry Status ===" << std::endl;
    std::cout << "Total registrations: " << m_forceRegistrations.size() << std::endl;
    std::cout << "Active registrations: " << getActiveForceCount() << std::endl;
    
    for (const auto& [id, registration] : m_forceRegistrations) {
        std::cout << "ID: " << id 
                  << ", Name: '" << registration->name << "'"
                  << ", Type: " << registration->generator->getName()
                  << ", Enabled: " << (registration->enabled ? "Yes" : "No")
                  << ", Target bodies: " << registration->targetBodies.size()
                  << ", Priority: " << registration->generator->getPriority()
                  << std::endl;
    }
    std::cout << "=== End Registry Status ===" << std::endl;
}

std::vector<std::string> ForceRegistry::getForceNames() const {
    std::vector<std::string> names;
    for (const auto& [id, registration] : m_forceRegistrations) {
        names.push_back(registration->name);
    }
    return names;
}

void ForceRegistry::updateSortedCache() const {
    if (m_sortedForcesValid) {
        return;
    }
    
    m_sortedForces.clear();
    m_sortedForces.reserve(m_forceRegistrations.size());
    
    // Collect all registrations
    for (const auto& [id, registration] : m_forceRegistrations) {
        m_sortedForces.push_back(registration.get());
    }
    
    // Sort by priority (higher priority first)
    std::sort(m_sortedForces.begin(), m_sortedForces.end(),
        [](const ForceRegistration* a, const ForceRegistration* b) {
            if (!a || !a->generator) return false;
            if (!b || !b->generator) return true;
            return a->generator->getPriority() > b->generator->getPriority();
        });
    
    m_sortedForcesValid = true;
}

bool ForceRegistry::isBodyTargeted(const ForceRegistration& registration, dynamics::RigidBody* body) const {
    // If no specific targets, affect all bodies
    if (registration.targetBodies.empty()) {
        return true;
    }
    
    // Check if body is in target list
    return registration.targetBodies.find(body) != registration.targetBodies.end();
}

void ForceRegistry::applyForceRegistration(ForceRegistration& registration, 
                                         const std::vector<dynamics::RigidBody*>& allBodies, 
                                         float deltaTime) {
    if (!registration.generator) {
        return;
    }
    
    // Try to cast to global force first (most efficient for multi-body forces)
    if (auto* globalForce = dynamic_cast<GlobalForceGenerator*>(registration.generator.get())) {
        // Filter bodies based on targets
        std::vector<dynamics::RigidBody*> targetedBodies;
        if (registration.targetBodies.empty()) {
            // No specific targets, use all bodies
            targetedBodies = allBodies;
        } else {
            // Filter to only targeted bodies
            for (auto* body : allBodies) {
                if (isBodyTargeted(registration, body)) {
                    targetedBodies.push_back(body);
                }
            }
        }
        
        if (!targetedBodies.empty()) {
            globalForce->applyForceToMultiple(targetedBodies.data(), targetedBodies.size(), deltaTime);
        }
    }
    // Handle single body forces
    else if (auto* singleForce = dynamic_cast<SingleBodyForceGenerator*>(registration.generator.get())) {
        // If it has a specific target body, use that
        if (singleForce->getTargetBody()) {
            if (isBodyTargeted(registration, singleForce->getTargetBody())) {
                singleForce->applyForce(singleForce->getTargetBody(), deltaTime);
            }
        }
        // Otherwise apply to all targeted bodies
        else {
            for (auto* body : allBodies) {
                if (isBodyTargeted(registration, body)) {
                    singleForce->applyForce(body, deltaTime);
                }
            }
        }
    }
    // Handle pair forces
    else if (auto* pairForce = dynamic_cast<PairForceGenerator*>(registration.generator.get())) {
        auto* bodyA = pairForce->getBodyA();
        auto* bodyB = pairForce->getBodyB();
        
        if (bodyA && bodyB && 
            isBodyTargeted(registration, bodyA) && 
            isBodyTargeted(registration, bodyB)) {
            // For pair forces, apply to bodyA (the force should handle both bodies internally)
            pairForce->applyForce(bodyA, deltaTime);
        }
    }
    // Fallback: generic force generator
    else {
        for (auto* body : allBodies) {
            if (isBodyTargeted(registration, body)) {
                registration.generator->applyForce(body, deltaTime);
            }
        }
    }
}

} // namespace forces
} // namespace physics
} // namespace ohao