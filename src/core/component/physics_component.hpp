#pragma once

#include "component.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace ohao {

// Forward declarations
class Actor;
class CollisionShape;

/* Physics system temporarily disabled
class PhysicsComponent : public Component {
public:
    using Ptr = std::shared_ptr<PhysicsComponent>;
    
    PhysicsComponent();
    ~PhysicsComponent() override;
    
    // Physics properties
    void setMass(float mass);
    float getMass() const;
    
    void setLinearVelocity(const glm::vec3& velocity);
    glm::vec3 getLinearVelocity() const;
    
    void setAngularVelocity(const glm::vec3& velocity);
    glm::vec3 getAngularVelocity() const;
    
    void applyForce(const glm::vec3& force);
    void applyImpulse(const glm::vec3& impulse);
    void applyTorque(const glm::vec3& torque);
    
    void setStatic(bool isStatic);
    bool isStatic() const;
    
    void setGravityEnabled(bool enabled);
    bool isGravityEnabled() const;
    
    void setFriction(float friction);
    float getFriction() const;
    
    void setRestitution(float restitution);
    float getRestitution() const;
    
    // Collision shape
    void setCollisionShape(std::shared_ptr<CollisionShape> shape);
    std::shared_ptr<CollisionShape> getCollisionShape() const;
    
    // Automatic shape generation
    void createBoxShape(const glm::vec3& halfExtents);
    void createSphereShape(float radius);
    void createCapsuleShape(float radius, float height);
    void createConvexHullShape(const std::vector<glm::vec3>& points);
    
    // Component overrides
    void initialize() override;
    void update(float deltaTime) override;
    void destroy() override;
    const char* getTypeName() const override;
    
    // Collision callbacks
    void onCollisionBegin(PhysicsComponent* other);
    void onCollisionEnd(PhysicsComponent* other);
    
    // Serialization
    void serialize(class Serializer& serializer) const override;
    void deserialize(class Deserializer& deserializer) override;
    
private:
    // Physics state
    float mass;
    glm::vec3 linearVelocity;
    glm::vec3 angularVelocity;
    glm::vec3 force;
    glm::vec3 torque;
    
    // Physics properties
    bool isStaticBody;
    bool gravityEnabled;
    float friction;
    float restitution;
    
    // Collision detection
    std::shared_ptr<CollisionShape> collisionShape;
    
    // Physics integration
    void integrateForces(float deltaTime);
    void integrateVelocity(float deltaTime);
    void updateTransform();
};
*/

// Temporary stub for PhysicsComponent while system is disabled
class PhysicsComponent : public Component {
public:
    using Ptr = std::shared_ptr<PhysicsComponent>;
    
    PhysicsComponent() {}
    ~PhysicsComponent() override {}
    
    // Component overrides
    void initialize() override {}
    void update(float deltaTime) override {}
    void destroy() override {}
    const char* getTypeName() const override { return "PhysicsComponent"; }
    
    // Serialization
    void serialize(class Serializer& serializer) const override {}
    void deserialize(class Deserializer& deserializer) override {}
};

} // namespace ohao