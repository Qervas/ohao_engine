#pragma once

#include <vector>
#include <memory>
#include <glm/glm.hpp>

namespace ohao {

class RigidBody;
class CollisionShape;
class PhysicsComponent;

// Forward declare the enum from viewport toolbar
enum class PhysicsSimulationState;

struct PhysicsSettings {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    float timeStep{1.0f / 60.0f};
    int maxSubSteps{10};
    float fixedTimeStep{1.0f / 240.0f};
    bool enableCCD{true}; // Continuous Collision Detection
};

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    
    // Initialization
    bool initialize(const PhysicsSettings& settings = PhysicsSettings{});
    void cleanup();
    
    // Simulation
    void stepSimulation(float deltaTime);
    void setGravity(const glm::vec3& gravity);
    glm::vec3 getGravity() const;
    
    // Rigid body management
    std::shared_ptr<RigidBody> createRigidBody(PhysicsComponent* component);
    void removeRigidBody(std::shared_ptr<RigidBody> body);
    void removeRigidBody(PhysicsComponent* component);
    
    // Ray casting
    struct RaycastResult {
        bool hit{false};
        glm::vec3 hitPoint{0.0f};
        glm::vec3 hitNormal{0.0f};
        float distance{0.0f};
        RigidBody* body{nullptr};
    };
    
    RaycastResult raycast(const glm::vec3& from, const glm::vec3& to);
    
    // Settings
    void setSettings(const PhysicsSettings& settings);
    const PhysicsSettings& getSettings() const;
    
    // Debug
    void setDebugDrawEnabled(bool enabled);
    bool isDebugDrawEnabled() const;
    void debugDraw(); // TODO: Implement debug drawing
    
    // Statistics
    size_t getRigidBodyCount() const;
    
    // Simulation state
    void setSimulationState(PhysicsSimulationState state);
    PhysicsSimulationState getSimulationState() const;
    
private:
    // Collision detection structures
    struct CollisionInfo {
        std::shared_ptr<RigidBody> bodyA;
        std::shared_ptr<RigidBody> bodyB;
        glm::vec3 contactPoint{0.0f};
        glm::vec3 contactNormal{0.0f};
        float penetrationDepth{0.0f};
        bool hasCollision{false};
    };

    // Collision detection and response
    std::vector<CollisionInfo> detectCollisions();
    CollisionInfo checkCollision(std::shared_ptr<RigidBody> bodyA, std::shared_ptr<RigidBody> bodyB);
    bool checkAABBCollision(const glm::vec3& posA, const glm::vec3& sizeA, 
                           const glm::vec3& posB, const glm::vec3& sizeB,
                           CollisionInfo& info);
    void resolveCollisions(const std::vector<CollisionInfo>& collisions);
    void resolveCollision(const CollisionInfo& collision);
    
    // Helper methods
    glm::vec3 getCollisionShapeSize(std::shared_ptr<CollisionShape> shape);

    void updateRigidBodies();
    void initializeBulletPhysics();
    void cleanupBulletPhysics();
    
    PhysicsSettings m_settings;
    std::vector<std::shared_ptr<RigidBody>> m_rigidBodies;
    bool m_initialized{false};
    bool m_debugDrawEnabled{false};
    PhysicsSimulationState m_simulationState;
    

};

} // namespace ohao