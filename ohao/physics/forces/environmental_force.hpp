#pragma once

#include "force_generator.hpp"
#include <glm/glm.hpp>

namespace ohao {
namespace physics {
namespace forces {

/**
 * Buoyancy force - simulates floating in fluids
 * Applies upward force based on submerged volume
 */
class BuoyancyForce : public GlobalForceGenerator {
public:
    BuoyancyForce(float fluidDensity = 1000.0f, float liquidLevel = 0.0f, 
                  const glm::vec3& liquidNormal = glm::vec3(0, 1, 0))
        : m_fluidDensity(fluidDensity), m_liquidLevel(liquidLevel), 
          m_liquidNormal(glm::normalize(liquidNormal)) {}
    
    void setFluidDensity(float density) { m_fluidDensity = density; }
    float getFluidDensity() const { return m_fluidDensity; }
    
    void setLiquidLevel(float level) { m_liquidLevel = level; }
    float getLiquidLevel() const { return m_liquidLevel; }
    
    void setLiquidNormal(const glm::vec3& normal) { m_liquidNormal = glm::normalize(normal); }
    glm::vec3 getLiquidNormal() const { return m_liquidNormal; }
    
    // Drag coefficient for objects moving through fluid
    void setFluidDrag(float drag) { m_fluidDrag = drag; }
    float getFluidDrag() const { return m_fluidDrag; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    bool shouldAffectBody(dynamics::RigidBody* body) const override;
    
    std::string getName() const override { return "BuoyancyForce"; }

private:
    float m_fluidDensity = 1000.0f;     // kg/m³ (water ≈ 1000)
    float m_liquidLevel = 0.0f;         // Y-coordinate of liquid surface
    glm::vec3 m_liquidNormal{0, 1, 0};  // Normal vector of liquid surface
    float m_fluidDrag = 0.1f;           // Additional drag when submerged
    
    // Helper functions
    float calculateSubmergedVolume(dynamics::RigidBody* body) const;
    float getSubmersionDepth(dynamics::RigidBody* body) const;
};

/**
 * Wind force - directional force with turbulence
 * Simulates wind effects with gusts and variations
 */
class WindForce : public GlobalForceGenerator {
public:
    WindForce(const glm::vec3& direction = glm::vec3(1, 0, 0), float strength = 10.0f)
        : m_direction(glm::normalize(direction)), m_strength(strength) {}
    
    void setDirection(const glm::vec3& direction) { m_direction = glm::normalize(direction); }
    glm::vec3 getDirection() const { return m_direction; }
    
    void setStrength(float strength) { m_strength = strength; }
    float getStrength() const { return m_strength; }
    
    // Turbulence parameters
    void setTurbulence(float intensity, float frequency = 1.0f) {
        m_turbulenceIntensity = intensity;
        m_turbulenceFrequency = frequency;
    }
    
    float getTurbulenceIntensity() const { return m_turbulenceIntensity; }
    float getTurbulenceFrequency() const { return m_turbulenceFrequency; }
    
    // Altitude effects (wind strength varies with height)
    void setAltitudeRange(float minHeight, float maxHeight, float heightMultiplier = 1.5f) {
        m_minHeight = minHeight;
        m_maxHeight = maxHeight;
        m_heightMultiplier = heightMultiplier;
        m_useAltitudeEffect = true;
    }
    
    void disableAltitudeEffect() { m_useAltitudeEffect = false; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    bool shouldAffectBody(dynamics::RigidBody* body) const override;
    
    std::string getName() const override { return "WindForce"; }

private:
    glm::vec3 m_direction{1.0f, 0.0f, 0.0f};
    float m_strength = 10.0f;
    float m_turbulenceIntensity = 0.1f;
    float m_turbulenceFrequency = 1.0f;
    float m_time = 0.0f;
    
    // Altitude effects
    bool m_useAltitudeEffect = false;
    float m_minHeight = 0.0f;
    float m_maxHeight = 100.0f;
    float m_heightMultiplier = 1.5f;
    
    // Noise function for turbulence
    float noise(float x, float y, float z) const;
};

/**
 * Magnetic force - attraction/repulsion based on magnetic properties
 * Useful for magnetic objects, electromagnetic effects
 */
class MagneticForce : public PairForceGenerator {
public:
    MagneticForce(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                  float magneticStrengthA = 1.0f, float magneticStrengthB = 1.0f)
        : PairForceGenerator(bodyA, bodyB), m_magneticStrengthA(magneticStrengthA), 
          m_magneticStrengthB(magneticStrengthB) {}
    
    void setMagneticStrengthA(float strength) { m_magneticStrengthA = strength; }
    float getMagneticStrengthA() const { return m_magneticStrengthA; }
    
    void setMagneticStrengthB(float strength) { m_magneticStrengthB = strength; }
    float getMagneticStrengthB() const { return m_magneticStrengthB; }
    
    void setMaxDistance(float distance) { m_maxDistance = distance; }
    float getMaxDistance() const { return m_maxDistance; }
    
    void setMinDistance(float distance) { m_minDistance = distance; }
    float getMinDistance() const { return m_minDistance; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    
    std::string getName() const override { return "MagneticForce"; }

private:
    float m_magneticStrengthA = 1.0f;   // Positive = north pole, negative = south pole
    float m_magneticStrengthB = 1.0f;
    float m_maxDistance = 10.0f;        // Beyond this, no magnetic force
    float m_minDistance = 0.1f;         // Prevents infinite force
};

/**
 * Surface tension force - simulates liquid surface effects
 * Creates forces that minimize surface area
 */
class SurfaceTensionForce : public GlobalForceGenerator {
public:
    explicit SurfaceTensionForce(float surfaceTension = 0.072f) // Water surface tension
        : m_surfaceTension(surfaceTension) {}
    
    void setSurfaceTension(float tension) { m_surfaceTension = tension; }
    float getSurfaceTension() const { return m_surfaceTension; }
    
    void setLiquidLevel(float level) { m_liquidLevel = level; }
    float getLiquidLevel() const { return m_liquidLevel; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    bool shouldAffectBody(dynamics::RigidBody* body) const override;
    
    std::string getName() const override { return "SurfaceTensionForce"; }

private:
    float m_surfaceTension = 0.072f;    // N/m (surface tension coefficient)
    float m_liquidLevel = 0.0f;         // Level of liquid surface
    float m_influenceRadius = 1.0f;     // How far surface tension acts
};

} // namespace forces
} // namespace physics
} // namespace ohao