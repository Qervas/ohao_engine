#pragma once

#include "broad_phase.hpp"
#include "narrow_phase.hpp"
#include "contact_manifold.hpp"
#include "collision_resolver.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include <vector>
#include <memory>

namespace ohao {
namespace physics {
namespace collision {

// Enhanced collision detection system that coordinates broad phase, narrow phase,
// contact generation, and resolution
class CollisionSystem {
public:
    // Configuration structure
    struct Config {
        BroadPhase::Algorithm broadPhaseAlgorithm = BroadPhase::Algorithm::SPATIAL_HASH;
        float spatialHashCellSize = 5.0f;
        bool useSAT = true;
        bool useGJK = false;
        float contactTolerance = 0.001f;
        size_t maxIterations = 10;
        bool enableWarmStarting = true;
        bool enableFriction = true;
        float baumgarte = 0.2f;        // Position correction factor
        float slop = 0.01f;            // Allowed penetration
    };
    
    CollisionSystem();
    explicit CollisionSystem(const Config& config);
    ~CollisionSystem() = default;
    
    // Configuration
    void setConfig(const Config& config);
    const Config& getConfig() const { return m_config; }
    
    // Main collision detection and resolution pipeline
    void detectAndResolveCollisions(std::vector<dynamics::RigidBody*>& bodies, float deltaTime);
    
    // Individual pipeline stages (can be called separately if needed)
    void updateBroadPhase(const std::vector<dynamics::RigidBody*>& bodies);
    void performNarrowPhase();
    void resolveContacts(float deltaTime);
    
    // Collision resolution
    void resolveCollision(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                         const glm::vec3& normal, float penetration,
                         const glm::vec3& contactPoint, float deltaTime);
    
    // Access to sub-systems
    BroadPhase& getBroadPhase() { return *m_broadPhase; }
    const BroadPhase& getBroadPhase() const { return *m_broadPhase; }
    
    NarrowPhaseDetector& getNarrowPhase() { return *m_narrowPhase; }
    const NarrowPhaseDetector& getNarrowPhase() const { return *m_narrowPhase; }
    
    ContactCache& getContactCache() { return *m_contactCache; }
    const ContactCache& getContactCache() const { return *m_contactCache; }
    
    // Statistics and debugging
    struct Stats {
        size_t totalBodies{0};
        size_t potentialPairs{0};
        size_t actualCollisions{0};
        size_t totalContacts{0};
        float broadPhaseTimeMs{0.0f};
        float narrowPhaseTimeMs{0.0f};
        float resolutionTimeMs{0.0f};
        float totalTimeMs{0.0f};
    };
    
    const Stats& getStats() const { return m_stats; }
    void clearStats();
    
    // Debug visualization data
    struct DebugData {
        std::vector<math::AABB> broadPhaseAABBs;
        std::vector<std::pair<glm::vec3, glm::vec3>> contactNormals;
        std::vector<glm::vec3> contactPoints;
        std::vector<std::pair<glm::vec3, glm::vec3>> narrowPhasePairs;
    };
    
    const DebugData& getDebugData() const { return m_debugData; }
    void enableDebugVisualization(bool enable) { m_debugVisualization = enable; }
    bool isDebugVisualizationEnabled() const { return m_debugVisualization; }

private:
    Config m_config;
    
    // Sub-systems
    std::unique_ptr<BroadPhase> m_broadPhase;
    std::unique_ptr<NarrowPhaseDetector> m_narrowPhase;
    std::unique_ptr<ContactCache> m_contactCache;
    std::unique_ptr<CollisionResolver> m_resolver;
    
    // Current frame data
    std::vector<dynamics::RigidBody*> m_bodies;
    std::vector<BodyPair> m_potentialPairs;
    std::vector<ContactManifold*> m_activeManifolds;
    
    // Statistics and debugging
    Stats m_stats;
    DebugData m_debugData;
    bool m_debugVisualization{false};
    
    // Helper functions
    void initializeSubSystems();
    void updateConfiguration();
    void collectDebugData();
    
    // Collision detection methods
    bool detectCollisionBetweenShapes(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                                     CollisionShape* shapeA, CollisionShape* shapeB,
                                     glm::vec3& normal, float& penetration, glm::vec3& contactPoint);
    
    bool detectSphereSphere(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                           CollisionShape* shapeA, CollisionShape* shapeB,
                           glm::vec3& normal, float& penetration, glm::vec3& contactPoint);
                           
    bool detectBoxBox(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                     CollisionShape* shapeA, CollisionShape* shapeB,
                     glm::vec3& normal, float& penetration, glm::vec3& contactPoint);
                     
    bool detectSphereBox(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                        CollisionShape* shapeA, CollisionShape* shapeB,
                        glm::vec3& normal, float& penetration, glm::vec3& contactPoint);
    
    // Contact manifold generation methods
    bool generateContactManifold(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB, ContactManifold* manifold);
    bool generateSphereSphereManifold(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB, ContactManifold* manifold);
    bool generateBoxBoxManifold(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB, ContactManifold* manifold);
    bool generateSphereBoxManifold(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB, ContactManifold* manifold);
    
    // Material and constraint solving methods
    void updateManifoldMaterialProperties(ContactManifold* manifold);
    void solveContactConstraints(float deltaTime);
    void solveManifoldConstraints(ContactManifold* manifold, float deltaTime);
    void solveFrictionConstraints(ContactManifold* manifold, ContactPoint& contact, const glm::vec3& relativeVelocity);
    void applyPositionCorrection(ContactManifold* manifold);
    
    // Body management
    std::unordered_map<dynamics::RigidBody*, uint32_t> m_bodyIds;
    uint32_t m_nextBodyId{0};
    
    dynamics::RigidBody* getBodyFromId(uint32_t id) const;
    uint32_t getOrAssignBodyId(dynamics::RigidBody* body);
};

// Utility class for collision detection queries
class CollisionQueries {
public:
    explicit CollisionQueries(CollisionSystem* system);
    ~CollisionQueries() = default;
    
    // Ray casting
    struct RayHit {
        bool hit{false};
        dynamics::RigidBody* body{nullptr};
        glm::vec3 point{0.0f};
        glm::vec3 normal{0.0f};
        float distance{0.0f};
    };
    
    RayHit raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance = 100.0f);
    std::vector<RayHit> raycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance = 100.0f);
    
    // Shape queries
    std::vector<dynamics::RigidBody*> overlapSphere(const glm::vec3& center, float radius);
    std::vector<dynamics::RigidBody*> overlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation = glm::quat(1,0,0,0));
    std::vector<dynamics::RigidBody*> overlapAABB(const math::AABB& aabb);
    
    // Point queries
    std::vector<dynamics::RigidBody*> pointQuery(const glm::vec3& point);
    dynamics::RigidBody* getClosestBody(const glm::vec3& point, float maxDistance = 10.0f);

private:
    CollisionSystem* m_collisionSystem;
    NarrowPhaseDetector m_queryDetector; // Separate detector for queries
    
    // Helper functions
    bool rayIntersectsAABB(const glm::vec3& rayOrigin, const glm::vec3& rayDir, const math::AABB& aabb, float& tMin, float& tMax);
    RayHit rayIntersectsShape(const glm::vec3& rayOrigin, const glm::vec3& rayDir, 
                             dynamics::RigidBody* body, float maxDistance);
};

} // namespace collision
} // namespace physics
} // namespace ohao