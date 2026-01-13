#include "dynamic_bvh.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include <algorithm>
#include <cassert>

namespace ohao {
namespace physics {
namespace collision {

// AABB implementation
AABB AABB::expand(const glm::vec3& velocity, float dt) const {
    glm::vec3 displacement = velocity * dt;
    AABB result = *this;

    // Expand in direction of motion
    if (displacement.x < 0.0f) result.min.x += displacement.x;
    else result.max.x += displacement.x;

    if (displacement.y < 0.0f) result.min.y += displacement.y;
    else result.max.y += displacement.y;

    if (displacement.z < 0.0f) result.min.z += displacement.z;
    else result.max.z += displacement.z;

    return result;
}

AABB AABB::fatten(float margin) const {
    glm::vec3 m(margin);
    return AABB(min - m, max + m);
}

AABB AABB::combine(const AABB& a, const AABB& b) {
    return AABB(
        glm::min(a.min, b.min),
        glm::max(a.max, b.max)
    );
}

bool AABB::overlaps(const AABB& other) const {
    return (min.x <= other.max.x && max.x >= other.min.x) &&
           (min.y <= other.max.y && max.y >= other.min.y) &&
           (min.z <= other.max.z && max.z >= other.min.z);
}

float AABB::surfaceArea() const {
    glm::vec3 d = max - min;
    return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
}

// DynamicAABBTree implementation
DynamicAABBTree::DynamicAABBTree()
    : m_root(NULL_NODE)
    , m_nodeCount(0)
    , m_nodeCapacity(INITIAL_CAPACITY)
    , m_freeList(0)
{
    m_nodes.resize(m_nodeCapacity);

    // Build free list
    for (int32_t i = 0; i < m_nodeCapacity - 1; ++i) {
        m_nodes[i].childA = i + 1;
        m_nodes[i].height = -1;
    }
    m_nodes[m_nodeCapacity - 1].childA = NULL_NODE;
    m_nodes[m_nodeCapacity - 1].height = -1;
}

DynamicAABBTree::~DynamicAABBTree() {
    // Nodes automatically freed by vector destructor
}

int32_t DynamicAABBTree::allocateNode() {
    // Expand pool if needed
    if (m_freeList == NULL_NODE) {
        assert(m_nodeCount == m_nodeCapacity);

        // Double capacity
        m_nodeCapacity *= 2;
        m_nodes.resize(m_nodeCapacity);

        // Build free list for new nodes
        for (int32_t i = m_nodeCount; i < m_nodeCapacity - 1; ++i) {
            m_nodes[i].childA = i + 1;
            m_nodes[i].height = -1;
        }
        m_nodes[m_nodeCapacity - 1].childA = NULL_NODE;
        m_nodes[m_nodeCapacity - 1].height = -1;
        m_freeList = m_nodeCount;
    }

    // Pop node from free list
    int32_t nodeId = m_freeList;
    m_freeList = m_nodes[nodeId].childA;
    m_nodes[nodeId].childA = NULL_NODE;
    m_nodes[nodeId].childB = NULL_NODE;
    m_nodes[nodeId].height = 0;
    m_nodes[nodeId].flags = 0;
    ++m_nodeCount;

    return nodeId;
}

void DynamicAABBTree::freeNode(int32_t nodeId) {
    assert(0 <= nodeId && nodeId < m_nodeCapacity);
    assert(m_nodeCount > 0);

    m_nodes[nodeId].childA = m_freeList;
    m_nodes[nodeId].height = -1;
    m_freeList = nodeId;
    --m_nodeCount;
}

int32_t DynamicAABBTree::insertBody(RigidBody* body, const AABB& aabb) {
    int32_t proxyId = allocateNode();

    // Fatten AABB for stability
    m_nodes[proxyId].bounds = aabb.fatten(AABB_MARGIN);
    m_nodes[proxyId].body = body;
    m_nodes[proxyId].bodyId = reinterpret_cast<uintptr_t>(body) & 0xFFFFFFFF;
    m_nodes[proxyId].setLeaf(true);
    m_nodes[proxyId].height = 0;

    insertLeaf(proxyId);

    return proxyId;
}

void DynamicAABBTree::removeBody(int32_t proxyId) {
    assert(0 <= proxyId && proxyId < m_nodeCapacity);
    assert(m_nodes[proxyId].isLeaf());

    removeLeaf(proxyId);
    freeNode(proxyId);
}

bool DynamicAABBTree::updateBody(int32_t proxyId, const AABB& aabb, const glm::vec3& displacement) {
    assert(0 <= proxyId && proxyId < m_nodeCapacity);
    assert(m_nodes[proxyId].isLeaf());

    // Check if AABB still fits in fattened bounds
    if (m_nodes[proxyId].bounds.overlaps(aabb)) {
        return false;  // No update needed
    }

    // Remove and reinsert with new AABB
    removeLeaf(proxyId);

    // Predict future position with displacement
    AABB fattenedAABB = aabb.fatten(AABB_MARGIN);

    glm::vec3 d = displacement * DISPLACEMENT_MULTIPLIER;
    if (d.x < 0.0f) fattenedAABB.min.x += d.x;
    else fattenedAABB.max.x += d.x;
    if (d.y < 0.0f) fattenedAABB.min.y += d.y;
    else fattenedAABB.max.y += d.y;
    if (d.z < 0.0f) fattenedAABB.min.z += d.z;
    else fattenedAABB.max.z += d.z;

    m_nodes[proxyId].bounds = fattenedAABB;
    insertLeaf(proxyId);

    return true;
}

void DynamicAABBTree::insertLeaf(int32_t leaf) {
    if (m_root == NULL_NODE) {
        m_root = leaf;
        m_nodes[m_root].childA = NULL_NODE;
        return;
    }

    // Find best sibling using SAH (Surface Area Heuristic)
    AABB leafAABB = m_nodes[leaf].bounds;
    int32_t sibling = m_root;

    while (!m_nodes[sibling].isLeaf()) {
        int32_t childA = m_nodes[sibling].childA;
        int32_t childB = m_nodes[sibling].childB;

        float area = m_nodes[sibling].bounds.surfaceArea();

        AABB combinedAABB = AABB::combine(m_nodes[sibling].bounds, leafAABB);
        float combinedArea = combinedAABB.surfaceArea();

        // Cost of creating a new parent for this node and the leaf
        float cost = 2.0f * combinedArea;

        // Minimum cost of pushing the leaf further down
        float inheritanceCost = 2.0f * (combinedArea - area);

        // Cost of descending into childA
        float costA;
        if (m_nodes[childA].isLeaf()) {
            AABB aabb = AABB::combine(leafAABB, m_nodes[childA].bounds);
            costA = aabb.surfaceArea() + inheritanceCost;
        } else {
            AABB aabb = AABB::combine(leafAABB, m_nodes[childA].bounds);
            float oldArea = m_nodes[childA].bounds.surfaceArea();
            float newArea = aabb.surfaceArea();
            costA = (newArea - oldArea) + inheritanceCost;
        }

        // Cost of descending into childB
        float costB;
        if (m_nodes[childB].isLeaf()) {
            AABB aabb = AABB::combine(leafAABB, m_nodes[childB].bounds);
            costB = aabb.surfaceArea() + inheritanceCost;
        } else {
            AABB aabb = AABB::combine(leafAABB, m_nodes[childB].bounds);
            float oldArea = m_nodes[childB].bounds.surfaceArea();
            float newArea = aabb.surfaceArea();
            costB = (newArea - oldArea) + inheritanceCost;
        }

        // Descend according to minimum cost
        if (cost < costA && cost < costB) {
            break;
        }

        sibling = (costA < costB) ? childA : childB;
    }

    // Create new parent
    int32_t oldParent = m_nodes[sibling].childA;  // Note: reusing childA as parent pointer
    int32_t newParent = allocateNode();
    m_nodes[newParent].childA = oldParent;
    m_nodes[newParent].body = nullptr;
    m_nodes[newParent].bounds = AABB::combine(leafAABB, m_nodes[sibling].bounds);
    m_nodes[newParent].setLeaf(false);
    m_nodes[newParent].height = m_nodes[sibling].height + 1;

    if (oldParent != NULL_NODE) {
        // Sibling was not the root
        if (m_nodes[oldParent].childA == sibling) {
            m_nodes[oldParent].childA = newParent;
        } else {
            m_nodes[oldParent].childB = newParent;
        }

        m_nodes[newParent].childA = sibling;
        m_nodes[newParent].childB = leaf;
        m_nodes[sibling].childA = newParent;
        m_nodes[leaf].childA = newParent;
    } else {
        // Sibling was the root
        m_nodes[newParent].childA = sibling;
        m_nodes[newParent].childB = leaf;
        m_nodes[sibling].childA = newParent;
        m_nodes[leaf].childA = newParent;
        m_root = newParent;
    }

    // Walk back up the tree fixing heights and balancing
    int32_t index = m_nodes[leaf].childA;
    while (index != NULL_NODE) {
        index = balance(index);

        int32_t childA = m_nodes[index].childA;
        int32_t childB = m_nodes[index].childB;

        assert(childA != NULL_NODE);
        assert(childB != NULL_NODE);

        m_nodes[index].height = 1 + std::max(m_nodes[childA].height, m_nodes[childB].height);
        m_nodes[index].bounds = AABB::combine(m_nodes[childA].bounds, m_nodes[childB].bounds);

        index = m_nodes[index].childA;  // Move to parent
    }
}

void DynamicAABBTree::removeLeaf(int32_t leaf) {
    if (leaf == m_root) {
        m_root = NULL_NODE;
        return;
    }

    int32_t parent = m_nodes[leaf].childA;
    int32_t grandParent = m_nodes[parent].childA;
    int32_t sibling = (m_nodes[parent].childA == leaf) ? m_nodes[parent].childB : m_nodes[parent].childA;

    if (grandParent != NULL_NODE) {
        // Destroy parent and connect sibling to grandParent
        if (m_nodes[grandParent].childA == parent) {
            m_nodes[grandParent].childA = sibling;
        } else {
            m_nodes[grandParent].childB = sibling;
        }
        m_nodes[sibling].childA = grandParent;
        freeNode(parent);

        // Adjust ancestor bounds
        int32_t index = grandParent;
        while (index != NULL_NODE) {
            index = balance(index);

            int32_t childA = m_nodes[index].childA;
            int32_t childB = m_nodes[index].childB;

            m_nodes[index].bounds = AABB::combine(m_nodes[childA].bounds, m_nodes[childB].bounds);
            m_nodes[index].height = 1 + std::max(m_nodes[childA].height, m_nodes[childB].height);

            index = m_nodes[index].childA;
        }
    } else {
        m_root = sibling;
        m_nodes[sibling].childA = NULL_NODE;
        freeNode(parent);
    }
}

int32_t DynamicAABBTree::balance(int32_t nodeId) {
    assert(nodeId != NULL_NODE);

    BVHNode* node = &m_nodes[nodeId];
    if (node->isLeaf() || node->height < 2) {
        return nodeId;
    }

    int32_t childA = node->childA;
    int32_t childB = node->childB;

    int32_t balance = m_nodes[childB].height - m_nodes[childA].height;

    // Rotate B up
    if (balance > 1) {
        return rotateLeft(nodeId);
    }

    // Rotate A up
    if (balance < -1) {
        return rotateRight(nodeId);
    }

    return nodeId;
}

int32_t DynamicAABBTree::rotateLeft(int32_t nodeId) {
    int32_t childB = m_nodes[nodeId].childB;
    int32_t childC = m_nodes[childB].childA;
    int32_t childD = m_nodes[childB].childB;

    // Swap A and B
    m_nodes[childB].childA = nodeId;
    m_nodes[childB].childA = m_nodes[nodeId].childA;  // Parent
    m_nodes[nodeId].childA = childB;

    // A's old parent should point to B
    if (m_nodes[childB].childA != NULL_NODE) {
        if (m_nodes[m_nodes[childB].childA].childA == nodeId) {
            m_nodes[m_nodes[childB].childA].childA = childB;
        } else {
            m_nodes[m_nodes[childB].childA].childB = childB;
        }
    } else {
        m_root = childB;
    }

    // Rotate
    if (m_nodes[childC].height > m_nodes[childD].height) {
        m_nodes[childB].childB = childC;
        m_nodes[nodeId].childB = childD;
        m_nodes[childD].childA = nodeId;

        m_nodes[nodeId].bounds = AABB::combine(m_nodes[m_nodes[nodeId].childA].bounds, m_nodes[childD].bounds);
        m_nodes[childB].bounds = AABB::combine(m_nodes[nodeId].bounds, m_nodes[childC].bounds);

        m_nodes[nodeId].height = 1 + std::max(m_nodes[m_nodes[nodeId].childA].height, m_nodes[childD].height);
        m_nodes[childB].height = 1 + std::max(m_nodes[nodeId].height, m_nodes[childC].height);
    } else {
        m_nodes[childB].childB = childD;
        m_nodes[nodeId].childB = childC;
        m_nodes[childC].childA = nodeId;

        m_nodes[nodeId].bounds = AABB::combine(m_nodes[m_nodes[nodeId].childA].bounds, m_nodes[childC].bounds);
        m_nodes[childB].bounds = AABB::combine(m_nodes[nodeId].bounds, m_nodes[childD].bounds);

        m_nodes[nodeId].height = 1 + std::max(m_nodes[m_nodes[nodeId].childA].height, m_nodes[childC].height);
        m_nodes[childB].height = 1 + std::max(m_nodes[nodeId].height, m_nodes[childD].height);
    }

    return childB;
}

int32_t DynamicAABBTree::rotateRight(int32_t nodeId) {
    int32_t childA = m_nodes[nodeId].childA;
    int32_t childC = m_nodes[childA].childA;
    int32_t childD = m_nodes[childA].childB;

    // Swap B and A
    m_nodes[childA].childA = nodeId;
    m_nodes[childA].childA = m_nodes[nodeId].childA;  // Parent
    m_nodes[nodeId].childA = childA;

    // B's old parent should point to A
    if (m_nodes[childA].childA != NULL_NODE) {
        if (m_nodes[m_nodes[childA].childA].childA == nodeId) {
            m_nodes[m_nodes[childA].childA].childA = childA;
        } else {
            m_nodes[m_nodes[childA].childA].childB = childA;
        }
    } else {
        m_root = childA;
    }

    // Rotate
    if (m_nodes[childC].height > m_nodes[childD].height) {
        m_nodes[childA].childB = childC;
        m_nodes[nodeId].childA = childD;
        m_nodes[childD].childA = nodeId;

        m_nodes[nodeId].bounds = AABB::combine(m_nodes[childD].bounds, m_nodes[m_nodes[nodeId].childB].bounds);
        m_nodes[childA].bounds = AABB::combine(m_nodes[nodeId].bounds, m_nodes[childC].bounds);

        m_nodes[nodeId].height = 1 + std::max(m_nodes[childD].height, m_nodes[m_nodes[nodeId].childB].height);
        m_nodes[childA].height = 1 + std::max(m_nodes[nodeId].height, m_nodes[childC].height);
    } else {
        m_nodes[childA].childB = childD;
        m_nodes[nodeId].childA = childC;
        m_nodes[childC].childA = nodeId;

        m_nodes[nodeId].bounds = AABB::combine(m_nodes[childC].bounds, m_nodes[m_nodes[nodeId].childB].bounds);
        m_nodes[childA].bounds = AABB::combine(m_nodes[nodeId].bounds, m_nodes[childD].bounds);

        m_nodes[nodeId].height = 1 + std::max(m_nodes[childC].height, m_nodes[m_nodes[nodeId].childB].height);
        m_nodes[childA].height = 1 + std::max(m_nodes[nodeId].height, m_nodes[childD].height);
    }

    return childA;
}

void DynamicAABBTree::queryOverlaps(std::vector<BodyPair>& pairs) {
    pairs.clear();

    if (m_root == NULL_NODE) return;

    // Self-collision check
    collectPairs(m_root, m_root, pairs);
}

void DynamicAABBTree::collectPairs(int32_t nodeA, int32_t nodeB, std::vector<BodyPair>& pairs) {
    if (nodeA == NULL_NODE || nodeB == NULL_NODE) return;

    // Check AABB overlap
    if (!m_nodes[nodeA].bounds.overlaps(m_nodes[nodeB].bounds)) {
        return;
    }

    if (m_nodes[nodeA].isLeaf() && m_nodes[nodeB].isLeaf()) {
        if (nodeA != nodeB) {  // Don't test body against itself
            RigidBody* bodyA = m_nodes[nodeA].body;
            RigidBody* bodyB = m_nodes[nodeB].body;
            if (bodyA && bodyB) {
                pairs.emplace_back(bodyA, bodyB);
            }
        }
        return;
    }

    // Recurse
    if (m_nodes[nodeA].isLeaf()) {
        collectPairs(nodeA, m_nodes[nodeB].childA, pairs);
        collectPairs(nodeA, m_nodes[nodeB].childB, pairs);
    } else {
        collectPairs(m_nodes[nodeA].childA, nodeB, pairs);
        collectPairs(m_nodes[nodeA].childB, nodeB, pairs);
    }
}

RigidBody* DynamicAABBTree::getBody(int32_t proxyId) const {
    if (proxyId < 0 || proxyId >= m_nodeCapacity) return nullptr;
    if (!m_nodes[proxyId].isLeaf()) return nullptr;
    return m_nodes[proxyId].body;
}

int32_t DynamicAABBTree::getHeight() const {
    if (m_root == NULL_NODE) return 0;
    return m_nodes[m_root].height;
}

void DynamicAABBTree::clear() {
    m_root = NULL_NODE;
    m_nodeCount = 0;

    // Rebuild free list
    for (int32_t i = 0; i < m_nodeCapacity - 1; ++i) {
        m_nodes[i].childA = i + 1;
        m_nodes[i].height = -1;
    }
    m_nodes[m_nodeCapacity - 1].childA = NULL_NODE;
    m_nodes[m_nodeCapacity - 1].height = -1;
    m_freeList = 0;
}

}}} // namespace ohao::physics::collision
