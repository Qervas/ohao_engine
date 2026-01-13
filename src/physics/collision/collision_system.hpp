#pragma once
#include <vector>
#include <memory>
#include <unordered_map>

namespace ohao {
namespace physics {

// Forward declarations
namespace dynamics {
    class RigidBody;
}

namespace collision {
using dynamics::RigidBody;

// Forward declarations
class DynamicAABBTree;
class GJKSolver;
class EPASolver;
class ContactManifold;
struct BodyPair;

// Main collision detection and resolution system
class CollisionSystem {
public:
    CollisionSystem();
    ~CollisionSystem();

    // Update bodies in broad phase
    void updateBroadPhase(const std::vector<RigidBody*>& bodies);

    // Perform narrow phase collision detection
    std::vector<ContactManifold*> performNarrowPhase();

    // Clear all data
    void clear();

    // Stats
    int getBroadPhasePairCount() const;
    int getManifoldCount() const { return static_cast<int>(m_manifolds.size()); }

private:
    // Narrow phase detection per shape pair
    bool detectCollision(RigidBody* bodyA, RigidBody* bodyB, ContactManifold* manifold);

    // Get or create manifold for body pair
    ContactManifold* getOrCreateManifold(RigidBody* bodyA, RigidBody* bodyB);

    // Compute hash for body pair
    uint64_t computePairHash(RigidBody* bodyA, RigidBody* bodyB) const;

    // Broad phase
    std::unique_ptr<DynamicAABBTree> m_broadPhase;

    // Narrow phase solvers
    std::unique_ptr<GJKSolver> m_gjkSolver;
    std::unique_ptr<EPASolver> m_epaSolver;

    // Contact manifolds
    std::vector<std::unique_ptr<ContactManifold>> m_manifolds;
    std::unordered_map<uint64_t, ContactManifold*> m_manifoldMap;  // For persistence

    // Proxy IDs for bodies
    std::unordered_map<RigidBody*, int32_t> m_bodyProxies;
};

}}} // namespace ohao::physics::collision
