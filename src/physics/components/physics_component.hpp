#pragma once

#include "engine/component/component.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include "physics/collision/shapes/shape_factory.hpp"
#include "physics/world/physics_world.hpp"
#include <memory>

namespace ohao {

// Forward declarations
class Actor;
class TransformComponent;

class PhysicsComponent : public Component {
public:
    using Ptr = std::shared_ptr<PhysicsComponent>;
    
    PhysicsComponent();
    ~PhysicsComponent() override;
    
    // === RIGID BODY TYPE ===
    void setRigidBodyType(physics::dynamics::RigidBodyType type);
    physics::dynamics::RigidBodyType getRigidBodyType() const;
    
    // === PHYSICS PROPERTIES ===
    void setMass(float mass);
    float getMass() const;
    
    void setRestitution(float restitution);  // Bounciness (0-1)
    float getRestitution() const;
    
    void setFriction(float friction);        // Surface friction (0+)
    float getFriction() const;
    
    void setLinearDamping(float damping);    // Air resistance for linear motion (0-1)
    float getLinearDamping() const;
    
    void setAngularDamping(float damping);   // Air resistance for rotation (0-1)
    float getAngularDamping() const;
    
    // === MOVEMENT ===
    void setLinearVelocity(const glm::vec3& velocity);
    glm::vec3 getLinearVelocity() const;
    
    void setAngularVelocity(const glm::vec3& velocity);
    glm::vec3 getAngularVelocity() const;
    
    // === FORCES ===
    void applyForce(const glm::vec3& force, const glm::vec3& relativePos = glm::vec3(0.0f));
    void applyImpulse(const glm::vec3& impulse, const glm::vec3& relativePos = glm::vec3(0.0f));
    void applyTorque(const glm::vec3& torque);
    void clearForces();
    
    // === COLLISION SHAPES ===
    void setCollisionShape(std::shared_ptr<physics::collision::CollisionShape> shape);
    std::shared_ptr<physics::collision::CollisionShape> getCollisionShape() const;
    
    // Convenient shape creators
    void createBoxShape(const glm::vec3& halfExtents);
    void createBoxShape(float width, float height, float depth);
    void createSphereShape(float radius);
    void createCubeShape(float size);
    void createCapsuleShape(float radius, float height);
    void createCylinderShape(float radius, float height);
    void createPlaneShape(const glm::vec3& normal, float distance = 0.0f);
    void createTriangleMeshShape(const std::vector<glm::vec3>& vertices, const std::vector<uint32_t>& indices);
    void createMeshShape(const std::vector<glm::vec3>& vertices, const std::vector<uint32_t>& indices);
    
    // Create collision shape from rendered model
    void createCollisionShapeFromModel(const class Model& model);
    
    // === PHYSICS WORLD INTEGRATION ===
    void setPhysicsWorld(physics::PhysicsWorld* world);
    physics::PhysicsWorld* getPhysicsWorld() const;
    
    // === TRANSFORM SYNC ===
    void setTransformComponent(TransformComponent* transform);
    TransformComponent* getTransformComponent() const;
    void updateRigidBodyFromTransform(); // Sync transform changes to physics body
    
    // === RIGID BODY ACCESS ===
    std::shared_ptr<physics::dynamics::RigidBody> getRigidBody() const { return m_rigidBody; }
    
    // === COMPONENT INTERFACE ===
    void initialize() override;
    void update(float deltaTime) override;
    void destroy() override;
    const char* getTypeName() const override { return "PhysicsComponent"; }
    
    // === COLLISION EVENTS ===
    virtual void onCollisionBegin(PhysicsComponent* other) {}
    virtual void onCollisionEnd(PhysicsComponent* other) {}
    virtual void onCollisionStay(PhysicsComponent* other) {}
    
    // === SERIALIZATION ===
    void serialize(class Serializer& serializer) const override;
    void deserialize(class Deserializer& deserializer) override;
    
    // === SETTINGS ===
    void setGravityEnabled(bool enabled) { m_gravityEnabled = enabled; }
    bool isGravityEnabled() const { return m_gravityEnabled; }
    
    void setAwake(bool awake);
    bool isAwake() const;
    
private:
    // Create rigid body in physics world
    void createRigidBody();
    void destroyRigidBody();
    
    // Sync between physics and transform
    void updateTransformFromRigidBody();
    
    // Component references
    physics::PhysicsWorld* m_physicsWorld{nullptr};
    TransformComponent* m_transformComponent{nullptr};
    
    // Physics state
    std::shared_ptr<physics::dynamics::RigidBody> m_rigidBody;
    std::shared_ptr<physics::collision::CollisionShape> m_collisionShape;
    
    // Settings
    bool m_gravityEnabled{true};
    bool m_initialized{false};
};

} // namespace ohao