#pragma once

#include "force_generator.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

namespace ohao {
namespace physics {
namespace forces {

/**
 * Spring force between two rigid bodies
 * Implements Hooke's law: F = -k * (x - rest_length) - damping * velocity
 */
class SpringForce : public PairForceGenerator {
public:
    SpringForce(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                float springConstant = 10.0f, float restLength = 1.0f, float damping = 0.5f)
        : PairForceGenerator(bodyA, bodyB), m_springConstant(springConstant), 
          m_restLength(restLength), m_damping(damping) {}
    
    // Spring properties
    void setSpringConstant(float k) { m_springConstant = k; }
    float getSpringConstant() const { return m_springConstant; }
    
    void setRestLength(float length) { m_restLength = length; }
    float getRestLength() const { return m_restLength; }
    
    void setDamping(float damping) { m_damping = damping; }
    float getDamping() const { return m_damping; }
    
    // Attachment points (relative to body centers)
    void setAttachmentPointA(const glm::vec3& point) { m_attachmentPointA = point; }
    glm::vec3 getAttachmentPointA() const { return m_attachmentPointA; }
    
    void setAttachmentPointB(const glm::vec3& point) { m_attachmentPointB = point; }
    glm::vec3 getAttachmentPointB() const { return m_attachmentPointB; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    
    std::string getName() const override { return "SpringForce"; }
    
    // Utilities
    float getCurrentLength() const;
    float getCurrentExtension() const { return getCurrentLength() - m_restLength; }
    glm::vec3 getSpringDirection() const; // From A to B

private:
    float m_springConstant = 10.0f;     // Spring stiffness (N/m)
    float m_restLength = 1.0f;          // Natural length (m)
    float m_damping = 0.5f;             // Damping coefficient
    
    // Attachment points relative to body centers
    glm::vec3 m_attachmentPointA{0.0f}; 
    glm::vec3 m_attachmentPointB{0.0f};
};

/**
 * Spring force to a fixed world position
 * Useful for anchoring objects to specific locations
 */
class AnchorSpringForce : public SingleBodyForceGenerator {
public:
    AnchorSpringForce(dynamics::RigidBody* body, const glm::vec3& anchorPosition,
                      float springConstant = 10.0f, float restLength = 1.0f, float damping = 0.5f)
        : SingleBodyForceGenerator(body), m_anchorPosition(anchorPosition),
          m_springConstant(springConstant), m_restLength(restLength), m_damping(damping) {}
    
    // Spring properties
    void setSpringConstant(float k) { m_springConstant = k; }
    float getSpringConstant() const { return m_springConstant; }
    
    void setRestLength(float length) { m_restLength = length; }
    float getRestLength() const { return m_restLength; }
    
    void setDamping(float damping) { m_damping = damping; }
    float getDamping() const { return m_damping; }
    
    // Anchor and attachment
    void setAnchorPosition(const glm::vec3& position) { m_anchorPosition = position; }
    glm::vec3 getAnchorPosition() const { return m_anchorPosition; }
    
    void setAttachmentPoint(const glm::vec3& point) { m_attachmentPoint = point; }
    glm::vec3 getAttachmentPoint() const { return m_attachmentPoint; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    
    std::string getName() const override { return "AnchorSpringForce"; }
    
    // Utilities
    float getCurrentLength() const;
    float getCurrentExtension() const { return getCurrentLength() - m_restLength; }

private:
    glm::vec3 m_anchorPosition{0.0f};   // World position of anchor
    float m_springConstant = 10.0f;
    float m_restLength = 1.0f;
    float m_damping = 0.5f;
    
    glm::vec3 m_attachmentPoint{0.0f};  // Relative to body center
};

/**
 * Bungee spring force - only applies when stretched beyond rest length
 * No force when compressed (like a bungee cord)
 */
class BungeeSpringForce : public PairForceGenerator {
public:
    BungeeSpringForce(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                      float springConstant = 10.0f, float restLength = 1.0f)
        : PairForceGenerator(bodyA, bodyB), m_springConstant(springConstant), 
          m_restLength(restLength) {}
    
    void setSpringConstant(float k) { m_springConstant = k; }
    float getSpringConstant() const { return m_springConstant; }
    
    void setRestLength(float length) { m_restLength = length; }
    float getRestLength() const { return m_restLength; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    
    std::string getName() const override { return "BungeeSpringForce"; }

private:
    float m_springConstant = 10.0f;
    float m_restLength = 1.0f;
};

/**
 * Angular spring force (torsion spring)
 * Applies torque to restore bodies to a relative orientation
 */
class AngularSpringForce : public PairForceGenerator {
public:
    AngularSpringForce(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                       float springConstant = 5.0f, float damping = 0.2f);
    
    void setSpringConstant(float k) { m_springConstant = k; }
    float getSpringConstant() const { return m_springConstant; }
    
    void setDamping(float damping) { m_damping = damping; }
    float getDamping() const { return m_damping; }
    
    void setRestOrientation(const glm::quat& orientation) { m_restOrientation = orientation; }
    glm::quat getRestOrientation() const { return m_restOrientation; }
    
    void applyForce(dynamics::RigidBody* body, float deltaTime) override;
    
    std::string getName() const override { return "AngularSpringForce"; }

private:
    float m_springConstant = 5.0f;
    float m_damping = 0.2f;
    glm::quat m_restOrientation = glm::quat(1, 0, 0, 0); // Relative rest orientation
};

} // namespace forces
} // namespace physics
} // namespace ohao