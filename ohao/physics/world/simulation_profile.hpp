#pragma once

#include "physics/dynamics/rigid_body.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <chrono>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ohao {
namespace physics {

/**
 * SimulationProfile - Snapshot of complete physics state
 *
 * Stores a complete snapshot of all rigid body states at a specific moment.
 * Can be used to save/restore physics state for experimentation,
 * comparison, and iterative testing of different scenarios.
 *
 * Think of it as "Git branches for physics" - you can have multiple named
 * snapshots and switch between them at will.
 */
class SimulationProfile {
public:
    /**
     * BodySnapshot - Complete state of a single rigid body
     */
    struct BodySnapshot {
        uint32_t bodyId;              // Unique body identifier
        glm::vec3 position;
        glm::quat rotation;
        glm::vec3 linearVelocity;
        glm::vec3 angularVelocity;
        glm::vec3 accumulatedForce;   // Include forces for completeness
        glm::vec3 accumulatedTorque;
        bool isAwake;
    };

    /**
     * Constructor
     * @param name Profile name (e.g., "Gravity Test", "Rolling Sim")
     */
    explicit SimulationProfile(const std::string& name);

    /**
     * Capture current state of all bodies
     * @param bodies Vector of all rigid bodies in the physics world
     */
    void capture(const std::vector<std::shared_ptr<dynamics::RigidBody>>& bodies);

    /**
     * Restore this profile to physics world
     * @param bodies Vector of all rigid bodies to restore state to
     */
    void restore(std::vector<std::shared_ptr<dynamics::RigidBody>>& bodies);

    // === METADATA ===
    std::string getName() const { return m_name; }
    void setName(const std::string& name) { m_name = name; }

    std::chrono::system_clock::time_point getCreationTime() const { return m_creationTime; }
    size_t getBodyCount() const { return m_bodySnapshots.size(); }

    /**
     * Get human-readable timestamp string
     */
    std::string getCreationTimeString() const;

private:
    std::string m_name;                            // "Gravity Test", "Rolling Sim", etc.
    std::chrono::system_clock::time_point m_creationTime;
    std::vector<BodySnapshot> m_bodySnapshots;     // All body states
    std::unordered_map<uint32_t, size_t> m_bodyIdToIndex;  // Fast lookup: bodyId -> snapshot index
};

} // namespace physics
} // namespace ohao
