#pragma once

#include "force_generator.hpp"
#include <glm/glm.hpp>

namespace ohao {
namespace physics {
namespace forces {

/**
 * Linear drag force (air resistance)
 * F = -k1 * v (where v is velocity)
 */
class LinearDragForce : public GlobalForceGenerator {
public:
    explicit LinearDragForce(float dragCoefficient = 0.1f)
        : m_dragCoefficient(dragCoefficient) {}
    
    void setDragCoefficient(float coefficient) { m_dragCoefficient = coefficient; }
    float getDragCoefficient() const { return m_dragCoefficient; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    bool shouldAffectBody(dynamics::RigidBody* body) const override;
    
    std::string getName() const override { return "LinearDragForce"; }

private:
    float m_dragCoefficient = 0.1f;
};

/**
 * Quadratic drag force (realistic air resistance)
 * F = -k2 * |v| * v (more realistic for high speeds)
 */
class QuadraticDragForce : public GlobalForceGenerator {
public:
    explicit QuadraticDragForce(float dragCoefficient = 0.01f)
        : m_dragCoefficient(dragCoefficient) {}
    
    void setDragCoefficient(float coefficient) { m_dragCoefficient = coefficient; }
    float getDragCoefficient() const { return m_dragCoefficient; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    bool shouldAffectBody(dynamics::RigidBody* body) const override;
    
    std::string getName() const override { return "QuadraticDragForce"; }

private:
    float m_dragCoefficient = 0.01f;
};

/**
 * Combined linear + quadratic drag (most realistic)
 * F = -k1 * v - k2 * |v| * v
 */
class CombinedDragForce : public GlobalForceGenerator {
public:
    CombinedDragForce(float linearCoeff = 0.05f, float quadraticCoeff = 0.01f)
        : m_linearCoeff(linearCoeff), m_quadraticCoeff(quadraticCoeff) {}
    
    void setLinearCoefficient(float coeff) { m_linearCoeff = coeff; }
    float getLinearCoefficient() const { return m_linearCoeff; }
    
    void setQuadraticCoefficient(float coeff) { m_quadraticCoeff = coeff; }
    float getQuadraticCoefficient() const { return m_quadraticCoeff; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    bool shouldAffectBody(dynamics::RigidBody* body) const override;
    
    std::string getName() const override { return "CombinedDragForce"; }

private:
    float m_linearCoeff = 0.05f;
    float m_quadraticCoeff = 0.01f;
};

/**
 * Angular drag force (rotational damping)
 * Applies drag to angular velocity
 */
class AngularDragForce : public GlobalForceGenerator {
public:
    explicit AngularDragForce(float angularDragCoeff = 0.1f)
        : m_angularDragCoeff(angularDragCoeff) {}
    
    void setAngularDragCoefficient(float coeff) { m_angularDragCoeff = coeff; }
    float getAngularDragCoefficient() const { return m_angularDragCoeff; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    bool shouldAffectBody(dynamics::RigidBody* body) const override;
    
    std::string getName() const override { return "AngularDragForce"; }

private:
    float m_angularDragCoeff = 0.1f;
};

/**
 * Fluid drag force with density
 * More physically accurate drag that considers fluid properties
 */
class FluidDragForce : public GlobalForceGenerator {
public:
    FluidDragForce(float fluidDensity = 1.2f, float dragCoeff = 0.47f, float crossSectionArea = 1.0f)
        : m_fluidDensity(fluidDensity), m_dragCoeff(dragCoeff), m_crossSectionArea(crossSectionArea) {}
    
    void setFluidDensity(float density) { m_fluidDensity = density; }
    float getFluidDensity() const { return m_fluidDensity; }
    
    void setDragCoefficient(float coeff) { m_dragCoeff = coeff; }
    float getDragCoefficient() const { return m_dragCoeff; }
    
    void setCrossSectionArea(float area) { m_crossSectionArea = area; }
    float getCrossSectionArea() const { return m_crossSectionArea; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    bool shouldAffectBody(dynamics::RigidBody* body) const override;
    
    std::string getName() const override { return "FluidDragForce"; }

private:
    float m_fluidDensity = 1.2f;        // kg/m³ (air at sea level)
    float m_dragCoeff = 0.47f;          // Drag coefficient (sphere ≈ 0.47)
    float m_crossSectionArea = 1.0f;    // m² cross-sectional area
};

} // namespace forces
} // namespace physics
} // namespace ohao