#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <span>
#include <concepts>

namespace ohao {
namespace physics {

// Forward declarations
namespace dynamics {
    class RigidBody;
}

namespace forces {

/**
 * Abstract base class for all force generators
 * Force generators calculate and apply forces to rigid bodies
 */
class ForceGenerator {
public:
    virtual ~ForceGenerator() = default;
    
    /**
     * Apply force to the given rigid body
     * @param body The rigid body to apply force to
     * @param deltaTime Time step for force calculation
     */
    virtual void applyForce(dynamics::RigidBody* body, float deltaTime) = 0;
    
    /**
     * Get the name/type of this force generator
     */
    [[nodiscard]] virtual std::string getName() const = 0;
    
    /**
     * Check if this force generator is enabled
     */
    [[nodiscard]] virtual bool isEnabled() const noexcept { return m_enabled; }
    
    /**
     * Enable/disable this force generator
     */
    virtual void setEnabled(bool enabled) noexcept { m_enabled = enabled; }
    
    /**
     * Get priority for force application order
     * Higher priority forces are applied first
     */
    [[nodiscard]] virtual int getPriority() const noexcept { return m_priority; }
    
    /**
     * Set priority for force application order
     */
    virtual void setPriority(int priority) noexcept { m_priority = priority; }

protected:
    bool m_enabled = true;
    int m_priority = 0; // Default priority
};

template<typename T>
concept ForceGeneratorLike = requires(T& t, dynamics::RigidBody* body, float dt) {
    { t.applyForce(body, dt) } -> std::same_as<void>;
    { t.getName() } -> std::convertible_to<std::string>;
    { t.isEnabled() } -> std::convertible_to<bool>;
};

/**
 * Base class for forces that affect single bodies
 */
class SingleBodyForceGenerator : public ForceGenerator {
public:
    /**
     * Constructor
     * @param targetBody The body this force affects (nullptr for global forces)
     */
    explicit SingleBodyForceGenerator(dynamics::RigidBody* targetBody = nullptr)
        : m_targetBody(targetBody) {}
    
    void setTargetBody(dynamics::RigidBody* body) noexcept { m_targetBody = body; }
    [[nodiscard]] dynamics::RigidBody* getTargetBody() const noexcept { return m_targetBody; }

protected:
    dynamics::RigidBody* m_targetBody = nullptr;
};

/**
 * Base class for forces that affect pairs of bodies
 */
class PairForceGenerator : public ForceGenerator {
public:
    /**
     * Constructor
     * @param bodyA First body
     * @param bodyB Second body
     */
    PairForceGenerator(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB)
        : m_bodyA(bodyA), m_bodyB(bodyB) {}
    
    void setBodies(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB) {
        m_bodyA = bodyA;
        m_bodyB = bodyB;
    }
    
    [[nodiscard]] dynamics::RigidBody* getBodyA() const noexcept { return m_bodyA; }
    [[nodiscard]] dynamics::RigidBody* getBodyB() const noexcept { return m_bodyB; }

protected:
    dynamics::RigidBody* m_bodyA = nullptr;
    dynamics::RigidBody* m_bodyB = nullptr;
};

/**
 * Base class for global forces that can affect multiple bodies
 */
class GlobalForceGenerator : public ForceGenerator {
public:
    /**
     * Apply force to multiple bodies
     * @param bodies Span of bodies to potentially affect
     * @param deltaTime Time step
     */
    virtual void applyForceToMultiple(std::span<dynamics::RigidBody*> bodies, float deltaTime) {
        // Default implementation applies to each body individually
        for (auto* body : bodies) {
            if (body && shouldAffectBody(body)) {
                applyForce(body, deltaTime);
            }
        }
    }
    
    /**
     * Check if this force should affect the given body
     */
    virtual bool shouldAffectBody(dynamics::RigidBody* body) const = 0;

protected:
    // Default implementation for single body (called by applyForceToMultiple)
    void applyForce(dynamics::RigidBody* body, float deltaTime) override {
        // Override in derived classes
    }
};

/**
 * Utility functions for force calculations
 */
namespace ForceUtils {
    /**
     * Calculate distance squared between two bodies
     */
    float distanceSquared(const dynamics::RigidBody* bodyA, const dynamics::RigidBody* bodyB);
    
    /**
     * Calculate distance between two bodies
     */
    float distance(const dynamics::RigidBody* bodyA, const dynamics::RigidBody* bodyB);
    
    /**
     * Calculate direction vector from bodyA to bodyB
     */
    glm::vec3 direction(const dynamics::RigidBody* bodyA, const dynamics::RigidBody* bodyB);
    
    /**
     * Apply force at world position
     */
    void applyForceAtWorldPosition(dynamics::RigidBody* body, const glm::vec3& force, const glm::vec3& worldPos);
    
    /**
     * Calculate relative velocity between two bodies at contact point
     */
    glm::vec3 relativeVelocity(const dynamics::RigidBody* bodyA, const dynamics::RigidBody* bodyB, const glm::vec3& contactPoint);
}

} // namespace forces
} // namespace physics
} // namespace ohao