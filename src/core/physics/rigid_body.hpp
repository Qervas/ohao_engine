#pragma once

#include <memory>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ohao {

class PhysicsComponent;
class CollisionShape;

enum class RigidBodyType {
    STATIC = 0,     // Never moves (ground, walls)
    KINEMATIC = 1,  // Moves but not affected by forces
    DYNAMIC = 2     // Full physics simulation
};

class RigidBody {
public:
    RigidBody(PhysicsComponent* component);
    ~RigidBody();
    
    // Physics properties
    void setMass(float mass);
    float getMass() const;
    
    void setRestitution(float restitution); // Bounciness
    float getRestitution() const;
    
    void setFriction(float friction);
    float getFriction() const;
    
    void setLinearDamping(float damping);
    float getLinearDamping() const;
    
    void setAngularDamping(float damping);
    float getAngularDamping() const;
    
    // Transform
    void setPosition(const glm::vec3& position);
    glm::vec3 getPosition() const;
    
    void setRotation(const glm::quat& rotation);
    glm::quat getRotation() const;
    
    void setTransform(const glm::vec3& position, const glm::quat& rotation);
    glm::mat4 getTransformMatrix() const;
    
    // Velocity
    void setLinearVelocity(const glm::vec3& velocity);
    glm::vec3 getLinearVelocity() const;
    
    void setAngularVelocity(const glm::vec3& velocity);
    glm::vec3 getAngularVelocity() const;
    
    // Forces
    void applyForce(const glm::vec3& force, const glm::vec3& relativePos = glm::vec3(0.0f));
    void applyImpulse(const glm::vec3& impulse, const glm::vec3& relativePos = glm::vec3(0.0f));
    void applyTorque(const glm::vec3& torque);
    void clearForces();
    glm::vec3 getAccumulatedForce() const; 
    
    // Collision shape
    void setCollisionShape(std::shared_ptr<CollisionShape> shape);
    std::shared_ptr<CollisionShape> getCollisionShape() const;
    
    // Type
    void setType(RigidBodyType type);
    RigidBodyType getType() const;
    
    // Activation
    void activate();
    void setActivationState(bool active);
    bool isActive() const;
    
    // Component reference
    PhysicsComponent* getComponent() const;
    
    // TODO: Update transform from physics simulation
    void updateTransform();
    
private:
    // TODO: Add Bullet Physics rigid body pointer
    // btRigidBody* m_bulletBody{nullptr};
    
    PhysicsComponent* m_component;
    std::shared_ptr<CollisionShape> m_collisionShape;
    RigidBodyType m_type{RigidBodyType::DYNAMIC};
    
    // Properties cache (for when Bullet is not available)
    float m_mass{1.0f};
    float m_restitution{0.0f};
    float m_friction{0.5f};
    float m_linearDamping{0.0f};
    float m_angularDamping{0.0f};
    
    glm::vec3 m_position{0.0f};
    glm::quat m_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 m_linearVelocity{0.0f};
    glm::vec3 m_angularVelocity{0.0f};
    glm::vec3 m_accumulatedForce{0.0f};

  
   
    
    // TODO: Implementation methods
    void createBulletRigidBody();
    void updateBulletProperties();
    void syncFromBullet();
    void syncToBullet();
};

} // namespace ohao
