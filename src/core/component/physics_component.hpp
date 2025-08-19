#pragma once

#include "component.hpp"
#include "../physics/rigid_body.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace ohao {

// Forward declarations
class Actor;
class CollisionShape;
class RigidBody;
class PhysicsWorld;
class TransformComponent;

class PhysicsComponent : public Component {
public:
    using Ptr = std::shared_ptr<PhysicsComponent>;
    
    PhysicsComponent();
    ~PhysicsComponent() override;
    
    // Physics properties
    void setMass(float mass);
    float getMass() const;
    
    void setRigidBodyType(RigidBodyType type);
    RigidBodyType getRigidBodyType() const;
    
    void setLinearVelocity(const glm::vec3& velocity);
    glm::vec3 getLinearVelocity() const;
    
    void setAngularVelocity(const glm::vec3& velocity);
    glm::vec3 getAngularVelocity() const;
    
    void applyForce(const glm::vec3& force, const glm::vec3& relativePos = glm::vec3(0.0f));
    void applyImpulse(const glm::vec3& impulse, const glm::vec3& relativePos = glm::vec3(0.0f));
    void applyTorque(const glm::vec3& torque);
    void clearForces();
    
    void setGravityEnabled(bool enabled);
    bool isGravityEnabled() const;
    
    void setFriction(float friction);
    float getFriction() const;
    
    void setRestitution(float restitution);
    float getRestitution() const;
    
    void setLinearDamping(float damping);
    float getLinearDamping() const;
    
    void setAngularDamping(float damping);
    float getAngularDamping() const;
    
    // Collision shape
    void setCollisionShape(std::shared_ptr<CollisionShape> shape);
    std::shared_ptr<CollisionShape> getCollisionShape() const;
    
    // Automatic shape generation helpers
    void createBoxShape(const glm::vec3& halfExtents);
    void createSphereShape(float radius);
    void createCapsuleShape(float radius, float height);
    void createMeshShape(const std::vector<glm::vec3>& vertices, const std::vector<uint32_t>& indices);
    
    // Transform integration
    void setTransformComponent(TransformComponent* transform);
    TransformComponent* getTransformComponent() const;
    
    // Physics world integration
    void setPhysicsWorld(PhysicsWorld* world);
    PhysicsWorld* getPhysicsWorld() const;
    
    // Rigid body access
    std::shared_ptr<RigidBody> getRigidBody() const;
    
    // Component overrides
    void initialize() override;
    void update(float deltaTime) override;
    void destroy() override;
    const char* getTypeName() const override;
    
    // Collision callbacks - TODO: Implement collision system
    void onCollisionBegin(PhysicsComponent* other);
    void onCollisionEnd(PhysicsComponent* other);
    
    // Serialization
    void serialize(class Serializer& serializer) const override;
    void deserialize(class Deserializer& deserializer) override;
    
private:
    // Rigid body (handles actual physics simulation)
    std::shared_ptr<RigidBody> m_rigidBody;
    
    // Component references
    TransformComponent* m_transformComponent{nullptr};
    PhysicsWorld* m_physicsWorld{nullptr};
    
    // Physics properties cache
    std::shared_ptr<CollisionShape> m_collisionShape;
    bool m_gravityEnabled{true};
    
    // Cached physics properties (used when rigid body doesn't exist yet)
    float m_mass{1.0f};
    RigidBodyType m_rigidBodyType{RigidBodyType::DYNAMIC};
    float m_friction{0.5f};
    float m_restitution{0.0f};
    float m_linearDamping{0.0f};
    float m_angularDamping{0.0f};
    
    // Helper methods
    void createRigidBody();
    void updateRigidBodyFromTransform();
    void updateTransformFromRigidBody();
};

} // namespace ohao