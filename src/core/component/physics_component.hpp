#pragma once

#include "component.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>
#include "../physics/rigid_body.hpp"
#include "../physics/collision_shape.hpp"

namespace ohao {

// Forward declarations
class Actor;
class CollisionShape;
class PhysicsWorld;
class RigidBody;

class PhysicsComponent : public Component {
public:
    using Ptr = std::shared_ptr<PhysicsComponent>;
    
    enum class BodyType {
        STATIC,
        DYNAMIC,
        KINEMATIC
    };
    
    PhysicsComponent(Actor* owner = nullptr);
    ~PhysicsComponent() override;
    
    // Body type
    void setStatic(bool isStatic);
    void setDynamic(bool isDynamic);
    void setKinematic(bool isKinematic);
    void setBodyType(BodyType type);
    BodyType getBodyType() const;
    bool isStatic() const { return bodyType == BodyType::STATIC; }
    bool isDynamic() const { return bodyType == BodyType::DYNAMIC; }
    bool isKinematic() const { return bodyType == BodyType::KINEMATIC; }
    
    // Physical properties
    void setMass(float mass);
    float getMass() const;
    
    void setFriction(float friction);
    float getFriction() const;
    
    void setRestitution(float restitution);
    float getRestitution() const;
    
    void setLinearDamping(float damping);
    float getLinearDamping() const;
    
    void setAngularDamping(float damping);
    float getAngularDamping() const;
    
    // Collision detection
    void createBoxShape(const glm::vec3& size);
    void createSphereShape(float radius);
    void createCapsuleShape(float radius, float height);
    void createConvexHullShape(const std::vector<glm::vec3>& vertices);
    void createMeshShape(const std::vector<glm::vec3>& vertices, const std::vector<unsigned int>& indices);
    
    CollisionShape* getCollisionShape() const;
    
    // Motion states
    void setLinearVelocity(const glm::vec3& velocity);
    glm::vec3 getLinearVelocity() const;
    
    void setAngularVelocity(const glm::vec3& velocity);
    glm::vec3 getAngularVelocity() const;
    
    void applyForce(const glm::vec3& force, const glm::vec3& relativePosition = glm::vec3(0.0f));
    void applyImpulse(const glm::vec3& impulse, const glm::vec3& relativePosition = glm::vec3(0.0f));
    void applyTorque(const glm::vec3& torque);
    void applyTorqueImpulse(const glm::vec3& torque);
    
    // Constraint properties
    void setGravityEnabled(bool enabled);
    bool isGravityEnabled() const;
    
    void setCollisionEnabled(bool enabled);
    bool isCollisionEnabled() const;
    
    // Component interface
    void initialize() override;
    void update(float deltaTime) override;
    void destroy() override;
    
    // Type information
    const char* getTypeName() const override;
    static const char* staticTypeName() { return "PhysicsComponent"; }
    
    // Serialization
    nlohmann::json serialize() const override;
    void deserialize(const nlohmann::json& data) override;
    
private:
    BodyType bodyType;
    float mass;
    float friction;
    float restitution;
    float linearDamping;
    float angularDamping;
    bool gravityEnabled;
    bool collisionEnabled;
    
    std::unique_ptr<CollisionShape> collisionShape;
    std::unique_ptr<RigidBody> rigidBody;
    
    // Internal helpers
    void updateRigidBody();
    void syncTransformToPhysics();
    void syncPhysicsToTransform();
};

} // namespace ohao 