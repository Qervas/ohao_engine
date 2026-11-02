#pragma once

/**
 * Physics Backend Interface - Plugin architecture for physics engines
 *
 * Design: Microservice pattern - if the backend fails, the engine still runs.
 * - IPhysicsBackend: abstract contract any physics engine must implement
 * - NullPhysicsBackend: graceful fallback (no physics, but no crashes)
 * - JoltPhysicsBackend: production backend (Jolt Physics, MIT license)
 *
 * All external APIs (OhaoPhysicsBody, GDScript) are unchanged.
 * PhysicsWorld delegates to whichever backend is active.
 *
 * Features:
 *   - Rigid body lifecycle, transform, velocity, forces, properties, sleep, shape
 *   - CCD (Continuous Collision Detection) per body
 *   - 16-layer collision filtering with configurable pair matrix
 *   - Raycasting and shape queries (sphere/box overlap, shape casts)
 *   - Contact callbacks (begin/persist/end) with thread-safe event queue
 *   - Constraints/Joints (Fixed, Point, Hinge, Slider, Cone, Distance, SixDOF)
 *   - Heightfield terrain shapes
 *   - Character controller (CharacterVirtual-style, kinematic)
 */

#include "core/concepts.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <memory>
#include <cstdint>
#include <functional>

namespace ohao {
namespace physics {

// Forward declarations
struct PhysicsWorldConfig;

namespace backend {

// ============================================================================
// Handle types
// ============================================================================

using BodyHandle = uint32_t;
constexpr BodyHandle INVALID_BODY = UINT32_MAX;

using ConstraintHandle = uint32_t;
constexpr ConstraintHandle INVALID_CONSTRAINT = UINT32_MAX;

using CharacterHandle = uint32_t;
constexpr CharacterHandle INVALID_CHARACTER = UINT32_MAX;

[[nodiscard]] constexpr bool isValidBody(BodyHandle h) noexcept { return h != INVALID_BODY; }
[[nodiscard]] constexpr bool isValidConstraint(ConstraintHandle h) noexcept {
    return h != INVALID_CONSTRAINT;
}
[[nodiscard]] constexpr bool isValidCharacter(CharacterHandle h) noexcept {
    return h != INVALID_CHARACTER;
}

// ============================================================================
// Collision Layers (16-layer system)
// ============================================================================

namespace CollisionLayer {
    constexpr uint16_t DEFAULT     = 0;
    constexpr uint16_t STATIC      = 1;
    constexpr uint16_t DYNAMIC     = 2;
    constexpr uint16_t KINEMATIC   = 3;
    constexpr uint16_t CHARACTER   = 4;
    constexpr uint16_t TRIGGER     = 5;
    constexpr uint16_t DEBRIS      = 6;
    constexpr uint16_t PROJECTILE  = 7;
    constexpr uint16_t VEHICLE     = 8;
    constexpr uint16_t RAGDOLL     = 9;
    constexpr uint16_t TERRAIN     = 10;
    constexpr uint16_t WATER       = 11;
    constexpr uint16_t USER_0      = 12;
    constexpr uint16_t USER_1      = 13;
    constexpr uint16_t USER_2      = 14;
    constexpr uint16_t USER_3      = 15;
    constexpr uint16_t NUM_LAYERS  = 16;
    constexpr uint16_t ALL_MASK    = 0xFFFF;
}

// ============================================================================
// Shape description (backend-agnostic)
// ============================================================================

struct CompoundChild;

struct ShapeInfo {
    enum class Type {
        BOX,
        SPHERE,
        CAPSULE,
        CYLINDER,
        PLANE,
        MESH,
        HEIGHTFIELD,
        CONVEX_HULL,
        COMPOUND
    };

    Type type = Type::BOX;

    // BOX
    glm::vec3 halfExtents{0.5f};

    // SPHERE, CAPSULE, CYLINDER
    float radius = 0.5f;

    // CAPSULE, CYLINDER
    float height = 1.0f;

    // PLANE
    glm::vec3 planeNormal{0.0f, 1.0f, 0.0f};
    float planeDistance = 0.0f;

    // MESH / CONVEX_HULL (raw pointers, caller owns data lifetime during createBody)
    const glm::vec3* meshVertices = nullptr;
    uint32_t meshVertexCount = 0;
    const uint32_t* meshIndices = nullptr;
    uint32_t meshIndexCount = 0;

    // HEIGHTFIELD (row-major height samples)
    const float* heightfieldData = nullptr;
    uint32_t heightfieldSizeX = 0;
    uint32_t heightfieldSizeZ = 0;
    glm::vec3 heightfieldScale{1.0f, 1.0f, 1.0f}; // x, y(vertical), z
    float heightfieldMinHeight = 0.0f;
    float heightfieldMaxHeight = 1.0f;

    // COMPOUND: child shapes, each placed in the body's frame (one rigid shape; a static
    // compound in Jolt). A child is any other type, a compound included. A single child with
    // an offset is how a shape away from the body's origin is made.
    std::vector<CompoundChild> children;

    // Span convenience (sets raw pointers; caller keeps storage alive)
    void setMesh(std::span<const glm::vec3> vertices, std::span<const uint32_t> indices) noexcept {
        meshVertices = vertices.data();
        meshVertexCount = static_cast<uint32_t>(vertices.size());
        meshIndices = indices.data();
        meshIndexCount = static_cast<uint32_t>(indices.size());
        type = Type::MESH;
    }

    void setHeightfield(std::span<const float> heights, uint32_t sizeX, uint32_t sizeZ) noexcept {
        heightfieldData = heights.data();
        heightfieldSizeX = sizeX;
        heightfieldSizeZ = sizeZ;
        type = Type::HEIGHTFIELD;
    }

    [[nodiscard]] std::span<const glm::vec3> meshVertexSpan() const noexcept {
        return {meshVertices, meshVertexCount};
    }
    [[nodiscard]] std::span<const uint32_t> meshIndexSpan() const noexcept {
        return {meshIndices, meshIndexCount};
    }
};

struct CompoundChild {
    ShapeInfo shape;
    glm::vec3 position{0.0f};          // in the body's frame
    glm::quat rotation{1, 0, 0, 0};
};

// ============================================================================
// Motion type
// ============================================================================

enum class MotionType {
    DYNAMIC,
    STATIC,
    KINEMATIC
};

// ============================================================================
// Body creation info
// ============================================================================

struct BodyCreationInfo {
    glm::vec3 position{0.0f};
    glm::quat rotation{1, 0, 0, 0};
    MotionType motionType = MotionType::DYNAMIC;
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.3f;
    float linearDamping = 0.01f;
    float angularDamping = 0.05f;
    bool gravityEnabled = true;
    float gravityScale = 1.0f;  // Multiplier on world gravity (0=no gravity, 0.5=half, 2=double)
    ShapeInfo shape;

    // CCD - prevents fast objects from tunneling through thin surfaces
    bool useCCD = false;

    // A sensor: reports contacts (to the contact listener) and blocks nothing
    bool isSensor = false;

    // Collision layer (0 = auto-assign based on motionType)
    uint16_t layer = 0;
};

// ============================================================================
// Raycast / Query results
// ============================================================================

struct RaycastHit {
    BodyHandle bodyHandle = INVALID_BODY;
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float fraction = 1.0f; // 0..1 along the ray
    uint16_t layer = 0;

    [[nodiscard]] bool hit() const noexcept { return isValidBody(bodyHandle); }
    [[nodiscard]] explicit operator bool() const noexcept { return hit(); }
};

struct ShapeCastResult {
    BodyHandle bodyHandle = INVALID_BODY;
    glm::vec3 contactPoint{0.0f};
    glm::vec3 contactNormal{0.0f, 1.0f, 0.0f};
    float fraction = 1.0f;
};

// ============================================================================
// Contact events
// ============================================================================

struct ContactEvent {
    enum class Type { BEGIN, PERSIST, END };
    Type type = Type::BEGIN;
    BodyHandle body1 = INVALID_BODY;
    BodyHandle body2 = INVALID_BODY;
    glm::vec3 contactPoint{0.0f};
    glm::vec3 contactNormal{0.0f};
    float penetrationDepth = 0.0f;
    glm::vec3 relativeVelocity{0.0f};
};

class IContactListener {
public:
    virtual ~IContactListener() = default;
    virtual void onContactBegin(const ContactEvent& event) = 0;
    virtual void onContactPersist(const ContactEvent& event) = 0;
    virtual void onContactEnd(const ContactEvent& event) = 0;
};

// ============================================================================
// Constraint settings
// ============================================================================

enum class ConstraintType {
    FIXED,
    POINT,        // Ball-and-socket
    HINGE,
    SLIDER,       // Prismatic
    SWING_TWIST,  // Ragdoll joint: a swing cone about the twist axis and a twist range
    DISTANCE,
    SIX_DOF
};

struct ConstraintSettings {
    ConstraintType type = ConstraintType::FIXED;
    BodyHandle body1 = INVALID_BODY;
    BodyHandle body2 = INVALID_BODY; // INVALID_BODY = attach to world

    // Where anchors and axes are given: Local, each in its own body's space (its origin and rotation,
    // not its centre of mass; body2's in the world's when body2 is INVALID_BODY), or World.
    enum class Space { Local, World };
    Space space = Space::Local;

    // Anchor points
    glm::vec3 anchor1{0.0f};
    glm::vec3 anchor2{0.0f};

    // Axis (hinge axis, slider axis, swing-twist's twist axis)
    glm::vec3 axis1{0.0f, 1.0f, 0.0f};
    glm::vec3 axis2{0.0f, 1.0f, 0.0f};

    // Limits (hinge: radians, slider: meters, swing-twist: its twist, radians)
    bool enableLimits = false;
    float limitMin = 0.0f;
    float limitMax = 0.0f;

    // Motor
    bool enableMotor = false;
    float motorSpeed = 0.0f;        // Target velocity (rad/s or m/s)
    float motorMaxForce = 1000.0f;  // Maximum force/torque

    // Spring (distance constraint or soft limits)
    bool enableSpring = false;
    float springFrequency = 1.0f;   // Hz
    float springDamping = 0.1f;     // 0..1

    // Swing-twist: the swing cone's half angles about the axis normal to `axis` and in its plane
    // (radians); the twist range is limitMin..limitMax (always limited)
    float swingNormalHalfCone = 0.0f;
    float swingPlaneHalfCone = 0.0f;

    // Distance constraint specific
    float minDistance = 0.0f;
    float maxDistance = -1.0f; // -1 = auto-detect from current positions

    // Whether the two bodies still collide with each other (false: their contacts are rejected while
    // the constraint exists, as PhysX and Unity do; a ragdoll's neighbouring parts overlap at the joint)
    bool collideConnected = false;

    // Solver iterations for the island this constraint is in (0: the system's default); the island
    // runs the largest of its bodies' and constraints'. Long chains and ragdolls need more.
    uint32_t velocitySteps = 0;
    uint32_t positionSteps = 0;

    // Breaking threshold (0 = unbreakable): a step whose impulse passes it disables the constraint
    // and reports it (getAndClearBrokenConstraints); only destroyConstraint removes one
    float breakingForce = 0.0f;
    float breakingTorque = 0.0f;

    // SixDOF specific: per-axis freedom
    // Each axis: 0 = free, 1 = fixed, 2 = limited
    int axisMode[6] = {0, 0, 0, 0, 0, 0}; // TX, TY, TZ, RX, RY, RZ
    float axisLimitMin[6] = {0, 0, 0, 0, 0, 0};
    float axisLimitMax[6] = {0, 0, 0, 0, 0, 0};
};

// ============================================================================
// Character controller
// ============================================================================

struct CharacterCreationInfo {
    glm::vec3 position{0.0f};
    glm::quat rotation{1, 0, 0, 0};

    // Capsule dimensions
    float capsuleRadius = 0.3f;
    float capsuleHeight = 1.8f; // Total height including hemispheres

    // Movement parameters
    float maxSlopeAngleDeg = 50.0f;
    float mass = 80.0f;
    float maxStrength = 100.0f;
    float friction = 0.5f;
    float gravityFactor = 1.0f;

    // Collision layer
    uint16_t layer = CollisionLayer::CHARACTER;

    // The character's position is its feet (the capsule stands on it) instead of the capsule's
    // centre, and only contacts below the lower hemisphere's centre support it.
    bool positionAtFeet = false;
    // A kinematic inner body of the same capsule on `layer`, so sensors and rays see the
    // character (it is not a rigid body otherwise). getCharacterInnerBody gives its handle.
    bool innerBody = false;
};

enum class GroundState {
    ON_GROUND,
    ON_STEEP_GROUND,
    NOT_SUPPORTED,
    IN_AIR
};

struct CharacterState {
    glm::vec3 position{0.0f};
    glm::quat rotation{1, 0, 0, 0};
    glm::vec3 linearVelocity{0.0f};
    GroundState groundState = GroundState::IN_AIR;
    glm::vec3 groundNormal{0.0f, 1.0f, 0.0f};
    BodyHandle groundBody = INVALID_BODY;
};

// ============================================================================
// Backend statistics
// ============================================================================

struct BackendStats {
    size_t totalBodies = 0;
    size_t activeBodies = 0;
    size_t sleepingBodies = 0;
    size_t constraintCount = 0;
    size_t characterCount = 0;
    size_t contactEventCount = 0;
    float stepTimeMs = 0.0f;
};

// ============================================================================
// Abstract physics backend interface
// ============================================================================

class IPhysicsBackend {
public:
    virtual ~IPhysicsBackend() = default;

    // === LIFECYCLE ===
    [[nodiscard]] virtual bool initialize(const PhysicsWorldConfig& config) = 0;
    virtual void shutdown() = 0;
    [[nodiscard]] virtual bool isInitialized() const = 0;

    // === SIMULATION ===
    virtual void step(float deltaTime) = 0;

    // === BODY MANAGEMENT ===
    [[nodiscard]] virtual BodyHandle createBody(const BodyCreationInfo& info) = 0;
    virtual void destroyBody(BodyHandle handle) = 0;
    [[nodiscard]] virtual bool isValidBody(BodyHandle handle) const = 0;

    // === BODY TRANSFORM ===
    virtual void setPosition(BodyHandle h, const glm::vec3& pos) = 0;
    virtual glm::vec3 getPosition(BodyHandle h) const = 0;
    virtual void setRotation(BodyHandle h, const glm::quat& rot) = 0;
    virtual glm::quat getRotation(BodyHandle h) const = 0;
    // Position and rotation at once (a teleport): the body lands exactly where a new body made
    // there would be, which setPosition then setRotation does not promise (the second re-derives
    // the position through the centre of mass).
    virtual void setPose(BodyHandle h, const glm::vec3& pos, const glm::quat& rot) = 0;

    // === SIMULATION STATE (snapshots) ===
    // Everything the next step depends on: every body's pose, velocities and sleep, the contact
    // cache (the impulses the solver warm-starts with), each character, every constraint's enabled
    // flag. restoreState needs the same bodies, characters and constraints as at saveState (the
    // caller checks the bodies; a character set or a constraint list, in the solver's order, that
    // differs is refused before anything changes) and returns false otherwise.
    [[nodiscard]] virtual std::string saveState() const = 0;
    virtual bool restoreState(const std::string& state) = 0;

    // === BODY VELOCITY ===
    virtual void setLinearVelocity(BodyHandle h, const glm::vec3& vel) = 0;
    virtual glm::vec3 getLinearVelocity(BodyHandle h) const = 0;
    virtual void setAngularVelocity(BodyHandle h, const glm::vec3& vel) = 0;
    virtual glm::vec3 getAngularVelocity(BodyHandle h) const = 0;

    // === FORCES ===
    virtual void applyForce(BodyHandle h, const glm::vec3& force, const glm::vec3& relPos) = 0;
    virtual void applyImpulse(BodyHandle h, const glm::vec3& impulse, const glm::vec3& relPos) = 0;
    virtual void applyTorque(BodyHandle h, const glm::vec3& torque) = 0;

    // === BODY PROPERTIES ===
    virtual void setMotionType(BodyHandle h, MotionType type) = 0;
    virtual void setMass(BodyHandle h, float mass) = 0;
    virtual void setFriction(BodyHandle h, float friction) = 0;
    virtual void setRestitution(BodyHandle h, float restitution) = 0;
    virtual void setLinearDamping(BodyHandle h, float damping) = 0;
    virtual void setAngularDamping(BodyHandle h, float damping) = 0;
    virtual void setGravityEnabled(BodyHandle h, bool enabled) = 0;
    virtual void setGravityScale(BodyHandle h, float scale) = 0;

    // === SLEEP ===
    virtual void setAwake(BodyHandle h, bool awake) = 0;
    [[nodiscard]] virtual bool isAwake(BodyHandle h) const = 0;

    // === SHAPE ===
    virtual void setShape(BodyHandle h, const ShapeInfo& shape) = 0;

    // === WORLD PROPERTIES ===
    virtual void setGravity(const glm::vec3& gravity) = 0;
    virtual glm::vec3 getGravity() const = 0;

    // === CCD ===
    virtual void setCCDEnabled(BodyHandle h, bool enabled) = 0;

    // === COLLISION LAYERS ===
    virtual void setBodyLayer(BodyHandle h, uint16_t layer) = 0;
    virtual uint16_t getBodyLayer(BodyHandle h) const = 0;
    virtual void setLayerCollision(uint16_t layer1, uint16_t layer2, bool shouldCollide) = 0;

    // === RAYCASTING & QUERIES ===
    [[nodiscard]] virtual bool castRay(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                         RaycastHit& outHit, uint16_t layerMask = CollisionLayer::ALL_MASK) const = 0;
    [[nodiscard]] virtual std::vector<RaycastHit> castRayAll(const glm::vec3& origin, const glm::vec3& direction,
                                                float maxDistance, uint16_t layerMask = CollisionLayer::ALL_MASK) const = 0;
    [[nodiscard]] virtual bool castSphere(const glm::vec3& origin, const glm::vec3& direction, float radius,
                            float maxDistance, ShapeCastResult& outHit,
                            uint16_t layerMask = CollisionLayer::ALL_MASK) const = 0;
    [[nodiscard]] virtual bool castBox(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& halfExtents,
                         const glm::quat& rotation, float maxDistance, ShapeCastResult& outHit,
                         uint16_t layerMask = CollisionLayer::ALL_MASK) const = 0;
    [[nodiscard]] virtual std::vector<BodyHandle> overlapSphere(const glm::vec3& center, float radius,
                                                   uint16_t layerMask = CollisionLayer::ALL_MASK) const = 0;
    [[nodiscard]] virtual std::vector<BodyHandle> overlapBox(const glm::vec3& center, const glm::vec3& halfExtents,
                                                const glm::quat& rotation,
                                                uint16_t layerMask = CollisionLayer::ALL_MASK) const = 0;

    // === CONTACT CALLBACKS ===
    virtual void setContactListener(IContactListener* listener) = 0;
    [[nodiscard]] virtual std::vector<ContactEvent> getContactEvents() = 0;

    // === CONSTRAINTS ===
    [[nodiscard]] virtual ConstraintHandle createConstraint(const ConstraintSettings& settings) = 0;
    virtual void destroyConstraint(ConstraintHandle handle) = 0;
    [[nodiscard]] virtual bool isValidConstraint(ConstraintHandle handle) const = 0;
    virtual void setConstraintEnabled(ConstraintHandle handle, bool enabled) = 0;
    [[nodiscard]] virtual bool isConstraintEnabled(ConstraintHandle handle) const = 0;
    virtual void setConstraintMotorState(ConstraintHandle handle, bool enabled, float speed, float maxForce) = 0;
    virtual void setConstraintLimits(ConstraintHandle handle, float min, float max) = 0;
    virtual size_t getConstraintCount() const = 0;
    // Breaking thresholds: impulse (N·s) per step; 0 = disabled
    virtual void setConstraintBreaking(ConstraintHandle handle, float maxForce, float maxTorque) = 0;
    virtual std::vector<ConstraintHandle> getAndClearBrokenConstraints() = 0;

    // === CHARACTER CONTROLLER ===
    [[nodiscard]] virtual CharacterHandle createCharacter(const CharacterCreationInfo& info) = 0;
    virtual void destroyCharacter(CharacterHandle handle) = 0;
    [[nodiscard]] virtual bool isValidCharacter(CharacterHandle handle) const = 0;
    virtual CharacterState getCharacterState(CharacterHandle handle) const = 0;
    // A teleport: its contacts and ground are found where it now is (a new character's too).
    virtual void setCharacterPosition(CharacterHandle handle, const glm::vec3& pos) = 0;
    virtual void setCharacterRotation(CharacterHandle handle, const glm::quat& rot) = 0;
    virtual void setCharacterLinearVelocity(CharacterHandle handle, const glm::vec3& vel) = 0;
    virtual void updateCharacter(CharacterHandle handle, float deltaTime,
                                  const glm::vec3& gravity, const glm::vec3& movementInput) = 0;
    // Moves the character with exactly this velocity for deltaTime (no gravity added): sliding
    // along what it hits, walking up steps of at most stepUp, sticking to the floor down to
    // stickDown when it was on ground, pushing dynamic bodies with its strength.
    virtual void moveCharacter(CharacterHandle handle, float deltaTime, const glm::vec3& velocity,
                               const glm::vec3& gravity, float stepUp, float stickDown) = 0;
    // The character's inner body (CharacterCreationInfo::innerBody), or INVALID_BODY.
    [[nodiscard]] virtual BodyHandle getCharacterInnerBody(CharacterHandle handle) const = 0;
    // A character out of the world: its inner body leaves the broadphase (rays, bodies and other
    // characters pass where it was); it keeps its handles and state, and saveState/restoreState
    // carry it, so it can come back as the same character.
    virtual void setCharacterInWorld(CharacterHandle handle, bool inWorld) = 0;
    virtual size_t getCharacterCount() const = 0;

    // A kinematic body set moving so that it reaches this pose after deltaTime (its velocity
    // pushes what it meets); setLinearVelocity/setAngularVelocity to zero stop it.
    virtual void moveKinematic(BodyHandle h, const glm::vec3& pos, const glm::quat& rot, float deltaTime) = 0;

    // === INFO ===
    virtual const char* getName() const = 0;
    virtual BackendStats getStats() const = 0;
};

// ============================================================================
// Null physics backend - graceful degradation
// ============================================================================

class NullPhysicsBackend : public IPhysicsBackend {
public:
    [[nodiscard]] bool initialize(const PhysicsWorldConfig&) override { m_initialized = true; return true; }
    void shutdown() override { m_initialized = false; }
    [[nodiscard]] bool isInitialized() const override { return m_initialized; }

    void step(float) override {}

    [[nodiscard]] BodyHandle createBody(const BodyCreationInfo&) override { return m_nextHandle++; }
    void destroyBody(BodyHandle) override {}
    [[nodiscard]] bool isValidBody(BodyHandle h) const override { return h < m_nextHandle; }

    void setPosition(BodyHandle, const glm::vec3&) override {}
    glm::vec3 getPosition(BodyHandle) const override { return glm::vec3(0.0f); }
    void setRotation(BodyHandle, const glm::quat&) override {}
    void setPose(BodyHandle, const glm::vec3&, const glm::quat&) override {}
    [[nodiscard]] std::string saveState() const override { return {}; }
    bool restoreState(const std::string&) override { return false; }
    glm::quat getRotation(BodyHandle) const override { return glm::quat(1, 0, 0, 0); }

    void setLinearVelocity(BodyHandle, const glm::vec3&) override {}
    glm::vec3 getLinearVelocity(BodyHandle) const override { return glm::vec3(0.0f); }
    void setAngularVelocity(BodyHandle, const glm::vec3&) override {}
    glm::vec3 getAngularVelocity(BodyHandle) const override { return glm::vec3(0.0f); }

    void applyForce(BodyHandle, const glm::vec3&, const glm::vec3&) override {}
    void applyImpulse(BodyHandle, const glm::vec3&, const glm::vec3&) override {}
    void applyTorque(BodyHandle, const glm::vec3&) override {}

    void setMotionType(BodyHandle, MotionType) override {}
    void setMass(BodyHandle, float) override {}
    void setFriction(BodyHandle, float) override {}
    void setRestitution(BodyHandle, float) override {}
    void setLinearDamping(BodyHandle, float) override {}
    void setAngularDamping(BodyHandle, float) override {}
    void setGravityEnabled(BodyHandle, bool) override {}
    void setGravityScale(BodyHandle, float) override {}

    void setAwake(BodyHandle, bool) override {}
    [[nodiscard]] bool isAwake(BodyHandle) const override { return false; }

    void setShape(BodyHandle, const ShapeInfo&) override {}

    void setGravity(const glm::vec3&) override {}
    glm::vec3 getGravity() const override { return glm::vec3(0, -9.81f, 0); }

    // CCD
    void setCCDEnabled(BodyHandle, bool) override {}

    // Collision layers
    void setBodyLayer(BodyHandle, uint16_t) override {}
    uint16_t getBodyLayer(BodyHandle) const override { return 0; }
    void setLayerCollision(uint16_t, uint16_t, bool) override {}

    // Raycasting
    [[nodiscard]] bool castRay(const glm::vec3&, const glm::vec3&, float, RaycastHit&, uint16_t) const override { return false; }
    [[nodiscard]] std::vector<RaycastHit> castRayAll(const glm::vec3&, const glm::vec3&, float, uint16_t) const override { return {}; }
    [[nodiscard]] bool castSphere(const glm::vec3&, const glm::vec3&, float, float, ShapeCastResult&, uint16_t) const override { return false; }
    [[nodiscard]] bool castBox(const glm::vec3&, const glm::vec3&, const glm::vec3&, const glm::quat&, float, ShapeCastResult&, uint16_t) const override { return false; }
    [[nodiscard]] std::vector<BodyHandle> overlapSphere(const glm::vec3&, float, uint16_t) const override { return {}; }
    [[nodiscard]] std::vector<BodyHandle> overlapBox(const glm::vec3&, const glm::vec3&, const glm::quat&, uint16_t) const override { return {}; }

    // Contacts
    void setContactListener(IContactListener*) override {}
    [[nodiscard]] std::vector<ContactEvent> getContactEvents() override { return {}; }

    // Constraints
    [[nodiscard]] ConstraintHandle createConstraint(const ConstraintSettings&) override { return INVALID_CONSTRAINT; }
    void destroyConstraint(ConstraintHandle) override {}
    [[nodiscard]] bool isValidConstraint(ConstraintHandle) const override { return false; }
    void setConstraintEnabled(ConstraintHandle, bool) override {}
    [[nodiscard]] bool isConstraintEnabled(ConstraintHandle) const override { return false; }
    void setConstraintMotorState(ConstraintHandle, bool, float, float) override {}
    void setConstraintLimits(ConstraintHandle, float, float) override {}
    size_t getConstraintCount() const override { return 0; }
    void setConstraintBreaking(ConstraintHandle, float, float) override {}
    std::vector<ConstraintHandle> getAndClearBrokenConstraints() override { return {}; }

    // Character controller
    [[nodiscard]] CharacterHandle createCharacter(const CharacterCreationInfo&) override { return INVALID_CHARACTER; }
    void destroyCharacter(CharacterHandle) override {}
    [[nodiscard]] bool isValidCharacter(CharacterHandle) const override { return false; }
    CharacterState getCharacterState(CharacterHandle) const override { return {}; }
    void setCharacterPosition(CharacterHandle, const glm::vec3&) override {}
    void setCharacterRotation(CharacterHandle, const glm::quat&) override {}
    void setCharacterLinearVelocity(CharacterHandle, const glm::vec3&) override {}
    void updateCharacter(CharacterHandle, float, const glm::vec3&, const glm::vec3&) override {}
    void moveCharacter(CharacterHandle, float, const glm::vec3&, const glm::vec3&, float, float) override {}
    [[nodiscard]] BodyHandle getCharacterInnerBody(CharacterHandle) const override { return INVALID_BODY; }
    void setCharacterInWorld(CharacterHandle, bool) override {}
    size_t getCharacterCount() const override { return 0; }
    void moveKinematic(BodyHandle, const glm::vec3&, const glm::quat&, float) override {}

    const char* getName() const override { return "null"; }
    BackendStats getStats() const override { return {}; }

private:
    bool m_initialized = false;
    BodyHandle m_nextHandle = 0;
};

/**
 * Factory function - creates the best available backend.
 * Tries Jolt first, falls back to null if unavailable.
 */
[[nodiscard]] std::unique_ptr<IPhysicsBackend> createPhysicsBackend(std::string_view preferred = "jolt");

} // namespace backend
} // namespace physics
} // namespace ohao
