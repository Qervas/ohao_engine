#include "physics_controller.h"

#include "physics/world/physics_world.hpp"
#include "engine/scene/scene.hpp"

#include <vector>

namespace godot {

// ===== Physics Simulation =====

void PhysicsController::play(ohao::Scene* scene) {
    m_playing = true;
    if (scene && scene->getPhysicsWorld()) {
        scene->getPhysicsWorld()->setSimulationState(
            ohao::physics::SimulationState::RUNNING);
    }
}

void PhysicsController::pause(ohao::Scene* scene) {
    m_playing = false;
    if (scene && scene->getPhysicsWorld()) {
        scene->getPhysicsWorld()->pause();
    }
}

void PhysicsController::step(ohao::Scene* scene) {
    if (scene && scene->getPhysicsWorld()) {
        scene->getPhysicsWorld()->stepOnce();
    }
}

void PhysicsController::stop(ohao::Scene* scene) {
    m_playing = false;
    if (scene && scene->getPhysicsWorld()) {
        scene->getPhysicsWorld()->stop();
    }
}

// ===== Raycasting =====

PhysicsController::RayHitResult PhysicsController::castRay(ohao::Scene* scene, const glm::vec3& origin,
                                                            const glm::vec3& direction, float maxDistance,
                                                            uint16_t layerMask) {
    RayHitResult result;
    if (!scene) return result;

    auto* physWorld = scene->getPhysicsWorld();
    if (!physWorld) return result;

    ohao::physics::backend::RaycastHit hit;
    if (physWorld->castRay(origin, direction, maxDistance, hit, layerMask)) {
        result.hit = true;
        result.position = hit.position;
        result.normal = hit.normal;
        result.fraction = hit.fraction;
        result.bodyHandle = hit.bodyHandle;
        result.layer = hit.layer;
    }
    return result;
}

std::vector<PhysicsController::RayHitEntry> PhysicsController::castRayAll(ohao::Scene* scene, const glm::vec3& origin,
                                                                           const glm::vec3& direction, float maxDistance,
                                                                           uint16_t layerMask) {
    std::vector<RayHitEntry> results;
    if (!scene) return results;

    auto* physWorld = scene->getPhysicsWorld();
    if (!physWorld) return results;

    auto hits = physWorld->castRayAll(origin, direction, maxDistance, layerMask);
    results.reserve(hits.size());
    for (const auto& hit : hits) {
        RayHitEntry entry;
        entry.position = hit.position;
        entry.normal = hit.normal;
        entry.fraction = hit.fraction;
        entry.bodyHandle = hit.bodyHandle;
        entry.layer = hit.layer;
        results.push_back(entry);
    }
    return results;
}

std::vector<uint32_t> PhysicsController::overlapSphere(ohao::Scene* scene, const glm::vec3& center,
                                                        float radius, uint16_t layerMask) {
    if (!scene) return {};

    auto* physWorld = scene->getPhysicsWorld();
    if (!physWorld) return {};

    return physWorld->overlapSphere(center, radius, layerMask);
}

std::vector<uint32_t> PhysicsController::overlapBox(ohao::Scene* scene, const glm::vec3& center,
                                                     const glm::vec3& halfExtents, const glm::quat& rotation,
                                                     uint16_t layerMask) {
    if (!scene) return {};

    auto* physWorld = scene->getPhysicsWorld();
    if (!physWorld) return {};

    return physWorld->overlapBox(center, halfExtents, rotation, layerMask);
}

// ===== Collision Layers =====

void PhysicsController::setLayerCollision(ohao::Scene* scene, uint16_t layer1, uint16_t layer2, bool shouldCollide) {
    if (!scene) return;
    auto* physWorld = scene->getPhysicsWorld();
    if (physWorld) physWorld->setLayerCollision(layer1, layer2, shouldCollide);
}

// ===== Constraints =====

static int createConstraintImpl(ohao::Scene* scene, const ohao::physics::backend::ConstraintSettings& cs) {
    if (!scene) return -1;
    auto* physWorld = scene->getPhysicsWorld();
    if (!physWorld) return -1;
    return static_cast<int>(physWorld->createConstraint(cs));
}

int PhysicsController::createConstraintFixed(ohao::Scene* scene, uint32_t body1, uint32_t body2,
                                              const glm::vec3& anchor) {
    ohao::physics::backend::ConstraintSettings cs;
    cs.type = ohao::physics::backend::ConstraintType::FIXED;
    cs.body1 = body1;
    cs.body2 = body2;
    cs.anchor1 = anchor;
    cs.anchor2 = anchor;
    return createConstraintImpl(scene, cs);
}

int PhysicsController::createConstraintHinge(ohao::Scene* scene, uint32_t body1, uint32_t body2,
                                              const glm::vec3& anchor, const glm::vec3& axis,
                                              float limitMin, float limitMax) {
    ohao::physics::backend::ConstraintSettings cs;
    cs.type = ohao::physics::backend::ConstraintType::HINGE;
    cs.body1 = body1;
    cs.body2 = body2;
    cs.anchor1 = anchor;
    cs.anchor2 = anchor;
    cs.axis1 = axis;
    cs.axis2 = axis;
    if (limitMin != 0.0f || limitMax != 0.0f) {
        cs.enableLimits = true;
        cs.limitMin = limitMin;
        cs.limitMax = limitMax;
    }
    return createConstraintImpl(scene, cs);
}

int PhysicsController::createConstraintSlider(ohao::Scene* scene, uint32_t body1, uint32_t body2,
                                               const glm::vec3& axis, float limitMin, float limitMax) {
    ohao::physics::backend::ConstraintSettings cs;
    cs.type = ohao::physics::backend::ConstraintType::SLIDER;
    cs.body1 = body1;
    cs.body2 = body2;
    cs.axis1 = axis;
    cs.axis2 = axis;
    if (limitMin != 0.0f || limitMax != 0.0f) {
        cs.enableLimits = true;
        cs.limitMin = limitMin;
        cs.limitMax = limitMax;
    }
    return createConstraintImpl(scene, cs);
}

int PhysicsController::createConstraintPoint(ohao::Scene* scene, uint32_t body1, uint32_t body2,
                                              const glm::vec3& anchor1, const glm::vec3& anchor2) {
    ohao::physics::backend::ConstraintSettings cs;
    cs.type = ohao::physics::backend::ConstraintType::POINT;
    cs.body1 = body1;
    cs.body2 = body2;
    cs.anchor1 = anchor1;
    cs.anchor2 = anchor2;
    return createConstraintImpl(scene, cs);
}

int PhysicsController::createConstraintDistance(ohao::Scene* scene, uint32_t body1, uint32_t body2,
                                                const glm::vec3& anchor1, const glm::vec3& anchor2,
                                                float minDist, float maxDist) {
    ohao::physics::backend::ConstraintSettings cs;
    cs.type = ohao::physics::backend::ConstraintType::DISTANCE;
    cs.body1 = body1;
    cs.body2 = body2;
    cs.anchor1 = anchor1;
    cs.anchor2 = anchor2;
    cs.minDistance = minDist;
    cs.maxDistance = maxDist;
    return createConstraintImpl(scene, cs);
}

int PhysicsController::createConstraintCone(ohao::Scene* scene, uint32_t body1, uint32_t body2,
                                             const glm::vec3& anchor, const glm::vec3& twistAxis,
                                             float halfConeAngle) {
    ohao::physics::backend::ConstraintSettings cs;
    cs.type = ohao::physics::backend::ConstraintType::CONE_TWIST;
    cs.body1 = body1;
    cs.body2 = body2;
    cs.anchor1 = anchor;
    cs.anchor2 = anchor;
    cs.axis1 = twistAxis;
    cs.axis2 = twistAxis;
    cs.enableLimits = true;
    cs.limitMax = halfConeAngle;
    return createConstraintImpl(scene, cs);
}

void PhysicsController::destroyConstraint(ohao::Scene* scene, uint32_t handle) {
    if (!scene) return;
    auto* physWorld = scene->getPhysicsWorld();
    if (physWorld) physWorld->destroyConstraint(handle);
}

void PhysicsController::setConstraintEnabled(ohao::Scene* scene, uint32_t handle, bool enabled) {
    if (!scene) return;
    auto* physWorld = scene->getPhysicsWorld();
    if (physWorld) physWorld->setConstraintEnabled(handle, enabled);
}

void PhysicsController::setConstraintMotor(ohao::Scene* scene, uint32_t handle, bool enabled, float speed, float maxForce) {
    if (!scene) return;
    auto* physWorld = scene->getPhysicsWorld();
    if (physWorld) physWorld->setConstraintMotorState(handle, enabled, speed, maxForce);
}

void PhysicsController::setConstraintLimits(ohao::Scene* scene, uint32_t handle, float minVal, float maxVal) {
    if (!scene) return;
    auto* physWorld = scene->getPhysicsWorld();
    if (physWorld) physWorld->setConstraintLimits(handle, minVal, maxVal);
}

// ===== Character Controller =====

int PhysicsController::createCharacter(ohao::Scene* scene, const glm::vec3& position, float capsuleRadius,
                                        float capsuleHeight, float maxSlopeDeg, float mass) {
    if (!scene) return -1;
    auto* physWorld = scene->getPhysicsWorld();
    if (!physWorld) return -1;

    ohao::physics::backend::CharacterCreationInfo info;
    info.position = position;
    info.capsuleRadius = capsuleRadius;
    info.capsuleHeight = capsuleHeight;
    info.maxSlopeAngleDeg = maxSlopeDeg;
    info.mass = mass;
    return static_cast<int>(physWorld->createCharacter(info));
}

void PhysicsController::destroyCharacter(ohao::Scene* scene, uint32_t handle) {
    if (!scene) return;
    auto* physWorld = scene->getPhysicsWorld();
    if (physWorld) physWorld->destroyCharacter(handle);
}

PhysicsController::CharacterStateResult PhysicsController::getCharacterState(ohao::Scene* scene, uint32_t handle) {
    CharacterStateResult result;
    if (!scene) return result;

    auto* physWorld = scene->getPhysicsWorld();
    if (!physWorld) return result;

    auto state = physWorld->getCharacterState(handle);
    result.position = state.position;
    result.velocity = state.linearVelocity;
    result.groundNormal = state.groundNormal;
    result.groundState = static_cast<int>(state.groundState);
    result.groundBody = static_cast<int>(state.groundBody);
    return result;
}

void PhysicsController::setCharacterPosition(ohao::Scene* scene, uint32_t handle, const glm::vec3& position) {
    if (!scene) return;
    auto* physWorld = scene->getPhysicsWorld();
    if (physWorld) physWorld->setCharacterPosition(handle, position);
}

void PhysicsController::setCharacterRotation(ohao::Scene* scene, uint32_t handle, const glm::quat& rotation) {
    if (!scene) return;
    auto* physWorld = scene->getPhysicsWorld();
    if (physWorld) physWorld->setCharacterRotation(handle, rotation);
}

void PhysicsController::setCharacterVelocity(ohao::Scene* scene, uint32_t handle, const glm::vec3& velocity) {
    if (!scene) return;
    auto* physWorld = scene->getPhysicsWorld();
    if (physWorld) physWorld->setCharacterLinearVelocity(handle, velocity);
}

void PhysicsController::updateCharacter(ohao::Scene* scene, uint32_t handle, float delta,
                                         const glm::vec3& gravity, const glm::vec3& movementInput) {
    if (!scene) return;
    auto* physWorld = scene->getPhysicsWorld();
    if (physWorld) physWorld->updateCharacter(handle, delta, gravity, movementInput);
}

} // namespace godot
