#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace ohao {
namespace physics {

// Forward declarations
namespace dynamics {
    class RigidBody;
}

namespace collision {

using dynamics::RigidBody;

// Axis-Aligned Bounding Box
struct AABB {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};

    AABB() = default;
    AABB(const glm::vec3& min, const glm::vec3& max) : min(min), max(max) {}

    // Expand by velocity for swept bounds (CCD)
    AABB expand(const glm::vec3& velocity, float dt) const;

    // Expand by margin for stability
    AABB fatten(float margin) const;

    // Union of two AABBs
    static AABB combine(const AABB& a, const AABB& b);

    // Check overlap
    bool overlaps(const AABB& other) const;

    // Surface area (for SAH cost metric)
    float surfaceArea() const;

    // Center point
    glm::vec3 center() const { return (min + max) * 0.5f; }
};

// BVH Node (32-byte aligned for SIMD)
struct alignas(32) BVHNode {
    AABB bounds;           // 24 bytes (2x vec3)

    union {
        // Internal node
        struct {
            uint32_t childA;    // Left child index
            uint32_t childB;    // Right child index
        };

        // Leaf node
        struct {
            RigidBody* body;    // Pointer to rigid body
            uint32_t bodyId;    // Body ID for stability
        };
    };

    uint16_t height;       // Tree height (for balancing)
    uint16_t flags;        // Leaf bit, dirty bit

    // Flag accessors
    bool isLeaf() const { return (flags & 0x0001) != 0; }
    void setLeaf(bool leaf) {
        if (leaf) flags |= 0x0001;
        else flags &= ~0x0001;
    }

    bool isDirty() const { return (flags & 0x0002) != 0; }
    void setDirty(bool dirty) {
        if (dirty) flags |= 0x0002;
        else flags &= ~0x0002;
    }
};

// Body pair for collision detection
struct BodyPair {
    RigidBody* bodyA;
    RigidBody* bodyB;

    BodyPair(RigidBody* a, RigidBody* b) : bodyA(a), bodyB(b) {
        // Ensure consistent ordering (smaller pointer first)
        if (a > b) std::swap(bodyA, bodyB);
    }

    bool operator==(const BodyPair& other) const {
        return bodyA == other.bodyA && bodyB == other.bodyB;
    }
};

// Dynamic AABB Tree (Incremental BVH)
class DynamicAABBTree {
public:
    DynamicAABBTree();
    ~DynamicAABBTree();

    // Insert/Remove bodies
    int32_t insertBody(RigidBody* body, const AABB& aabb);
    void removeBody(int32_t proxyId);

    // Update body AABB (returns true if moved significantly)
    bool updateBody(int32_t proxyId, const AABB& aabb, const glm::vec3& displacement);

    // Query overlapping pairs
    void queryOverlaps(std::vector<BodyPair>& pairs);

    // Raycast query
    struct RaycastHit {
        RigidBody* body;
        float t;  // Ray parameter
        glm::vec3 point;
        glm::vec3 normal;
    };
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, RaycastHit& hit);

    // AABB query
    void queryAABB(const AABB& aabb, std::vector<RigidBody*>& results);

    // Get body from proxy ID
    RigidBody* getBody(int32_t proxyId) const;

    // Stats
    int32_t getNodeCount() const { return m_nodeCount; }
    int32_t getHeight() const;

    // Clear tree
    void clear();

private:
    // Node management
    int32_t allocateNode();
    void freeNode(int32_t nodeId);

    // Tree operations
    void insertLeaf(int32_t leaf);
    void removeLeaf(int32_t leaf);

    // Balancing (AVL-style rotations)
    int32_t balance(int32_t nodeId);
    int32_t rotateLeft(int32_t nodeId);
    int32_t rotateRight(int32_t nodeId);

    // Helper methods
    void updateNodeBounds(int32_t nodeId);
    int32_t computeHeight(int32_t nodeId) const;
    void validateStructure(int32_t nodeId) const;

    // Recursive queries
    void queryNode(int32_t nodeId, const AABB& aabb, std::vector<RigidBody*>& results);
    void collectPairs(int32_t nodeA, int32_t nodeB, std::vector<BodyPair>& pairs);

    // Data
    std::vector<BVHNode> m_nodes;
    int32_t m_root;             // Root node index (-1 if empty)
    int32_t m_nodeCount;        // Number of nodes in use
    int32_t m_nodeCapacity;     // Total allocated nodes
    int32_t m_freeList;         // Head of free node list

    // Configuration
    static constexpr float AABB_MARGIN = 0.1f;       // Fatten AABBs by this margin
    static constexpr float DISPLACEMENT_MULTIPLIER = 2.0f;  // Predict future position
    static constexpr int32_t NULL_NODE = -1;
    static constexpr int32_t INITIAL_CAPACITY = 256;
};

}}} // namespace ohao::physics::collision
