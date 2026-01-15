#include "collision_system.hpp"
#include "broad_phase/dynamic_bvh.hpp"
#include "narrow_phase/gjk_solver.hpp"
#include "narrow_phase/epa_solver.hpp"
#include "contact_manifold.hpp"
#include "../dynamics/rigid_body.hpp"
#include "shapes/collision_shape.hpp"

namespace ohao {
namespace physics {

using dynamics::RigidBody;

namespace collision {

CollisionSystem::CollisionSystem()
    : m_broadPhase(std::make_unique<DynamicAABBTree>())
    , m_gjkSolver(std::make_unique<GJKSolver>())
    , m_epaSolver(std::make_unique<EPASolver>())
{
}

CollisionSystem::~CollisionSystem() {
}

void CollisionSystem::updateBroadPhase(const std::vector<RigidBody*>& bodies) {
    // Early exit if no bodies
    if (bodies.empty()) {
        return;
    }

    // Update or insert bodies into broad phase
    for (RigidBody* body : bodies) {
        if (!body) {
            continue;
        }

        // Skip bodies without collision shapes - they shouldn't participate in collision detection
        if (!body->getCollisionShape()) {
            continue;
        }

        // Get body's AABB
        math::AABB mathAABB = body->getAABB();
        AABB aabb(mathAABB.min, mathAABB.max);

        auto it = m_bodyProxies.find(body);
        if (it != m_bodyProxies.end()) {
            // Update existing proxy
            glm::vec3 displacement = body->getLinearVelocity() * (1.0f / 60.0f);  // Predict motion
            m_broadPhase->updateBody(it->second, aabb, displacement);
        } else {
            // Insert new body
            int32_t proxyId = m_broadPhase->insertBody(body, aabb);
            m_bodyProxies[body] = proxyId;
        }
    }

    // Remove bodies that are no longer in the list
    std::vector<RigidBody*> toRemove;
    for (auto& pair : m_bodyProxies) {
        bool found = false;
        for (RigidBody* body : bodies) {
            if (body == pair.first) {
                found = true;
                break;
            }
        }
        if (!found) {
            toRemove.push_back(pair.first);
        }
    }

    for (RigidBody* body : toRemove) {
        auto it = m_bodyProxies.find(body);
        if (it != m_bodyProxies.end()) {
            m_broadPhase->removeBody(it->second);
            m_bodyProxies.erase(it);
        }
    }
}

std::vector<ContactManifold*> CollisionSystem::performNarrowPhase() {
    // Get overlapping pairs from broad phase
    std::vector<BodyPair> pairs;
    m_broadPhase->queryOverlaps(pairs);

    // Clear old manifolds that are no longer active
    std::unordered_map<uint64_t, ContactManifold*> activeManifolds;

    // Process each pair with narrow phase
    for (const BodyPair& pair : pairs) {
        ContactManifold* manifold = getOrCreateManifold(pair.bodyA, pair.bodyB);

        // Clear previous contacts
        manifold->clear();

        // Run GJK/EPA collision detection
        bool colliding = detectCollision(pair.bodyA, pair.bodyB, manifold);

        if (colliding && manifold->getContactCount() > 0) {
            uint64_t hash = computePairHash(pair.bodyA, pair.bodyB);
            activeManifolds[hash] = manifold;
        }
    }

    // Update manifold map with only active manifolds
    m_manifoldMap = activeManifolds;

    // Return active manifolds as vector
    std::vector<ContactManifold*> result;
    result.reserve(activeManifolds.size());
    for (auto& pair : activeManifolds) {
        result.push_back(pair.second);
    }

    return result;
}

bool CollisionSystem::detectCollision(RigidBody* bodyA, RigidBody* bodyB, ContactManifold* manifold) {
    if (!bodyA || !bodyB) return false;

    auto shapeA = bodyA->getCollisionShape();
    auto shapeB = bodyB->getCollisionShape();
    if (!shapeA || !shapeB) {
        return false;
    }

    // Get transforms
    glm::mat4 transformA = bodyA->getTransformMatrix();
    glm::mat4 transformB = bodyB->getTransformMatrix();

    // Run GJK
    GJKSolver::Result gjkResult = m_gjkSolver->solve(shapeA.get(), transformA, shapeB.get(), transformB);

    if (gjkResult.intersecting) {
        // Shapes are intersecting - run EPA for penetration depth
        EPASolver::Result epaResult = m_epaSolver->solve(gjkResult.simplex, shapeA.get(), transformA, shapeB.get(), transformB);

        if (epaResult.success) {
            // CRITICAL FIX: EPA normal is inverted
            // EPA should return normal from B→A (ground→cube = pointing UP)
            // But it's actually returning it pointing DOWN (0,-1,0)
            // Constraint solver expects normal that pushes bodyA away from bodyB
            // So we flip it to get the correct direction
            glm::vec3 separationNormal = -epaResult.normal;

            // Add contact point
            manifold->setNormal(separationNormal);
            manifold->addContact(epaResult.contactPointA, separationNormal, epaResult.penetrationDepth);

            // Set material properties from bodies (average of both)
            float restitution = (bodyA->getRestitution() + bodyB->getRestitution()) * 0.5f;
            float friction = (bodyA->getStaticFriction() + bodyB->getStaticFriction()) * 0.5f;
            manifold->setFriction(friction);
            manifold->setRestitution(restitution);

            return true;
        }
    }

    return false;
}

ContactManifold* CollisionSystem::getOrCreateManifold(RigidBody* bodyA, RigidBody* bodyB) {
    uint64_t hash = computePairHash(bodyA, bodyB);

    auto it = m_manifoldMap.find(hash);
    if (it != m_manifoldMap.end()) {
        // Reuse existing manifold
        return it->second;
    }

    // Create new manifold
    m_manifolds.push_back(std::make_unique<ContactManifold>(bodyA, bodyB));
    ContactManifold* manifold = m_manifolds.back().get();
    m_manifoldMap[hash] = manifold;

    return manifold;
}

uint64_t CollisionSystem::computePairHash(RigidBody* bodyA, RigidBody* bodyB) const {
    // Ensure consistent ordering
    if (bodyA > bodyB) {
        std::swap(bodyA, bodyB);
    }

    uint64_t ptrA = reinterpret_cast<uintptr_t>(bodyA);
    uint64_t ptrB = reinterpret_cast<uintptr_t>(bodyB);

    // Cantor pairing function
    return ((ptrA + ptrB) * (ptrA + ptrB + 1) / 2) + ptrB;
}

void CollisionSystem::clear() {
    m_broadPhase->clear();
    m_manifolds.clear();
    m_manifoldMap.clear();
    m_bodyProxies.clear();
}

int CollisionSystem::getBroadPhasePairCount() const {
    std::vector<BodyPair> pairs;
    const_cast<DynamicAABBTree*>(m_broadPhase.get())->queryOverlaps(pairs);
    return static_cast<int>(pairs.size());
}

}}} // namespace ohao::physics::collision
