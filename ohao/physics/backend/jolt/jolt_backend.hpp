#pragma once

#if !defined(OHAO_HAS_JOLT) || OHAO_HAS_JOLT

#include "physics/backend/physics_backend.hpp"
#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>

// Jolt headers
#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Constraints/Constraint.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

namespace ohao {
namespace physics {
namespace backend {

/**
 * Jolt Physics backend - production-grade physics.
 *
 * MIT license, used by Godot 4.x and Horizon Forbidden West.
 * Provides rigid bodies, character controllers, vehicles, joints,
 * CCD, broadphase layers, body sleeping, and more.
 */
class JoltPhysicsBackend : public IPhysicsBackend {
public:
    JoltPhysicsBackend();
    ~JoltPhysicsBackend() override;

    // === IPhysicsBackend - Lifecycle ===
    bool initialize(const PhysicsWorldConfig& config) override;
    void shutdown() override;
    bool isInitialized() const override { return m_initialized; }

    // === Simulation ===
    void step(float deltaTime) override;

    // === Body Management ===
    BodyHandle createBody(const BodyCreationInfo& info) override;
    void destroyBody(BodyHandle handle) override;
    bool isValidBody(BodyHandle handle) const override;

    // === Body Transform ===
    void setPosition(BodyHandle h, const glm::vec3& pos) override;
    glm::vec3 getPosition(BodyHandle h) const override;
    void setRotation(BodyHandle h, const glm::quat& rot) override;
    glm::quat getRotation(BodyHandle h) const override;

    // === Body Velocity ===
    void setLinearVelocity(BodyHandle h, const glm::vec3& vel) override;
    glm::vec3 getLinearVelocity(BodyHandle h) const override;
    void setAngularVelocity(BodyHandle h, const glm::vec3& vel) override;
    glm::vec3 getAngularVelocity(BodyHandle h) const override;

    // === Forces ===
    void applyForce(BodyHandle h, const glm::vec3& force, const glm::vec3& relPos) override;
    void applyImpulse(BodyHandle h, const glm::vec3& impulse, const glm::vec3& relPos) override;
    void applyTorque(BodyHandle h, const glm::vec3& torque) override;

    // === Body Properties ===
    void setMotionType(BodyHandle h, MotionType type) override;
    void setMass(BodyHandle h, float mass) override;
    void setFriction(BodyHandle h, float friction) override;
    void setRestitution(BodyHandle h, float restitution) override;
    void setLinearDamping(BodyHandle h, float damping) override;
    void setAngularDamping(BodyHandle h, float damping) override;
    void setGravityEnabled(BodyHandle h, bool enabled) override;
    void setGravityScale(BodyHandle h, float scale) override;

    // === Sleep ===
    void setAwake(BodyHandle h, bool awake) override;
    bool isAwake(BodyHandle h) const override;

    // === Shape ===
    void setShape(BodyHandle h, const ShapeInfo& shape) override;

    // === World Properties ===
    void setGravity(const glm::vec3& gravity) override;
    glm::vec3 getGravity() const override;

    // === CCD ===
    void setCCDEnabled(BodyHandle h, bool enabled) override;

    // === Collision Layers ===
    void setBodyLayer(BodyHandle h, uint16_t layer) override;
    uint16_t getBodyLayer(BodyHandle h) const override;
    void setLayerCollision(uint16_t layer1, uint16_t layer2, bool shouldCollide) override;

    // === Raycasting & Queries ===
    bool castRay(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                 RaycastHit& outHit, uint16_t layerMask = CollisionLayer::ALL_MASK) const override;
    std::vector<RaycastHit> castRayAll(const glm::vec3& origin, const glm::vec3& direction,
                                        float maxDistance, uint16_t layerMask = CollisionLayer::ALL_MASK) const override;
    bool castSphere(const glm::vec3& origin, const glm::vec3& direction, float radius,
                    float maxDistance, ShapeCastResult& outHit,
                    uint16_t layerMask = CollisionLayer::ALL_MASK) const override;
    bool castBox(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& halfExtents,
                 const glm::quat& rotation, float maxDistance, ShapeCastResult& outHit,
                 uint16_t layerMask = CollisionLayer::ALL_MASK) const override;
    std::vector<BodyHandle> overlapSphere(const glm::vec3& center, float radius,
                                           uint16_t layerMask = CollisionLayer::ALL_MASK) const override;
    std::vector<BodyHandle> overlapBox(const glm::vec3& center, const glm::vec3& halfExtents,
                                        const glm::quat& rotation,
                                        uint16_t layerMask = CollisionLayer::ALL_MASK) const override;

    // === Contact Callbacks ===
    void setContactListener(IContactListener* listener) override;
    std::vector<ContactEvent> getContactEvents() override;

    // === Constraints ===
    ConstraintHandle createConstraint(const ConstraintSettings& settings) override;
    void destroyConstraint(ConstraintHandle handle) override;
    bool isValidConstraint(ConstraintHandle handle) const override;
    void setConstraintEnabled(ConstraintHandle handle, bool enabled) override;
    void setConstraintMotorState(ConstraintHandle handle, bool enabled, float speed, float maxForce) override;
    void setConstraintLimits(ConstraintHandle handle, float min, float max) override;
    size_t getConstraintCount() const override;
    void setConstraintBreaking(ConstraintHandle handle, float maxForce, float maxTorque) override;
    std::vector<ConstraintHandle> getAndClearBrokenConstraints() override;

    // === Character Controller ===
    CharacterHandle createCharacter(const CharacterCreationInfo& info) override;
    void destroyCharacter(CharacterHandle handle) override;
    bool isValidCharacter(CharacterHandle handle) const override;
    CharacterState getCharacterState(CharacterHandle handle) const override;
    void setCharacterPosition(CharacterHandle handle, const glm::vec3& pos) override;
    void setCharacterRotation(CharacterHandle handle, const glm::quat& rot) override;
    void setCharacterLinearVelocity(CharacterHandle handle, const glm::vec3& vel) override;
    void updateCharacter(CharacterHandle handle, float deltaTime,
                          const glm::vec3& gravity, const glm::vec3& movementInput) override;
    size_t getCharacterCount() const override;

    // === Info ===
    const char* getName() const override { return "jolt"; }
    BackendStats getStats() const override;

    // === Internal (used by contact listener) ===
    BodyHandle lookupHandle(JPH::BodyID joltId) const;
    void pushContactEvent(const ContactEvent& event);

private:
    // Shape creation
    JPH::ShapeRefC createJoltShape(const ShapeInfo& info) const;

    // Conversions
    static JPH::EMotionType toJoltMotionType(MotionType type);
    static uint16_t autoAssignLayer(MotionType type);

    // Look up Jolt BodyID from our handle
    JPH::BodyID lookupBodyID(BodyHandle handle) const;

    bool m_initialized = false;

    // Jolt core systems
    std::unique_ptr<JPH::PhysicsSystem> m_physicsSystem;
    std::unique_ptr<JPH::TempAllocatorImpl> m_tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> m_jobSystem;

    // Collision filtering (must outlive physics system)
    struct BroadPhaseLayerInterfaceImpl;
    struct ObjectVsBroadPhaseLayerFilterImpl;
    struct ObjectLayerPairFilterImpl;
    struct JoltContactListenerImpl;

    std::unique_ptr<BroadPhaseLayerInterfaceImpl> m_broadPhaseLayerInterface;
    std::unique_ptr<ObjectVsBroadPhaseLayerFilterImpl> m_objectVsBroadPhaseFilter;
    std::unique_ptr<ObjectLayerPairFilterImpl> m_objectLayerPairFilter;
    std::unique_ptr<JoltContactListenerImpl> m_contactListener;

    // Handle -> Jolt BodyID mapping (bidirectional)
    std::unordered_map<BodyHandle, JPH::BodyID> m_handleToBody;
    std::unordered_map<uint32_t, BodyHandle> m_bodyIdToHandle; // JPH BodyID raw -> our handle
    BodyHandle m_nextHandle = 0;

    // Per-body layer tracking
    std::unordered_map<BodyHandle, uint16_t> m_bodyLayers;

    // Constraint management (store type to avoid dynamic_cast — Jolt may disable RTTI)
    struct ConstraintData {
        JPH::Ref<JPH::Constraint> constraint;
        ConstraintType type;
        float breakingForce  = 0.0f; // 0 = disabled
        float breakingTorque = 0.0f; // 0 = disabled
    };
    std::unordered_map<ConstraintHandle, ConstraintData> m_constraints;
    ConstraintHandle m_nextConstraintHandle = 0;

    // Constraints broken during the last step() call (cleared each step)
    std::vector<ConstraintHandle> m_brokenConstraints;

    // Character controller management
    struct CharacterData {
        JPH::Ref<JPH::CharacterVirtual> character;
        uint16_t layer = CollisionLayer::CHARACTER;
    };
    std::unordered_map<CharacterHandle, CharacterData> m_characters;
    CharacterHandle m_nextCharacterHandle = 0;

    // Contact event queue (thread-safe)
    IContactListener* m_userContactListener = nullptr;
    std::mutex m_contactEventsMutex;
    std::vector<ContactEvent> m_contactEvents;

    // Config
    int m_collisionSteps = 1;
};

} // namespace backend
} // namespace physics
} // namespace ohao

#endif // OHAO_HAS_JOLT
