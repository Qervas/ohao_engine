#include "field_force.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include <algorithm>
#include <cmath>
#include <random>

namespace ohao {
namespace physics {
namespace forces {

// ExplosionForce implementation
void ExplosionForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!body || body->isStatic()) return;
    
    glm::vec3 bodyPos = body->getPosition();
    glm::vec3 direction = bodyPos - m_center;
    float distance = glm::length(direction);
    
    // Check bounds
    if (distance < m_minRadius || distance > m_maxRadius) {
        return;
    }
    
    // Normalize direction (outward from explosion center)
    direction /= distance;
    
    // Calculate force magnitude based on falloff type
    float forceMagnitude = 0.0f;
    switch (m_falloffType) {
        case FalloffType::LINEAR:
            forceMagnitude = m_maxForce * (1.0f - (distance - m_minRadius) / (m_maxRadius - m_minRadius));
            break;
        case FalloffType::QUADRATIC:
            forceMagnitude = m_maxForce / (distance * distance);
            break;
        case FalloffType::CONSTANT:
            forceMagnitude = m_maxForce;
            break;
    }
    
    glm::vec3 force = direction * forceMagnitude;
    body->applyForce(force);
}

bool ExplosionForce::shouldAffectBody(dynamics::RigidBody* body) const {
    if (!body || body->isStatic()) return false;
    
    float distance = glm::length(body->getPosition() - m_center);
    return distance >= m_minRadius && distance <= m_maxRadius;
}

// ImplosionForce implementation
void ImplosionForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!body || body->isStatic()) return;
    
    glm::vec3 bodyPos = body->getPosition();
    glm::vec3 direction = m_center - bodyPos; // Inward toward center
    float distance = glm::length(direction);
    
    // Check bounds
    if (distance < m_minRadius || distance > m_maxRadius) {
        return;
    }
    
    // Normalize direction (inward toward center)
    direction /= distance;
    
    // Force magnitude increases closer to center (inverse square)
    float forceMagnitude = m_maxForce / (distance * distance);
    
    glm::vec3 force = direction * forceMagnitude;
    body->applyForce(force);
}

bool ImplosionForce::shouldAffectBody(dynamics::RigidBody* body) const {
    if (!body || body->isStatic()) return false;
    
    float distance = glm::length(body->getPosition() - m_center);
    return distance >= m_minRadius && distance <= m_maxRadius;
}

// VortexForce implementation
void VortexForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!body || body->isStatic()) return;
    
    glm::vec3 bodyPos = body->getPosition();
    
    // Vector from center to body
    glm::vec3 centerToBody = bodyPos - m_center;
    
    // Project onto the plane perpendicular to vortex axis
    float axisComponent = glm::dot(centerToBody, m_axis);
    glm::vec3 axisProjection = axisComponent * m_axis;
    glm::vec3 radialVector = centerToBody - axisProjection;
    
    float radius = glm::length(radialVector);
    if (radius > m_maxRadius || radius < 0.1f) return;
    
    // Normalize radial vector
    radialVector /= radius;
    
    // Calculate tangential direction (perpendicular to both axis and radial)
    glm::vec3 tangentialDirection = glm::normalize(glm::cross(m_axis, radialVector));
    
    // Force magnitude decreases with distance
    float forceMagnitude = m_strength * (1.0f - radius / m_maxRadius);
    
    // Apply tangential force for swirl
    glm::vec3 tangentialForce = tangentialDirection * forceMagnitude;
    body->applyForce(tangentialForce);
    
    // Optional lift force along axis
    if (m_liftForce != 0.0f) {
        glm::vec3 liftForceVec = m_axis * m_liftForce * (1.0f - radius / m_maxRadius);
        body->applyForce(liftForceVec);
    }
}

bool VortexForce::shouldAffectBody(dynamics::RigidBody* body) const {
    if (!body || body->isStatic()) return false;
    
    glm::vec3 centerToBody = body->getPosition() - m_center;
    glm::vec3 radialVector = centerToBody - glm::dot(centerToBody, m_axis) * m_axis;
    float radius = glm::length(radialVector);
    
    return radius <= m_maxRadius;
}

// DirectionalFieldForce implementation
void DirectionalFieldForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!body || body->isStatic()) return;
    
    glm::vec3 force = m_direction * m_strength * body->getMass();
    body->applyForce(force);
}

bool DirectionalFieldForce::shouldAffectBody(dynamics::RigidBody* body) const {
    if (!body || body->isStatic()) return false;
    
    if (!m_hasBounds) return true;
    
    glm::vec3 pos = body->getPosition();
    return pos.x >= m_minBounds.x && pos.x <= m_maxBounds.x &&
           pos.y >= m_minBounds.y && pos.y <= m_maxBounds.y &&
           pos.z >= m_minBounds.z && pos.z <= m_maxBounds.z;
}

// TurbulenceForce implementation
void TurbulenceForce::applyForce(dynamics::RigidBody* body, float deltaTime) {
    if (!body || body->isStatic()) return;
    
    m_time += deltaTime;
    
    // Generate turbulent force using position and time
    glm::vec3 pos = body->getPosition();
    glm::vec3 turbulentForce = noiseVector3D(pos + glm::vec3(m_time * m_frequency)) * m_intensity;
    
    body->applyForce(turbulentForce);
}

bool TurbulenceForce::shouldAffectBody(dynamics::RigidBody* body) const {
    return body && !body->isStatic();
}

// Simple noise implementation (basic Perlin-like noise)
float TurbulenceForce::noise3D(float x, float y, float z) const {
    // Simple hash-based noise function
    int xi = (int)x;
    int yi = (int)y;
    int zi = (int)z;
    
    // Hash function
    int hash = ((xi * 73856093) ^ (yi * 19349663) ^ (zi * 83492791) ^ m_seed) & 0x7fffffff;
    
    // Convert to -1 to 1 range
    return (float)(hash % 2000) / 1000.0f - 1.0f;
}

glm::vec3 TurbulenceForce::noiseVector3D(const glm::vec3& pos) const {
    return glm::vec3(
        noise3D(pos.x, pos.y, pos.z),
        noise3D(pos.x + 100.0f, pos.y, pos.z),
        noise3D(pos.x, pos.y + 100.0f, pos.z)
    );
}

} // namespace forces
} // namespace physics
} // namespace ohao