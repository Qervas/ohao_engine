#pragma once

#include "force_generator.hpp"
#include <glm/glm.hpp>

namespace ohao {
namespace physics {
namespace forces {

/**
 * Explosion force - radial force emanating from a point
 * Useful for explosions, blasts, repulsion effects
 */
class ExplosionForce : public GlobalForceGenerator {
public:
    ExplosionForce(const glm::vec3& center, float maxForce = 1000.0f, float maxRadius = 10.0f)
        : m_center(center), m_maxForce(maxForce), m_maxRadius(maxRadius) {}
    
    void setCenter(const glm::vec3& center) { m_center = center; }
    glm::vec3 getCenter() const { return m_center; }
    
    void setMaxForce(float force) { m_maxForce = force; }
    float getMaxForce() const { return m_maxForce; }
    
    void setMaxRadius(float radius) { m_maxRadius = radius; }
    float getMaxRadius() const { return m_maxRadius; }
    
    void setMinRadius(float radius) { m_minRadius = radius; }
    float getMinRadius() const { return m_minRadius; }
    
    // Force falloff over distance
    enum class FalloffType {
        LINEAR,     // Linear decrease
        QUADRATIC,  // Inverse square law (realistic)
        CONSTANT    // Constant force within radius
    };
    
    void setFalloffType(FalloffType type) { m_falloffType = type; }
    FalloffType getFalloffType() const { return m_falloffType; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    bool shouldAffectBody(dynamics::RigidBody* body) const override;
    
    std::string getName() const override { return "ExplosionForce"; }

private:
    glm::vec3 m_center{0.0f};
    float m_maxForce = 1000.0f;
    float m_maxRadius = 10.0f;
    float m_minRadius = 0.1f;   // Minimum radius to prevent infinite force
    FalloffType m_falloffType = FalloffType::QUADRATIC;
};

/**
 * Implosion force - pulls objects toward a center point
 * Useful for black holes, vacuum effects, attraction
 */
class ImplosionForce : public GlobalForceGenerator {
public:
    ImplosionForce(const glm::vec3& center, float maxForce = 1000.0f, float maxRadius = 10.0f)
        : m_center(center), m_maxForce(maxForce), m_maxRadius(maxRadius) {}
    
    void setCenter(const glm::vec3& center) { m_center = center; }
    glm::vec3 getCenter() const { return m_center; }
    
    void setMaxForce(float force) { m_maxForce = force; }
    float getMaxForce() const { return m_maxForce; }
    
    void setMaxRadius(float radius) { m_maxRadius = radius; }
    float getMaxRadius() const { return m_maxRadius; }
    
    void setMinRadius(float radius) { m_minRadius = radius; }
    float getMinRadius() const { return m_minRadius; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    bool shouldAffectBody(dynamics::RigidBody* body) const override;
    
    std::string getName() const override { return "ImplosionForce"; }

private:
    glm::vec3 m_center{0.0f};
    float m_maxForce = 1000.0f;
    float m_maxRadius = 10.0f;
    float m_minRadius = 0.1f;
};

/**
 * Vortex force - creates swirling motion around an axis
 * Useful for tornadoes, whirlpools, spiral effects
 */
class VortexForce : public GlobalForceGenerator {
public:
    VortexForce(const glm::vec3& center, const glm::vec3& axis = glm::vec3(0, 1, 0),
                float strength = 100.0f, float maxRadius = 10.0f)
        : m_center(center), m_axis(glm::normalize(axis)), m_strength(strength), m_maxRadius(maxRadius) {}
    
    void setCenter(const glm::vec3& center) { m_center = center; }
    glm::vec3 getCenter() const { return m_center; }
    
    void setAxis(const glm::vec3& axis) { m_axis = glm::normalize(axis); }
    glm::vec3 getAxis() const { return m_axis; }
    
    void setStrength(float strength) { m_strength = strength; }
    float getStrength() const { return m_strength; }
    
    void setMaxRadius(float radius) { m_maxRadius = radius; }
    float getMaxRadius() const { return m_maxRadius; }
    
    // Upward/downward force component (for tornado effect)
    void setLiftForce(float lift) { m_liftForce = lift; }
    float getLiftForce() const { return m_liftForce; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    bool shouldAffectBody(dynamics::RigidBody* body) const override;
    
    std::string getName() const override { return "VortexForce"; }

private:
    glm::vec3 m_center{0.0f};
    glm::vec3 m_axis{0.0f, 1.0f, 0.0f};
    float m_strength = 100.0f;
    float m_maxRadius = 10.0f;
    float m_liftForce = 0.0f;   // Additional upward force
};

/**
 * Directional force field - applies force in a specific direction
 * Useful for wind, current, conveyor belts
 */
class DirectionalFieldForce : public GlobalForceGenerator {
public:
    DirectionalFieldForce(const glm::vec3& direction, float strength = 10.0f)
        : m_direction(glm::normalize(direction)), m_strength(strength) {}
    
    void setDirection(const glm::vec3& direction) { m_direction = glm::normalize(direction); }
    glm::vec3 getDirection() const { return m_direction; }
    
    void setStrength(float strength) { m_strength = strength; }
    float getStrength() const { return m_strength; }
    
    // Optional bounds for the field
    void setBounds(const glm::vec3& minBounds, const glm::vec3& maxBounds) {
        m_minBounds = minBounds;
        m_maxBounds = maxBounds;
        m_hasBounds = true;
    }
    
    void removeBounds() { m_hasBounds = false; }
    bool hasBounds() const { return m_hasBounds; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    bool shouldAffectBody(dynamics::RigidBody* body) const override;
    
    std::string getName() const override { return "DirectionalFieldForce"; }

private:
    glm::vec3 m_direction{1.0f, 0.0f, 0.0f};
    float m_strength = 10.0f;
    bool m_hasBounds = false;
    glm::vec3 m_minBounds{-1000.0f};
    glm::vec3 m_maxBounds{1000.0f};
};

/**
 * Turbulence force - random force variations
 * Useful for chaotic motion, particle effects
 */
class TurbulenceForce : public GlobalForceGenerator {
public:
    explicit TurbulenceForce(float intensity = 10.0f, float frequency = 1.0f)
        : m_intensity(intensity), m_frequency(frequency) {}
    
    void setIntensity(float intensity) { m_intensity = intensity; }
    float getIntensity() const { return m_intensity; }
    
    void setFrequency(float frequency) { m_frequency = frequency; }
    float getFrequency() const { return m_frequency; }
    
    void setSeed(unsigned int seed) { m_seed = seed; }
    unsigned int getSeed() const { return m_seed; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    bool shouldAffectBody(dynamics::RigidBody* body) const override;
    
    std::string getName() const override { return "TurbulenceForce"; }

private:
    float m_intensity = 10.0f;
    float m_frequency = 1.0f;
    unsigned int m_seed = 12345;
    float m_time = 0.0f;
    
    // Simple noise functions
    float noise3D(float x, float y, float z) const;
    glm::vec3 noiseVector3D(const glm::vec3& pos) const;
};

} // namespace forces
} // namespace physics
} // namespace ohao