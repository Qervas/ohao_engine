#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>
#include <variant>

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

// BVH Node Data (type-safe with std::variant)
struct InternalNodeData {
    int32_t parent = -1;   // Parent node index
    int32_t childA = -1;   // Left child index
    int32_t childB = -1;   // Right child index
};

struct LeafNodeData {
    int32_t parent = -1;        // Parent node index
    RigidBody* body = nullptr;  // Pointer to rigid body
    uint32_t bodyId = 0;        // Body ID for stability
};

// BVH Node (32-byte aligned for SIMD)
struct alignas(32) BVHNode {
    AABB bounds;           // 24 bytes (2x vec3)

    // Modern C++17 type-safe variant (replaces unsafe union)
    std::variant<InternalNodeData, LeafNodeData> data;

    uint16_t height = 0;   // Tree height (for balancing)
    uint16_t flags = 0;    // Reserved for future use (dirty bit, etc.)

    // Type-safe accessors
    bool isLeaf() const {
        return std::holds_alternative<LeafNodeData>(data);
    }

    bool isInternal() const {
        return std::holds_alternative<InternalNodeData>(data);
    }

    // Get internal node data (throws if not internal)
    InternalNodeData& getInternal() {
        return std::get<InternalNodeData>(data);
    }

    const InternalNodeData& getInternal() const {
        return std::get<InternalNodeData>(data);
    }

    // Get leaf node data (throws if not leaf)
    LeafNodeData& getLeaf() {
        return std::get<LeafNodeData>(data);
    }

    const LeafNodeData& getLeaf() const {
        return std::get<LeafNodeData>(data);
    }

    // Dirty flag accessor (kept for future use)
    bool isDirty() const { return (flags & 0x0001) != 0; }
    void setDirty(bool dirty) {
        if (dirty) flags |= 0x0001;
        else flags &= ~0x0001;
    }

    // Parent accessor (works for both leaf and internal nodes)
    int32_t getParent() const {
        if (isLeaf()) {
            return getLeaf().parent;
        } else {
            return getInternal().parent;
        }
    }

    void setParent(int32_t parentIdx) {
        if (isLeaf()) {
            std::get<LeafNodeData>(data).parent = parentIdx;
        } else {
            std::get<InternalNodeData>(data).parent = parentIdx;
        }
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
    static constexpr int32_t INITIAL_CAPACITY = 4096;  // Large enough to prevent resize (was 256)
};

}}} // namespace ohao::physics::collision
