#pragma once

#include "physics/utils/physics_math.hpp"
#include "contact_manifold.hpp"
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace ohao {
namespace physics {
namespace dynamics { class RigidBody; }

namespace collision {

// Pair of body IDs for collision detection
struct BodyPair {
    uint32_t bodyA;
    uint32_t bodyB;
    
    BodyPair(uint32_t a, uint32_t b) : bodyA(std::min(a, b)), bodyB(std::max(a, b)) {}
    
    bool operator==(const BodyPair& other) const {
        return bodyA == other.bodyA && bodyB == other.bodyB;
    }
    
    struct Hash {
        size_t operator()(const BodyPair& pair) const {
            return std::hash<uint64_t>()(static_cast<uint64_t>(pair.bodyA) << 32 | pair.bodyB);
        }
    };
};

// Spatial hash grid cell
struct SpatialHashCell {
    std::vector<dynamics::RigidBody*> bodies;
    
    void clear() {
        bodies.clear();
    }
    
    void addBody(dynamics::RigidBody* body) {
        bodies.push_back(body);
    }
};

// Spatial hash grid for broad phase collision detection
class SpatialHashGrid {
public:
    SpatialHashGrid(float cellSize = 5.0f);
    ~SpatialHashGrid() = default;
    
    // Clear all bodies from the grid
    void clear();
    
    // Insert a body into the grid based on its AABB
    void insertBody(dynamics::RigidBody* body);
    
    // Get all potential collision pairs
    std::vector<BodyPair> getPotentialPairs() const;
    
    // Update grid parameters
    void setCellSize(float cellSize) { m_cellSize = cellSize; }
    float getCellSize() const { return m_cellSize; }
    
    // Debug info
    size_t getActiveCellCount() const { return m_cells.size(); }
    size_t getTotalBodiesInGrid() const;

private:
    float m_cellSize;
    std::unordered_map<int64_t, SpatialHashCell> m_cells;
    
    // Hash a 3D position to a cell key
    int64_t hashPosition(const glm::vec3& position) const;
    
    // Get all cell keys that an AABB overlaps
    std::vector<int64_t> getCellKeysForAABB(const math::AABB& aabb) const;
    
    // Convert 3D grid coordinates to cell key
    int64_t gridCoordsToKey(int32_t x, int32_t y, int32_t z) const;
    
    // Get grid coordinates from world position
    glm::ivec3 getGridCoords(const glm::vec3& position) const;
};

// Broad phase collision detection system
class BroadPhase {
public:
    enum class Algorithm {
        AABB_SIMPLE,     // Simple AABB vs AABB testing
        SPATIAL_HASH,    // Spatial hash grid
        DYNAMIC_AABB     // Dynamic AABB tree (not yet implemented)
    };
    
    BroadPhase(Algorithm algorithm = Algorithm::SPATIAL_HASH);
    ~BroadPhase() = default;
    
    // Set the broad phase algorithm
    void setAlgorithm(Algorithm algorithm);
    Algorithm getAlgorithm() const { return m_algorithm; }
    
    // Update broad phase with current body positions
    void update(const std::vector<dynamics::RigidBody*>& bodies);
    
    // Quick AABB overlap test for two bodies
    static bool testAABBOverlap(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB);
    
    // Configuration
    void setSpatialHashCellSize(float cellSize);
    
    // Get all potential collision pairs
    std::vector<BodyPair> getPotentialPairs();
    
    // Statistics
    struct Stats {
        size_t totalBodies{0};
        size_t potentialPairs{0};
        size_t activeCells{0};
        float updateTimeMs{0.0f};
    };
    
    const Stats& getStats() const { return m_stats; }

private:
    Algorithm m_algorithm;
    std::unique_ptr<SpatialHashGrid> m_spatialHash;
    std::vector<dynamics::RigidBody*> m_bodies;
    mutable std::vector<BodyPair> m_potentialPairs;
    Stats m_stats;
    
    // Algorithm implementations
    std::vector<BodyPair> getSimplePairs();
    std::vector<BodyPair> getSpatialHashPairs();
    
    // Helper to assign unique IDs to bodies
    std::unordered_map<dynamics::RigidBody*, uint32_t> m_bodyToId;
    uint32_t m_nextBodyId{0};
    
    uint32_t getBodyId(dynamics::RigidBody* body);
};

// Contact cache for persistent contact manifolds
class ContactCache {
public:
    ContactCache() = default;
    ~ContactCache() = default;
    
    // Get or create contact manifold for body pair
    ContactManifold* getOrCreateManifold(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB);
    
    // Remove manifold for body pair
    void removeManifold(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB);
    
    // Remove all manifolds involving a specific body
    void removeManifolds(dynamics::RigidBody* body);
    
    // Update all manifolds (age them, remove stale ones)
    void update(float deltaTime);
    
    // Get all active manifolds
    std::vector<ContactManifold*> getActiveManifolds();
    
    // Clear all manifolds
    void clear();
    
    // Statistics
    size_t getManifoldCount() const { return m_manifolds.size(); }

private:
    std::unordered_map<BodyPair, std::unique_ptr<ContactManifold>, BodyPair::Hash> m_manifolds;
    
    static constexpr float MAX_MANIFOLD_LIFETIME = 1.0f; // Remove manifolds after 1 second of no contact
    
    BodyPair makeBodyPair(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB);
};

} // namespace collision
} // namespace physics
} // namespace ohao