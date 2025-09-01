#pragma once

#include "force_generator.hpp"
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <functional>

namespace ohao {
namespace physics {

// Forward declarations
namespace dynamics {
    class RigidBody;
}

namespace forces {

/**
 * Registration entry for a force generator
 */
struct ForceRegistration {
    std::unique_ptr<ForceGenerator> generator;
    std::unordered_set<dynamics::RigidBody*> targetBodies;
    std::string name;
    bool enabled = true;
    
    ForceRegistration(std::unique_ptr<ForceGenerator> gen, const std::string& registrationName)
        : generator(std::move(gen)), name(registrationName) {}
};

/**
 * Central registry for managing force generators
 * Handles registration, application, and lifecycle of forces
 */
class ForceRegistry {
public:
    ForceRegistry() = default;
    ~ForceRegistry() = default;
    
    // Non-copyable but movable
    ForceRegistry(const ForceRegistry&) = delete;
    ForceRegistry& operator=(const ForceRegistry&) = delete;
    ForceRegistry(ForceRegistry&&) = default;
    ForceRegistry& operator=(ForceRegistry&&) = default;
    
    /**
     * Register a force generator
     * @param generator Unique pointer to the force generator
     * @param name Name for this force registration
     * @param targetBodies Bodies this force should affect (empty = affect all)
     * @return Registration ID for later reference
     */
    size_t registerForce(std::unique_ptr<ForceGenerator> generator, 
                        const std::string& name = "",
                        const std::vector<dynamics::RigidBody*>& targetBodies = {});
    
    /**
     * Unregister a force by ID
     */
    bool unregisterForce(size_t registrationId);
    
    /**
     * Unregister all forces with the given name
     */
    void unregisterForcesByName(const std::string& name);
    
    /**
     * Clear all force registrations
     */
    void clear();
    
    /**
     * Apply all registered forces to their target bodies
     * @param allBodies All bodies in the physics world
     * @param deltaTime Time step
     */
    void applyForces(const std::vector<dynamics::RigidBody*>& allBodies, float deltaTime);
    
    /**
     * Apply forces to a specific set of bodies
     */
    void applyForcesToBodies(const std::vector<dynamics::RigidBody*>& bodies, float deltaTime);
    
    /**
     * Enable/disable a force registration
     */
    bool setForceEnabled(size_t registrationId, bool enabled);
    
    /**
     * Enable/disable forces by name
     */
    void setForcesEnabledByName(const std::string& name, bool enabled);
    
    /**
     * Get number of registered forces
     */
    size_t getForceCount() const { return m_forceRegistrations.size(); }
    
    /**
     * Get number of active (enabled) forces
     */
    size_t getActiveForceCount() const;
    
    /**
     * Add a body to a force's target list
     */
    bool addBodyToForce(size_t registrationId, dynamics::RigidBody* body);
    
    /**
     * Remove a body from a force's target list
     */
    bool removeBodyFromForce(size_t registrationId, dynamics::RigidBody* body);
    
    /**
     * Remove a body from all force registrations (call when body is destroyed)
     */
    void removeBodyFromAllForces(dynamics::RigidBody* body);
    
    /**
     * Get force registration by ID
     */
    ForceRegistration* getForceRegistration(size_t registrationId);
    const ForceRegistration* getForceRegistration(size_t registrationId) const;
    
    /**
     * Find force registrations by name
     */
    std::vector<ForceRegistration*> getForceRegistrationsByName(const std::string& name);
    
    /**
     * Get all force registrations
     */
    const std::unordered_map<size_t, std::unique_ptr<ForceRegistration>>& getAllRegistrations() const {
        return m_forceRegistrations;
    }
    
    // Statistics and debugging
    struct ForceStats {
        size_t totalForces = 0;
        size_t activeForces = 0;
        size_t globalForces = 0;
        size_t singleBodyForces = 0;
        size_t pairForces = 0;
        float totalApplicationTimeMs = 0.0f;
    };
    
    const ForceStats& getStats() const { return m_stats; }
    void resetStats();
    
    /**
     * Debug utilities
     */
    void logForceRegistrations() const;
    std::vector<std::string> getForceNames() const;

private:
    std::unordered_map<size_t, std::unique_ptr<ForceRegistration>> m_forceRegistrations;
    size_t m_nextRegistrationId = 1;
    ForceStats m_stats;
    
    // Performance optimization: cache sorted forces by priority
    mutable bool m_sortedForcesValid = false;
    mutable std::vector<ForceRegistration*> m_sortedForces;
    
    void invalidateSortedCache() { m_sortedForcesValid = false; }
    void updateSortedCache() const;
    
    // Helper methods
    bool isBodyTargeted(const ForceRegistration& registration, dynamics::RigidBody* body) const;
    void applyForceRegistration(ForceRegistration& registration, const std::vector<dynamics::RigidBody*>& allBodies, float deltaTime);
};

} // namespace forces
} // namespace physics
} // namespace ohao