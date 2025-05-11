#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ohao {

// Forward declarations
class CollisionShape;

// Simple stub for RigidBody class - will be implemented fully later
class RigidBody {
public:
    RigidBody() = default;
    ~RigidBody() = default;
    
    // Basic properties
    void setMass(float mass);
    float getMass() const;
    
    // State
    void setPosition(const glm::vec3& position);
    glm::vec3 getPosition() const;
    
    void setRotation(const glm::quat& rotation);
    glm::quat getRotation() const;
    
    // Motion
    void setLinearVelocity(const glm::vec3& velocity);
    glm::vec3 getLinearVelocity() const;
    
    void setAngularVelocity(const glm::vec3& velocity);
    glm::vec3 getAngularVelocity() const;
    
    // Forces
    void applyForce(const glm::vec3& force, const glm::vec3& relativePos = glm::vec3(0.0f));
    void applyTorque(const glm::vec3& torque);
    
    // Impulses
    void applyImpulse(const glm::vec3& impulse, const glm::vec3& relativePos = glm::vec3(0.0f));
    void applyTorqueImpulse(const glm::vec3& torqueImpulse);
    
private:
    float mass{1.0f};
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 linearVelocity{0.0f};
    glm::vec3 angularVelocity{0.0f};
};

} // namespace ohao 