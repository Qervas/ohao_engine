#include "broad_phase.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include <algorithm>
#include <chrono>

namespace ohao {
namespace physics {
namespace collision {

// === SPATIAL HASH GRID IMPLEMENTATION ===

SpatialHashGrid::SpatialHashGrid(float cellSize) : m_cellSize(cellSize) {
    // Ensure minimum cell size to avoid numerical issues
    m_cellSize = std::max(cellSize, 0.1f);
}

void SpatialHashGrid::clear() {
    m_cells.clear();
}

void SpatialHashGrid::insertBody(dynamics::RigidBody* body) {
    if (!body) return;
    
    math::AABB aabb = body->getAABB();
    std::vector<int64_t> cellKeys = getCellKeysForAABB(aabb);
    
    for (int64_t key : cellKeys) {
        m_cells[key].addBody(body);
    }
}

std::vector<BodyPair> SpatialHashGrid::getPotentialPairs() const {
    std::vector<BodyPair> pairs;
    std::unordered_set<BodyPair, BodyPair::Hash> seenPairs;
    
    // For each cell, test all body pairs within that cell
    for (const auto& [key, cell] : m_cells) {
        const std::vector<dynamics::RigidBody*>& bodies = cell.bodies;
        
        for (size_t i = 0; i < bodies.size(); ++i) {
            for (size_t j = i + 1; j < bodies.size(); ++j) {
                // Create body pair (with consistent ordering)
                uint32_t idA = reinterpret_cast<uintptr_t>(bodies[i]) & 0xFFFFFFFF;
                uint32_t idB = reinterpret_cast<uintptr_t>(bodies[j]) & 0xFFFFFFFF;
                BodyPair pair(idA, idB);
                
                // Only add if we haven't seen this pair yet
                if (seenPairs.find(pair) == seenPairs.end()) {
                    // Verify AABB overlap (since bodies might span multiple cells)
                    if (BroadPhase::testAABBOverlap(bodies[i], bodies[j])) {
                        pairs.push_back(pair);
                        seenPairs.insert(pair);
                    }
                }
            }
        }
    }
    
    return pairs;
}

size_t SpatialHashGrid::getTotalBodiesInGrid() const {
    size_t total = 0;
    for (const auto& [key, cell] : m_cells) {
        total += cell.bodies.size();
    }
    return total;
}

int64_t SpatialHashGrid::hashPosition(const glm::vec3& position) const {
    glm::ivec3 coords = getGridCoords(position);
    return gridCoordsToKey(coords.x, coords.y, coords.z);
}

std::vector<int64_t> SpatialHashGrid::getCellKeysForAABB(const math::AABB& aabb) const {
    std::vector<int64_t> keys;
    
    glm::ivec3 minCoords = getGridCoords(aabb.min);
    glm::ivec3 maxCoords = getGridCoords(aabb.max);
    
    for (int32_t x = minCoords.x; x <= maxCoords.x; ++x) {
        for (int32_t y = minCoords.y; y <= maxCoords.y; ++y) {
            for (int32_t z = minCoords.z; z <= maxCoords.z; ++z) {
                keys.push_back(gridCoordsToKey(x, y, z));
            }
        }
    }
    
    return keys;
}

int64_t SpatialHashGrid::gridCoordsToKey(int32_t x, int32_t y, int32_t z) const {
    // Pack 3D coordinates into 64-bit key using bit shifting
    // This assumes coordinates fit in 21 bits each (±1M range)
    constexpr int64_t COORD_MASK = 0x1FFFFF; // 21 bits
    
    int64_t key = (static_cast<int64_t>(x & COORD_MASK) << 42) |
                  (static_cast<int64_t>(y & COORD_MASK) << 21) |
                  (static_cast<int64_t>(z & COORD_MASK));
    
    return key;
}

glm::ivec3 SpatialHashGrid::getGridCoords(const glm::vec3& position) const {
    return glm::ivec3(
        static_cast<int32_t>(std::floor(position.x / m_cellSize)),
        static_cast<int32_t>(std::floor(position.y / m_cellSize)),
        static_cast<int32_t>(std::floor(position.z / m_cellSize))
    );
}

// === BROAD PHASE IMPLEMENTATION ===

BroadPhase::BroadPhase(Algorithm algorithm) : m_algorithm(algorithm) {
    switch (m_algorithm) {
        case Algorithm::SPATIAL_HASH:
            m_spatialHash = std::make_unique<SpatialHashGrid>(5.0f);
            break;
        default:
            break;
    }
}

void BroadPhase::setAlgorithm(Algorithm algorithm) {
    if (m_algorithm == algorithm) return;
    
    m_algorithm = algorithm;
    
    switch (m_algorithm) {
        case Algorithm::SPATIAL_HASH:
            if (!m_spatialHash) {
                m_spatialHash = std::make_unique<SpatialHashGrid>(5.0f);
            }
            break;
        default:
            m_spatialHash.reset();
            break;
    }
}

void BroadPhase::update(const std::vector<dynamics::RigidBody*>& bodies) {
    auto startTime = std::chrono::high_resolution_clock::now();
    
    m_bodies = bodies;
    m_stats.totalBodies = bodies.size();
    
    // Clear previous frame data
    if (m_spatialHash) {
        m_spatialHash->clear();
    }
    
    // Populate spatial structure
    switch (m_algorithm) {
        case Algorithm::SPATIAL_HASH:
            for (auto* body : bodies) {
                if (body) { // Insert both dynamic and static bodies
                    m_spatialHash->insertBody(body);
                    getBodyId(body); // Ensure body has an ID
                }
            }
            m_stats.activeCells = m_spatialHash->getActiveCellCount();
            break;
        default:
            break;
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    m_stats.updateTimeMs = duration.count() / 1000.0f;
}

std::vector<BodyPair> BroadPhase::getPotentialPairs() {
    switch (m_algorithm) {
        case Algorithm::SPATIAL_HASH:
            return getSpatialHashPairs();
        case Algorithm::AABB_SIMPLE:
        default:
            return getSimplePairs();
    }
}

bool BroadPhase::testAABBOverlap(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB) {
    if (!bodyA || !bodyB) return false;
    
    math::AABB aabbA = bodyA->getAABB();
    math::AABB aabbB = bodyB->getAABB();
    
    return aabbA.intersects(aabbB);
}

void BroadPhase::setSpatialHashCellSize(float cellSize) {
    if (m_spatialHash) {
        m_spatialHash->setCellSize(cellSize);
    }
}

std::vector<BodyPair> BroadPhase::getSimplePairs() {
    std::vector<BodyPair> pairs;
    
    for (size_t i = 0; i < m_bodies.size(); ++i) {
        for (size_t j = i + 1; j < m_bodies.size(); ++j) {
            auto* bodyA = m_bodies[i];
            auto* bodyB = m_bodies[j];
            
            if (!bodyA || !bodyB) continue;
            
            // Skip static-static pairs
            if (bodyA->isStatic() && bodyB->isStatic()) continue;
            
            if (testAABBOverlap(bodyA, bodyB)) {
                uint32_t idA = getBodyId(bodyA);
                uint32_t idB = getBodyId(bodyB);
                pairs.emplace_back(idA, idB);
            }
        }
    }
    
    m_stats.potentialPairs = pairs.size();
    return pairs;
}

std::vector<BodyPair> BroadPhase::getSpatialHashPairs() {
    if (!m_spatialHash) return getSimplePairs();
    
    std::vector<BodyPair> pairs = m_spatialHash->getPotentialPairs();
    m_stats.potentialPairs = pairs.size();
    return pairs;
}

uint32_t BroadPhase::getBodyId(dynamics::RigidBody* body) {
    auto it = m_bodyToId.find(body);
    if (it != m_bodyToId.end()) {
        return it->second;
    }
    
    uint32_t id = m_nextBodyId++;
    m_bodyToId[body] = id;
    return id;
}

// === CONTACT CACHE IMPLEMENTATION ===

ContactManifold* ContactCache::getOrCreateManifold(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB) {
    BodyPair pair = makeBodyPair(bodyA, bodyB);
    
    auto it = m_manifolds.find(pair);
    if (it != m_manifolds.end()) {
        it->second->isActive = true;
        it->second->lifetime = 0.0f; // Reset lifetime
        return it->second.get();
    }
    
    // Create new manifold
    auto manifold = std::make_unique<ContactManifold>();
    manifold->bodyA = bodyA;
    manifold->bodyB = bodyB;
    manifold->isActive = true;
    manifold->lifetime = 0.0f;
    
    ContactManifold* result = manifold.get();
    m_manifolds[pair] = std::move(manifold);
    return result;
}

void ContactCache::removeManifold(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB) {
    BodyPair pair = makeBodyPair(bodyA, bodyB);
    m_manifolds.erase(pair);
}

void ContactCache::removeManifolds(dynamics::RigidBody* body) {
    auto it = m_manifolds.begin();
    while (it != m_manifolds.end()) {
        if (it->second->bodyA == body || it->second->bodyB == body) {
            it = m_manifolds.erase(it);
        } else {
            ++it;
        }
    }
}

void ContactCache::update(float deltaTime) {
    auto it = m_manifolds.begin();
    while (it != m_manifolds.end()) {
        auto& manifold = it->second;
        manifold->updateLifetime(deltaTime);
        
        // Remove stale manifolds
        if (!manifold->isActive && manifold->lifetime > MAX_MANIFOLD_LIFETIME) {
            it = m_manifolds.erase(it);
        } else {
            // Mark as inactive for this frame (will be reactivated if collision detected)
            manifold->isActive = false;
            ++it;
        }
    }
}

std::vector<ContactManifold*> ContactCache::getActiveManifolds() {
    std::vector<ContactManifold*> active;
    for (auto& [pair, manifold] : m_manifolds) {
        if (manifold->isActive) {
            active.push_back(manifold.get());
        }
    }
    return active;
}

void ContactCache::clear() {
    m_manifolds.clear();
}

BodyPair ContactCache::makeBodyPair(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB) {
    uintptr_t ptrA = reinterpret_cast<uintptr_t>(bodyA);
    uintptr_t ptrB = reinterpret_cast<uintptr_t>(bodyB);
    return BodyPair(static_cast<uint32_t>(ptrA), static_cast<uint32_t>(ptrB));
}

} // namespace collision
} // namespace physics
} // namespace ohao