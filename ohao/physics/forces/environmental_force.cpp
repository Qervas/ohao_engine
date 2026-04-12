#include "environmental_force.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include <algorithm>
#include <cmath>

namespace ohao {
namespace physics {
namespace forces {

// BuoyancyForce implementation
void BuoyancyForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!body) return;
    
    float submersionDepth = getSubmersionDepth(body);
    if (submersionDepth <= 0.0f) return; // Not submerged
    
    // Calculate submerged volume (simplified as fraction of body)
    float submergedVolume = calculateSubmergedVolume(body);
    if (submergedVolume <= 0.0f) return;
    
    // Buoyant force: F = ρ * V * g (Archimedes' principle)
    // Using gravity magnitude of 9.81 as default
    float buoyantForceMagnitude = m_fluidDensity * submergedVolume * 9.81f;
    glm::vec3 buoyantForce = m_liquidNormal * buoyantForceMagnitude;
    
    body->applyForce(buoyantForce);
    
    // Apply fluid drag if object is moving through fluid
    if (m_fluidDrag > 0.0f && submersionDepth > 0.0f) {
        glm::vec3 velocity = body->getLinearVelocity();
        glm::vec3 dragForce = -m_fluidDrag * velocity * submersionDepth;
        body->applyForce(dragForce);
    }
}

bool BuoyancyForce::shouldAffectBody(dynamics::RigidBody* body) const {
    if (!body) return false;
    return getSubmersionDepth(body) > 0.0f;
}

float BuoyancyForce::getSubmersionDepth(dynamics::RigidBody* body) const {
    if (!body) return 0.0f;
    
    // Calculate distance from body center to liquid surface
    glm::vec3 bodyPos = body->getPosition();
    float distanceToSurface = glm::dot(bodyPos - glm::vec3(0, m_liquidLevel, 0), m_liquidNormal);
    
    // Positive distance means above surface, negative means below
    return -distanceToSurface;
}

float BuoyancyForce::calculateSubmergedVolume(dynamics::RigidBody* body) const {
    if (!body) return 0.0f;
    
    float submersionDepth = getSubmersionDepth(body);
    if (submersionDepth <= 0.0f) return 0.0f;
    
    // Simplified: assume spherical body with radius based on mass
    // Real implementation would use actual collision shape
    float approximateRadius = std::pow(body->getMass(), 1.0f / 3.0f);
    
    // Calculate submerged fraction (simplified)
    float submersionFraction = std::min(1.0f, submersionDepth / (2.0f * approximateRadius));
    
    // Volume of sphere: (4/3) * π * r³
    float totalVolume = (4.0f / 3.0f) * glm::pi<float>() * approximateRadius * approximateRadius * approximateRadius;
    
    return totalVolume * submersionFraction;
}

// WindForce implementation
void WindForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!body || body->isStatic()) return;
    
    m_time += deltaTime;
    
    // Base wind force
    glm::vec3 windForce = m_direction * m_strength;
    
    // Add turbulence
    if (m_turbulenceIntensity > 0.0f) {
        glm::vec3 bodyPos = body->getPosition();
        float turbulenceX = noise(bodyPos.x * 0.1f, bodyPos.y * 0.1f, m_time * m_turbulenceFrequency);
        float turbulenceY = noise(bodyPos.x * 0.1f + 100.0f, bodyPos.y * 0.1f, m_time * m_turbulenceFrequency);
        float turbulenceZ = noise(bodyPos.x * 0.1f, bodyPos.y * 0.1f + 100.0f, m_time * m_turbulenceFrequency);
        
        glm::vec3 turbulence = glm::vec3(turbulenceX, turbulenceY, turbulenceZ) * m_turbulenceIntensity * m_strength;
        windForce += turbulence;
    }
    
    // Apply altitude effects
    if (m_useAltitudeEffect) {
        float height = body->getPosition().y;
        float heightFactor = 1.0f;
        
        if (height > m_maxHeight) {
            heightFactor = m_heightMultiplier;
        } else if (height > m_minHeight) {
            float t = (height - m_minHeight) / (m_maxHeight - m_minHeight);
            heightFactor = 1.0f + t * (m_heightMultiplier - 1.0f);
        }
        
        windForce *= heightFactor;
    }
    
    body->applyForce(windForce);
}

bool WindForce::shouldAffectBody(dynamics::RigidBody* body) const {
    return body && !body->isStatic();
}

float WindForce::noise(float x, float y, float z) const {
    // Simple hash-based noise
    int xi = (int)std::floor(x);
    int yi = (int)std::floor(y);
    int zi = (int)std::floor(z);
    
    int hash = ((xi * 73856093) ^ (yi * 19349663) ^ (zi * 83492791)) & 0x7fffffff;
    return (float)(hash % 2000) / 1000.0f - 1.0f;
}

// MagneticForce implementation
void MagneticForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!m_bodyA || !m_bodyB) return;
    
    glm::vec3 posA = m_bodyA->getPosition();
    glm::vec3 posB = m_bodyB->getPosition();
    glm::vec3 direction = posB - posA;
    float distance = glm::length(direction);
    
    // Check distance bounds
    if (distance < m_minDistance || distance > m_maxDistance) {
        return;
    }
    
    direction /= distance;
    
    // Magnetic force: F = k * (m1 * m2) / r²
    // Same polarity repels, opposite attracts
    float forceMagnitude = (m_magneticStrengthA * m_magneticStrengthB) / (distance * distance);
    
    // If forces have same sign, they repel; if opposite sign, they attract
    glm::vec3 force = direction * forceMagnitude;
    
    // Apply forces (Newton's third law)
    if (body == m_bodyA) {
        body->applyForce(force);
    } else if (body == m_bodyB) {
        body->applyForce(-force);
    }
}

// SurfaceTensionForce implementation
void SurfaceTensionForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!body) return;
    
    glm::vec3 bodyPos = body->getPosition();
    float distanceFromSurface = std::abs(bodyPos.y - m_liquidLevel);
    
    // Only apply surface tension near the liquid surface
    if (distanceFromSurface > m_influenceRadius) return;
    
    // Surface tension tries to minimize surface area
    // Apply force toward surface if body is near it
    float forceMagnitude = m_surfaceTension * (1.0f - distanceFromSurface / m_influenceRadius);
    
    glm::vec3 forceDirection = (bodyPos.y > m_liquidLevel) ? 
        glm::vec3(0, -1, 0) : glm::vec3(0, 1, 0);
    
    glm::vec3 surfaceForce = forceDirection * forceMagnitude;
    body->applyForce(surfaceForce);
}

bool SurfaceTensionForce::shouldAffectBody(dynamics::RigidBody* body) const {
    if (!body) return false;
    
    float distanceFromSurface = std::abs(body->getPosition().y - m_liquidLevel);
    return distanceFromSurface <= m_influenceRadius;
}

} // namespace forces
} // namespace physics
} // namespace ohao