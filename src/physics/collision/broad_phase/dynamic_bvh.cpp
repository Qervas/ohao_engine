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

    // Build free list (using internal node data for free list)
    for (int32_t i = 0; i < m_nodeCapacity - 1; ++i) {
        m_nodes[i].data = InternalNodeData{i + 1, 0, 0};
        m_nodes[i].height = -1;
    }
    m_nodes[m_nodeCapacity - 1].data = InternalNodeData{NULL_NODE, 0, 0};
    m_nodes[m_nodeCapacity - 1].height = -1;
}

DynamicAABBTree::~DynamicAABBTree() {
    // Nodes automatically freed by vector destructor
}

int32_t DynamicAABBTree::allocateNode() {
    // Expand pool if needed
    if (m_freeList == NULL_NODE) {
        assert(m_nodeCount == m_nodeCapacity);

        // CRITICAL WARNING: Vector resize invalidates all stored node indices!
        // This was causing bus errors. We now use large INITIAL_CAPACITY to avoid this.
        printf("WARNING: BVH tree resize from %d to %d nodes - this may cause crashes!\n",
               m_nodeCapacity, m_nodeCapacity * 2);

        // Double capacity
        int32_t oldCapacity = m_nodeCapacity;
        m_nodeCapacity *= 2;
        m_nodes.resize(m_nodeCapacity);

        // Build free list for new nodes (using internal node data for free list)
        for (int32_t i = m_nodeCount; i < m_nodeCapacity - 1; ++i) {
            m_nodes[i].data = InternalNodeData{i + 1, 0};
            m_nodes[i].height = -1;
        }
        m_nodes[m_nodeCapacity - 1].data = InternalNodeData{NULL_NODE, 0};
        m_nodes[m_nodeCapacity - 1].height = -1;
        m_freeList = m_nodeCount;

        // TODO: Rebuild tree to fix all parent/child indices after resize
        // For now, clearing and rebuilding is safer but loses temporal coherence
    }

    // Pop node from free list
    int32_t nodeId = m_freeList;
    // BUGFIX: Free list uses 'parent' field to store next pointer, not childA!
    m_freeList = m_nodes[nodeId].getInternal().parent;

    // Initialize as internal node (will be set to leaf in insertBody if needed)
    m_nodes[nodeId].data = InternalNodeData{NULL_NODE, NULL_NODE, NULL_NODE};
    m_nodes[nodeId].height = 0;
    m_nodes[nodeId].flags = 0;
    ++m_nodeCount;

    return nodeId;
}

void DynamicAABBTree::freeNode(int32_t nodeId) {
    assert(0 <= nodeId && nodeId < m_nodeCapacity);
    assert(m_nodeCount > 0);

    // Use internal node data to maintain free list
    m_nodes[nodeId].data = InternalNodeData{m_freeList, 0};
    m_nodes[nodeId].height = -1;
    m_freeList = nodeId;
    --m_nodeCount;
}

int32_t DynamicAABBTree::insertBody(RigidBody* body, const AABB& aabb) {
    int32_t proxyId = allocateNode();

    // Fatten AABB for stability
    m_nodes[proxyId].bounds = aabb.fatten(AABB_MARGIN);

    printf("[DynamicAABBTree] insertBody: proxyId=%d, body=%p\n", proxyId, (void*)body);
    fflush(stdout);

    // Set as leaf node with type-safe variant
    m_nodes[proxyId].data = LeafNodeData{
        NULL_NODE,  // parent (will be set by insertLeaf)
        body,
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(body) & 0xFFFFFFFF)
    };
    m_nodes[proxyId].height = 0;

    printf("[DynamicAABBTree]   After setting: body=%p, isLeaf=%d\n",
           (void*)m_nodes[proxyId].getLeaf().body, m_nodes[proxyId].isLeaf());
    fflush(stdout);

    insertLeaf(proxyId);

    printf("[DynamicAABBTree]   After insertLeaf: body=%p, isLeaf=%d\n",
           (void*)m_nodes[proxyId].getLeaf().body, m_nodes[proxyId].isLeaf());
    fflush(stdout);

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

    // Check if AABB is still fully contained in fattened bounds
    const AABB& fattenedBounds = m_nodes[proxyId].bounds;
    bool contained = (aabb.min.x >= fattenedBounds.min.x && aabb.max.x <= fattenedBounds.max.x) &&
                     (aabb.min.y >= fattenedBounds.min.y && aabb.max.y <= fattenedBounds.max.y) &&
                     (aabb.min.z >= fattenedBounds.min.z && aabb.max.z <= fattenedBounds.max.z);

    if (contained) {
        return false;  // No update needed - still fits
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
    printf("[DynamicAABBTree] insertLeaf: leaf=%d, m_root=%d\n", leaf, m_root);
    fflush(stdout);

    if (m_root == NULL_NODE) {
        m_root = leaf;
        printf("[DynamicAABBTree]   Setting leaf %d as root, calling setParent(NULL_NODE)\n", leaf);
        fflush(stdout);
        printf("[DynamicAABBTree]   Node %d isLeaf=%d, isInternal=%d\n",
               leaf, m_nodes[m_root].isLeaf(), m_nodes[m_root].isInternal());
        fflush(stdout);
        m_nodes[m_root].setParent(NULL_NODE);
        printf("[DynamicAABBTree]   setParent completed\n");
        fflush(stdout);
        return;
    }

    // Find best sibling using SAH (Surface Area Heuristic)
    AABB leafAABB = m_nodes[leaf].bounds;
    int32_t sibling = m_root;
    int32_t descendIterations = 0;
    const int32_t MAX_DESCEND_ITERATIONS = 64;  // Safety limit

    while (!m_nodes[sibling].isLeaf() && descendIterations < MAX_DESCEND_ITERATIONS) {
        ++descendIterations;
        int32_t childA = m_nodes[sibling].getInternal().childA;
        int32_t childB = m_nodes[sibling].getInternal().childB;

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
    int32_t oldParent = m_nodes[sibling].getParent();
    int32_t newParent = allocateNode();
    m_nodes[newParent].bounds = AABB::combine(leafAABB, m_nodes[sibling].bounds);
    m_nodes[newParent].height = m_nodes[sibling].height + 1;
    // Create internal node with parent and both children
    m_nodes[newParent].data = InternalNodeData{oldParent, sibling, leaf};

    if (oldParent != NULL_NODE) {
        // Sibling was not the root
        if (m_nodes[oldParent].getInternal().childA == sibling) {
            m_nodes[oldParent].getInternal().childA = newParent;
        } else {
            m_nodes[oldParent].getInternal().childB = newParent;
        }

        m_nodes[sibling].setParent(newParent);
        m_nodes[leaf].setParent(newParent);
    } else {
        // Sibling was the root
        m_nodes[sibling].setParent(newParent);
        m_nodes[leaf].setParent(newParent);
        m_root = newParent;
    }

    // Walk back up the tree fixing heights and balancing
    int32_t index = m_nodes[leaf].getParent();
    int32_t iterationCount = 0;
    const int32_t MAX_TREE_DEPTH = 64;  // Safety limit to prevent infinite loops

    while (index != NULL_NODE && iterationCount < MAX_TREE_DEPTH) {
        index = balance(index);

        int32_t childA = m_nodes[index].getInternal().childA;
        int32_t childB = m_nodes[index].getInternal().childB;

        assert(childA != NULL_NODE);
        assert(childB != NULL_NODE);

        m_nodes[index].height = 1 + std::max(m_nodes[childA].height, m_nodes[childB].height);
        m_nodes[index].bounds = AABB::combine(m_nodes[childA].bounds, m_nodes[childB].bounds);

        index = m_nodes[index].getParent();  // Move to parent
        ++iterationCount;
    }
}

void DynamicAABBTree::removeLeaf(int32_t leaf) {
    if (leaf == m_root) {
        m_root = NULL_NODE;
        return;
    }

    int32_t parent = m_nodes[leaf].getParent();
    int32_t grandParent = m_nodes[parent].getParent();
    int32_t sibling = (m_nodes[parent].getInternal().childA == leaf) ? m_nodes[parent].getInternal().childB : m_nodes[parent].getInternal().childA;

    if (grandParent != NULL_NODE) {
        // Destroy parent and connect sibling to grandParent
        if (m_nodes[grandParent].getInternal().childA == parent) {
            m_nodes[grandParent].getInternal().childA = sibling;
        } else {
            m_nodes[grandParent].getInternal().childB = sibling;
        }
        m_nodes[sibling].setParent(grandParent);
        freeNode(parent);

        // Adjust ancestor bounds
        int32_t index = grandParent;
        int32_t iterationCount = 0;
        const int32_t MAX_TREE_DEPTH = 64;  // Safety limit to prevent infinite loops

        while (index != NULL_NODE && iterationCount < MAX_TREE_DEPTH) {
            index = balance(index);

            int32_t childA = m_nodes[index].getInternal().childA;
            int32_t childB = m_nodes[index].getInternal().childB;

            m_nodes[index].bounds = AABB::combine(m_nodes[childA].bounds, m_nodes[childB].bounds);
            m_nodes[index].height = 1 + std::max(m_nodes[childA].height, m_nodes[childB].height);

            index = m_nodes[index].getParent();
            ++iterationCount;
        }
    } else {
        m_root = sibling;
        m_nodes[sibling].setParent(NULL_NODE);
        freeNode(parent);
    }
}

int32_t DynamicAABBTree::balance(int32_t nodeId) {
    assert(nodeId != NULL_NODE);

    BVHNode* node = &m_nodes[nodeId];
    if (node->isLeaf() || node->height < 2) {
        return nodeId;
    }

    int32_t childA = node->getInternal().childA;
    int32_t childB = node->getInternal().childB;

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
    int32_t childB = m_nodes[nodeId].getInternal().childB;
    int32_t childC = m_nodes[childB].getInternal().childA;
    int32_t childD = m_nodes[childB].getInternal().childB;

    // Swap A and B - B becomes parent of A
    int32_t oldParent = m_nodes[nodeId].getParent();
    m_nodes[childB].setParent(oldParent);
    m_nodes[nodeId].setParent(childB);

    // A's old parent should point to B
    if (oldParent != NULL_NODE) {
        if (m_nodes[oldParent].getInternal().childA == nodeId) {
            m_nodes[oldParent].getInternal().childA = childB;
        } else {
            m_nodes[oldParent].getInternal().childB = childB;
        }
    } else {
        m_root = childB;
    }

    // Rotate - determine which child of B to swap with A's right child
    if (m_nodes[childC].height > m_nodes[childD].height) {
        // C stays with B, D goes to A
        int32_t nodeA_childA = m_nodes[nodeId].getInternal().childA;
        m_nodes[childB].data = InternalNodeData{oldParent, nodeId, childC};
        m_nodes[nodeId].data = InternalNodeData{childB, nodeA_childA, childD};
        m_nodes[childD].setParent(nodeId);

        m_nodes[nodeId].bounds = AABB::combine(m_nodes[nodeA_childA].bounds, m_nodes[childD].bounds);
        m_nodes[childB].bounds = AABB::combine(m_nodes[nodeId].bounds, m_nodes[childC].bounds);

        m_nodes[nodeId].height = 1 + std::max(m_nodes[nodeA_childA].height, m_nodes[childD].height);
        m_nodes[childB].height = 1 + std::max(m_nodes[nodeId].height, m_nodes[childC].height);
    } else {
        // D stays with B, C goes to A
        int32_t nodeA_childA = m_nodes[nodeId].getInternal().childA;
        m_nodes[childB].data = InternalNodeData{oldParent, nodeId, childD};
        m_nodes[nodeId].data = InternalNodeData{childB, nodeA_childA, childC};
        m_nodes[childC].setParent(nodeId);

        m_nodes[nodeId].bounds = AABB::combine(m_nodes[nodeA_childA].bounds, m_nodes[childC].bounds);
        m_nodes[childB].bounds = AABB::combine(m_nodes[nodeId].bounds, m_nodes[childD].bounds);

        m_nodes[nodeId].height = 1 + std::max(m_nodes[nodeA_childA].height, m_nodes[childC].height);
        m_nodes[childB].height = 1 + std::max(m_nodes[nodeId].height, m_nodes[childD].height);
    }

    return childB;
}

int32_t DynamicAABBTree::rotateRight(int32_t nodeId) {
    int32_t childA = m_nodes[nodeId].getInternal().childA;
    int32_t childC = m_nodes[childA].getInternal().childA;
    int32_t childD = m_nodes[childA].getInternal().childB;

    // Swap B and A - A becomes parent of B
    int32_t oldParent = m_nodes[nodeId].getParent();
    m_nodes[childA].setParent(oldParent);
    m_nodes[nodeId].setParent(childA);

    // B's old parent should point to A
    if (oldParent != NULL_NODE) {
        if (m_nodes[oldParent].getInternal().childA == nodeId) {
            m_nodes[oldParent].getInternal().childA = childA;
        } else {
            m_nodes[oldParent].getInternal().childB = childA;
        }
    } else {
        m_root = childA;
    }

    // Rotate - determine which child of A to swap with B's left child
    if (m_nodes[childC].height > m_nodes[childD].height) {
        // C stays with A, D goes to B
        int32_t nodeB_childB = m_nodes[nodeId].getInternal().childB;
        m_nodes[childA].data = InternalNodeData{oldParent, nodeId, childC};
        m_nodes[nodeId].data = InternalNodeData{childA, childD, nodeB_childB};
        m_nodes[childD].setParent(nodeId);

        m_nodes[nodeId].bounds = AABB::combine(m_nodes[childD].bounds, m_nodes[nodeB_childB].bounds);
        m_nodes[childA].bounds = AABB::combine(m_nodes[nodeId].bounds, m_nodes[childC].bounds);

        m_nodes[nodeId].height = 1 + std::max(m_nodes[childD].height, m_nodes[nodeB_childB].height);
        m_nodes[childA].height = 1 + std::max(m_nodes[nodeId].height, m_nodes[childC].height);
    } else {
        // D stays with A, C goes to B
        int32_t nodeB_childB = m_nodes[nodeId].getInternal().childB;
        m_nodes[childA].data = InternalNodeData{oldParent, nodeId, childD};
        m_nodes[nodeId].data = InternalNodeData{childA, childC, nodeB_childB};
        m_nodes[childC].setParent(nodeId);

        m_nodes[nodeId].bounds = AABB::combine(m_nodes[childC].bounds, m_nodes[nodeB_childB].bounds);
        m_nodes[childA].bounds = AABB::combine(m_nodes[nodeId].bounds, m_nodes[childD].bounds);

        m_nodes[nodeId].height = 1 + std::max(m_nodes[childC].height, m_nodes[nodeB_childB].height);
        m_nodes[childA].height = 1 + std::max(m_nodes[nodeId].height, m_nodes[childD].height);
    }

    return childA;
}

void DynamicAABBTree::queryOverlaps(std::vector<BodyPair>& pairs) {
    printf("[DynamicAABBTree] queryOverlaps START (root=%d, nodeCount=%d, capacity=%d)\n",
           m_root, m_nodeCount, m_nodeCapacity);
    fflush(stdout);

    pairs.clear();

    if (m_root == NULL_NODE) {
        printf("[DynamicAABBTree]   Root is NULL_NODE, returning\n");
        fflush(stdout);
        return;
    }

    // Self-collision check
    printf("[DynamicAABBTree]   Calling collectPairs with root=%d\n", m_root);
    fflush(stdout);
    collectPairs(m_root, m_root, pairs);
    printf("[DynamicAABBTree]   collectPairs completed, found %zu pairs\n", pairs.size());
    fflush(stdout);
}

void DynamicAABBTree::collectPairs(int32_t nodeA, int32_t nodeB, std::vector<BodyPair>& pairs) {
    if (nodeA == NULL_NODE || nodeB == NULL_NODE) return;

    // Bounds check before accessing
    if (nodeA < 0 || nodeA >= m_nodeCapacity || nodeB < 0 || nodeB >= m_nodeCapacity) {
        printf("[DynamicAABBTree] ERROR: Invalid node indices! nodeA=%d, nodeB=%d, capacity=%d\n",
               nodeA, nodeB, m_nodeCapacity);
        fflush(stdout);
        return;
    }

    // Debug: Print node info
    bool aIsLeaf = m_nodes[nodeA].isLeaf();
    bool bIsLeaf = m_nodes[nodeB].isLeaf();
    printf("[DynamicAABBTree] collectPairs(nodeA=%d, nodeB=%d): aIsLeaf=%d, bIsLeaf=%d\n",
           nodeA, nodeB, aIsLeaf, bIsLeaf);
    fflush(stdout);

    // Check AABB overlap
    const AABB& boundsA = m_nodes[nodeA].bounds;
    const AABB& boundsB = m_nodes[nodeB].bounds;

    if (!boundsA.overlaps(boundsB)) {
        printf("[DynamicAABBTree]   No overlap (A: [%.2f,%.2f] vs B: [%.2f,%.2f]), returning\n",
               boundsA.min.y, boundsA.max.y, boundsB.min.y, boundsB.max.y);
        fflush(stdout);
        return;
    }

    printf("[DynamicAABBTree]   AABBs overlap\n");
    fflush(stdout);

    if (aIsLeaf && bIsLeaf) {
        printf("[DynamicAABBTree]   Both are leaves\n");
        fflush(stdout);
        if (nodeA != nodeB) {  // Don't test body against itself
            RigidBody* bodyA = m_nodes[nodeA].getLeaf().body;
            RigidBody* bodyB = m_nodes[nodeB].getLeaf().body;
            printf("[DynamicAABBTree]     bodyA=%p, bodyB=%p\n", (void*)bodyA, (void*)bodyB);
            fflush(stdout);
            if (bodyA && bodyB) {
                pairs.emplace_back(bodyA, bodyB);
            }
        } else {
            printf("[DynamicAABBTree]     Same node, skipping\n");
            fflush(stdout);
        }
        return;
    }

    printf("[DynamicAABBTree]   At least one is internal, recursing\n");
    fflush(stdout);

    // Recurse
    if (m_nodes[nodeA].isLeaf()) {
        const auto& bInternal = m_nodes[nodeB].getInternal();
        printf("[DynamicAABBTree]     NodeA is leaf, recursing with B's children: childA=%d, childB=%d\n",
               bInternal.childA, bInternal.childB);
        fflush(stdout);
        collectPairs(nodeA, bInternal.childA, pairs);
        collectPairs(nodeA, bInternal.childB, pairs);
    } else {
        const auto& aInternal = m_nodes[nodeA].getInternal();
        printf("[DynamicAABBTree]     NodeA is internal, recursing with A's children: childA=%d, childB=%d\n",
               aInternal.childA, aInternal.childB);
        fflush(stdout);
        collectPairs(aInternal.childA, nodeB, pairs);
        collectPairs(aInternal.childB, nodeB, pairs);
    }
}

RigidBody* DynamicAABBTree::getBody(int32_t proxyId) const {
    if (proxyId < 0 || proxyId >= m_nodeCapacity) return nullptr;
    if (!m_nodes[proxyId].isLeaf()) return nullptr;
    return m_nodes[proxyId].getLeaf().body;
}

int32_t DynamicAABBTree::getHeight() const {
    if (m_root == NULL_NODE) return 0;
    return m_nodes[m_root].height;
}

void DynamicAABBTree::clear() {
    m_root = NULL_NODE;
    m_nodeCount = 0;

    // Rebuild free list (using internal node data for free list)
    for (int32_t i = 0; i < m_nodeCapacity - 1; ++i) {
        m_nodes[i].data = InternalNodeData{i + 1, 0, 0};
        m_nodes[i].height = -1;
    }
    m_nodes[m_nodeCapacity - 1].data = InternalNodeData{NULL_NODE, 0, 0};
    m_nodes[m_nodeCapacity - 1].height = -1;
    m_freeList = 0;
}

}}} // namespace ohao::physics::collision
