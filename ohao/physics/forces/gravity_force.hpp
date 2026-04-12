#pragma once

#include "force_generator.hpp"
#include <glm/glm.hpp>

namespace ohao {
namespace physics {
namespace forces {

/**
 * Enhanced gravity force generator
 * More flexible than the built-in world gravity
 */
class GravityForce : public GlobalForceGenerator {
public:
    explicit GravityForce(const glm::vec3& gravity = glm::vec3(0.0f, -9.81f, 0.0f))
        : m_gravity(gravity) {}
    
    void setGravity(const glm::vec3& gravity) { m_gravity = gravity; }
    glm::vec3 getGravity() const { return m_gravity; }
    
    // GlobalForceGenerator interface
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    bool shouldAffectBody(dynamics::RigidBody* body) const override;
    
    std::string getName() const override { return "GravityForce"; }
    
    /**
     * Set mass scale factor (default 1.0)
     * Values < 1.0 = lighter gravity, > 1.0 = heavier gravity
     */
    void setMassScale(float scale) { m_massScale = scale; }
    float getMassScale() const { return m_massScale; }
    
    /**
     * Enable/disable affecting static bodies
     */
    void setAffectStatic(bool affect) { m_affectStatic = affect; }
    bool getAffectStatic() const { return m_affectStatic; }

private:
    glm::vec3 m_gravity{0.0f, -9.81f, 0.0f};
    float m_massScale = 1.0f;
    bool m_affectStatic = false; // Usually gravity doesn't affect static bodies
};

/**
 * Directional gravity (like a planet's gravity with a direction)
 */
class DirectionalGravityForce : public GlobalForceGenerator {
public:
    DirectionalGravityForce(const glm::vec3& direction, float strength = 9.81f)
        : m_direction(glm::normalize(direction)), m_strength(strength) {}
    
    void setDirection(const glm::vec3& direction) { m_direction = glm::normalize(direction); }
    glm::vec3 getDirection() const { return m_direction; }
    
    void setStrength(float strength) { m_strength = strength; }
    float getStrength() const { return m_strength; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    bool shouldAffectBody(dynamics::RigidBody* body) const override;
    
    std::string getName() const override { return "DirectionalGravityForce"; }

private:
    glm::vec3 m_direction{0.0f, -1.0f, 0.0f};
    float m_strength = 9.81f;
    bool m_affectStatic = false;
};

/**
 * Point gravity (like a black hole or planet)
 */
class PointGravityForce : public GlobalForceGenerator {
public:
    PointGravityForce(const glm::vec3& center, float strength = 100.0f)
        : m_center(center), m_strength(strength) {}
    
    void setCenter(const glm::vec3& center) { m_center = center; }
    glm::vec3 getCenter() const { return m_center; }
    
    void setStrength(float strength) { m_strength = strength; }
    float getStrength() const { return m_strength; }
    
    void setMinDistance(float minDist) { m_minDistance = minDist; }
    float getMinDistance() const { return m_minDistance; }
    
    void setMaxDistance(float maxDist) { m_maxDistance = maxDist; }
    float getMaxDistance() const { return m_maxDistance; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    bool shouldAffectBody(dynamics::RigidBody* body) const override;
    
    std::string getName() const override { return "PointGravityForce"; }

private:
    glm::vec3 m_center{0.0f};
    float m_strength = 100.0f;
    float m_minDistance = 0.1f;  // Prevents infinite force at zero distance
    float m_maxDistance = 1000.0f; // Beyond this distance, no force is applied
    bool m_affectStatic = false;
};

} // namespace forces
} // namespace physics
} // namespace ohao