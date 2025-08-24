#pragma once

#include "contact_manifold.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include "shapes/collision_shape.hpp"
#include <functional>
#include <unordered_map>

namespace ohao {
namespace physics {
namespace collision {

// Type alias for collision detection functions
using CollisionFunction = std::function<ContactManifold(
    const CollisionShape* shapeA, const glm::vec3& posA, const glm::quat& rotA,
    const CollisionShape* shapeB, const glm::vec3& posB, const glm::quat& rotB
)>;

// Enhanced narrow phase collision detector with multiple algorithms
class NarrowPhaseDetector {
public:
    NarrowPhaseDetector();
    ~NarrowPhaseDetector() = default;
    
    // Main collision detection interface
    ContactManifold detectCollision(
        dynamics::RigidBody* bodyA, 
        dynamics::RigidBody* bodyB
    );
    
    // Direct shape collision detection
    ContactManifold detectShapeCollision(
        const CollisionShape* shapeA, const glm::vec3& posA, const glm::quat& rotA,
        const CollisionShape* shapeB, const glm::vec3& posB, const glm::quat& rotB
    );
    
    // Register custom collision functions
    void registerCollisionFunction(ShapeType typeA, ShapeType typeB, CollisionFunction func);
    
    // Enable/disable specific collision algorithms
    void setUseSAT(bool enable) { m_useSAT = enable; }
    void setUseGJK(bool enable) { m_useGJK = enable; }
    void setContactTolerance(float tolerance) { m_contactTolerance = tolerance; }
    
    // Configuration
    bool isUsingSAT() const { return m_useSAT; }
    bool isUsingGJK() const { return m_useGJK; }
    float getContactTolerance() const { return m_contactTolerance; }

private:
    std::unordered_map<uint64_t, CollisionFunction> m_collisionFunctions;
    bool m_useSAT{true};
    bool m_useGJK{false};
    float m_contactTolerance{0.001f};
    
    // Shape-specific collision detection functions
    ContactManifold detectBoxVsBox(
        const class BoxShape* boxA, const glm::vec3& posA, const glm::quat& rotA,
        const class BoxShape* boxB, const glm::vec3& posB, const glm::quat& rotB
    );
    
    ContactManifold detectSphereVsSphere(
        const class SphereShape* sphereA, const glm::vec3& posA,
        const class SphereShape* sphereB, const glm::vec3& posB
    );
    
    ContactManifold detectBoxVsSphere(
        const class BoxShape* box, const glm::vec3& boxPos, const glm::quat& boxRot,
        const class SphereShape* sphere, const glm::vec3& spherePos
    );
    
    ContactManifold detectSphereVsCapsule(
        const class SphereShape* sphere, const glm::vec3& spherePos,
        const class CapsuleShape* capsule, const glm::vec3& capsulePos, const glm::quat& capsuleRot
    );
    
    ContactManifold detectSphereVsPlane(
        const class SphereShape* sphere, const glm::vec3& spherePos,
        const class PlaneShape* plane, const glm::vec3& planePos, const glm::quat& planeRot
    );
    
    ContactManifold detectBoxVsPlane(
        const class BoxShape* box, const glm::vec3& boxPos, const glm::quat& boxRot,
        const class PlaneShape* plane, const glm::vec3& planePos, const glm::quat& planeRot
    );
    
    ContactManifold detectCapsuleVsCapsule(
        const class CapsuleShape* capsuleA, const glm::vec3& posA, const glm::quat& rotA,
        const class CapsuleShape* capsuleB, const glm::vec3& posB, const glm::quat& rotB
    );
    
    // SAT (Separating Axis Theorem) implementation for oriented boxes
    ContactManifold detectBoxVsBoxSAT(
        const class BoxShape* boxA, const glm::vec3& posA, const glm::quat& rotA,
        const class BoxShape* boxB, const glm::vec3& posB, const glm::quat& rotB
    );
    
    // Helper functions
    void setupCollisionFunctions();
    uint64_t makeShapeTypeKey(ShapeType typeA, ShapeType typeB) const;
    
    // SAT helper functions
    struct SATResult {
        bool separated{true};
        float penetration{0.0f};
        glm::vec3 normal{0.0f};
        glm::vec3 contactPoint{0.0f};
    };
    
    SATResult testSeparatingAxis(
        const glm::vec3& axis,
        const std::vector<glm::vec3>& verticesA,
        const std::vector<glm::vec3>& verticesB
    );
    
    std::vector<glm::vec3> getBoxVertices(
        const class BoxShape* box, const glm::vec3& position, const glm::quat& rotation
    );
    
    // Contact point generation
    std::vector<ContactPoint> generateBoxBoxContacts(
        const class BoxShape* boxA, const glm::vec3& posA, const glm::quat& rotA,
        const class BoxShape* boxB, const glm::vec3& posB, const glm::quat& rotB,
        const glm::vec3& normal, float penetration
    );
    
    // Material property calculation
    void calculateMaterialProperties(ContactManifold& manifold, 
                                   dynamics::RigidBody* bodyA, 
                                   dynamics::RigidBody* bodyB);
};

// Utility functions for collision detection
namespace CollisionUtils {
    // Find closest point on line segment to a point
    glm::vec3 closestPointOnLineSegment(const glm::vec3& point, 
                                       const glm::vec3& lineStart, 
                                       const glm::vec3& lineEnd);
    
    // Find closest points between two line segments
    struct ClosestPointsResult {
        glm::vec3 pointA{0.0f};
        glm::vec3 pointB{0.0f};
        float distance{0.0f};
        float paramA{0.0f}; // Parameter along segment A [0,1]
        float paramB{0.0f}; // Parameter along segment B [0,1]
    };
    
    ClosestPointsResult closestPointsBetweenSegments(
        const glm::vec3& startA, const glm::vec3& endA,
        const glm::vec3& startB, const glm::vec3& endB
    );
    
    // Project point onto plane
    glm::vec3 projectPointOntoPlane(const glm::vec3& point, 
                                   const glm::vec3& planeNormal, 
                                   const glm::vec3& planePoint);
    
    // Clamp point to box bounds in local space
    glm::vec3 clampPointToBox(const glm::vec3& point, const glm::vec3& halfExtents);
    
    // Transform point by position and rotation
    glm::vec3 transformPoint(const glm::vec3& point, 
                            const glm::vec3& position, 
                            const glm::quat& rotation);
    
    // Transform vector by rotation only
    glm::vec3 transformVector(const glm::vec3& vector, const glm::quat& rotation);
    
    // Get support point in direction for a shape (for GJK/EPA)
    glm::vec3 getSupportPoint(const CollisionShape* shape,
                             const glm::vec3& position,
                             const glm::quat& rotation,
                             const glm::vec3& direction);
    
    // Barycentric coordinates calculation
    glm::vec3 barycentricCoordinates(const glm::vec3& point,
                                    const glm::vec3& a,
                                    const glm::vec3& b,
                                    const glm::vec3& c);
}

} // namespace collision
} // namespace physics
} // namespace ohao