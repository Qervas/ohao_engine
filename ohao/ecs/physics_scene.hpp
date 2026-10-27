#pragma once
// The physics scene (sub-project 5, docs/specs/2026-10-26-level-pipeline-design.md, "Collision
// generation"; stepped since sub-project 6, docs/specs/2026-10-26-player-physics-design.md): a
// Jolt world (physics::backend) holding one body per entity with a Collider (and the body
// entities of ColliderParts), static unless the entity has a RigidBody.
//
//   - The `physics` sync system, right after `transform`, keeps it in step through dirty
//     state, so a still world costs nothing: Collider, RigidBody, ColliderPart, CollisionMesh
//     and MeshSource changes (and destroyed entities) rebuild their body, in creation order;
//     PhysicsMoved (tagged by the transform system) moves a body to its entity.
//   - The `physics` simulate system steps it once per fixed step (step()): propagate dirty
//     transforms, the same sync, Jolt in substeps of at most 1/60 s, every awake dynamic
//     body's pose written back to its entity's Transform, then the step's events.
//   - A dynamic body's entity moved by anything but that write-back (an edit, a restore, a
//     moved parent) teleports the body there, at rest.
//   - Snapshots replay exactly. A snapshot carries the simulation (saveSimulation, through the
//     World's SnapshotHook): the backend's bodies, contact cache and characters, the players and
//     the contact bookkeeping; the next sync after a restore puts it back if the bodies are the
//     snapshot's. When they are not (a body rebuilt since), BodyMotion (runtime, snapshotted:
//     each step writes it for every dynamic body awake in the step, once more as it falls
//     asleep, and for each player whose velocity changed) goes into the bodies after their
//     teleports, and into the players.
//   - Events: contact begin/end per body pair (sub-shape contacts collapsed) and trigger
//     enter/exit (a Collider with trigger is a sensor), drained from the backend's queue on
//     the simulating thread after the step, sorted, and published as the step's span and in
//     a sequenced log of the last kEventLog. Removing a body ends its pairs at once.
//   - Shapes are fitted to the entity's mesh in its own frame and baked with its world scale
//     (fitCollider); a body's position and rotation are its entity's world transform without
//     the scale. A mirrored mesh has its winding flipped, so it still faces out.
//   - A dynamic Mesh collider is built Convex (Jolt does not simulate triangle meshes), with
//     W_LEVEL_DYNAMIC_MESH. ColliderParts (a level's UCX_ hulls) join their body entity's own
//     collider in one compound shape.
//   - The backend is created at the first body, so a world without colliders pays nothing.
//
// No reflection: only components and the backend.
#include "ecs/asset_resolver.hpp"
#include "ecs/components.hpp"
#include "ecs/world.hpp"
#include "physics/backend/physics_backend.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ohao::ecs {

// A mesh's positions and triangles, in its own frame.
struct MeshData {
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;
};

// A collider fitted to a mesh (spec "Fitting from the node's mesh"), in the entity's frame with
// its scale baked in.
struct FittedCollider {
    ColliderShape shape{ColliderShape::Box};
    glm::vec3 halfExtents{0.0f};        // Box
    float radius{0.0f};                 // Sphere, Capsule
    float halfHeight{0.0f};             // Capsule: half the length of its straight part
    glm::quat rotation{1, 0, 0, 0};     // Capsule: from +Y to its axis
    glm::vec3 offset{0.0f};             // the shape's centre
    std::vector<glm::vec3> vertices;    // Convex, Mesh
    std::vector<uint32_t> indices;      // Mesh, wound to face out
    bool roundedScale{false};           // Sphere, Capsule: the scale's axes differ by > 1%; the largest is used
};

// box: the AABB, its centre the offset; sphere: the AABB centre to the farthest vertex;
// capsule: along the AABB's longest axis, the radius the larger other half extent; convex
// and mesh: the vertices (and triangles) scaled. Non-uniform scale is baked into vertices and
// box extents; a sphere or capsule takes the largest axis.
[[nodiscard]] FittedCollider fitCollider(const MeshData& mesh, ColliderShape shape, glm::vec3 scale);

struct RayHit {
    entt::entity entity{entt::null};
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f};
    float dist{0.0f};
};

struct PhysicsWarning {
    std::string code;   // W_LEVEL_DYNAMIC_MESH, W_COLLIDER_NO_MESH, W_COLLIDER_SCALE, W_PHYSICS_PARENT_SCALE
    entt::entity entity{entt::null};
    std::string message;
};

// Something physics decided in one step (spec "Events"), or a player's footstep or landing
// (spec "Footsteps").
struct PhysicsEvent {
    enum class Kind : uint8_t { ContactBegin, ContactEnd, TriggerEnter, TriggerExit, Footstep, Land };
    Kind kind{Kind::ContactBegin};
    // Contacts: a has the lower entity id. Triggers: a is the trigger, b the body. Footsteps and
    // landings: a is the player, b the ground's entity (null when it has none). The entities
    // may be gone by the time the event is read; the ids are kept.
    entt::entity a{entt::null}, b{entt::null};
    uint64_t aId{0}, bId{0};
    glm::vec3 point{0.0f};    // world; an end repeats its begin's; a footstep's or landing's: the feet
    glm::vec3 normal{0.0f};   // world, from a toward b; a footstep's or landing's: the ground's
    // The relative normal speed at a begin or enter, m/s; 0 for an end or exit. A footstep: the
    // player's horizontal speed; a landing: its downward speed on touching the ground.
    float speed{0.0f};
    std::string surface;      // footsteps and landings: the ground's Collider.surface ("default" if none)
    uint64_t step{0};         // the scene's step it was published in (1 is the first); a body removed between steps ends its pairs in the next
    uint64_t seq{0};          // its place in the log, from 1
};
[[nodiscard]] const char* physicsEventKindName(PhysicsEvent::Kind k) noexcept;   // contact_begin, ...

// A body pair in contact now, or a body inside a trigger (physics.contacts; spec "Events"): from
// its begin to its end, sleeping included.
struct PhysicsContact {
    entt::entity a{entt::null}, b{entt::null};   // as its begin event: a has the lower id, or is the trigger
    uint64_t aId{0}, bId{0};
    glm::vec3 point{0.0f}, normal{0.0f};         // the begin's
    bool trigger{false};
    uint64_t since{0};                           // the step it began in
};

// A player as its last step (or placement) left it (player.info).
struct PlayerInfo {
    glm::vec3 pos{0.0f};                // the feet, world
    float yawDeg{0.0f}, pitchDeg{0.0f};
    glm::vec3 velocity{0.0f};           // m/s
    bool onGround{false};
    entt::entity ground{entt::null};    // the body it stands on, when on the ground
    std::string surface;                // that body's Collider.surface ("default" if none); empty in the air
    uint64_t footsteps{0};              // footstep events since it was made
};

struct PhysicsInfo {
    struct BodyCounts {
        uint32_t statics{0}, dynamics{0}, kinematics{0};
    } bodies;
    struct ShapeCounts {
        uint32_t box{0}, sphere{0}, capsule{0}, convex{0}, mesh{0}, compound{0};
    } shapes;
    double buildMs{0.0};    // total time spent building bodies
    uint64_t steps{0};      // physics steps run
    uint32_t awake{0};      // dynamic bodies awake
    uint32_t triggers{0};   // sensor bodies
    uint32_t contacts{0};   // body pairs in contact now (triggers' overlaps included)
    uint32_t players{0};    // PlayerControllers with a character
};

// A player's look after a turn of `delta` look units (the mouse's movement, a look axis): yaw about
// +Y (positive delta.x turns right), pitch up for negative delta.y, kept within 89 degrees, yaw
// wrapped to [-180, 180]. The step turns players with it and the look lead (sub-project 9) draws
// with it, so a frame drawn with a look not yet stepped equals the step that takes it, bit for bit.
struct LookResult {
    float yawDeg{0.0f};
    float pitchDeg{0.0f};
};
[[nodiscard]] LookResult applyLook(float yawDeg, float pitchDeg, glm::vec2 delta, const PlayerController& pc) noexcept;

class PhysicsScene {
public:
    PhysicsScene(entt::registry& reg, AssetResolver& assets);
    ~PhysicsScene();
    PhysicsScene(const PhysicsScene&) = delete;
    PhysicsScene& operator=(const PhysicsScene&) = delete;

    static constexpr std::size_t kEventLog = 1024;   // events kept for eventsSince
    static constexpr int kMaxSubsteps = 8;

    // Builds, moves and removes bodies from the dirty state; no work when nothing changed.
    void sync(entt::registry& reg);
    // One fixed step of dt (the `physics` simulate system): propagate transforms, sync, step in
    // ceil(dt * 60) substeps (at most kMaxSubsteps), write back, publish events. A world with
    // no collider does nothing (not even the propagation).
    void step(World& world, float dt);
    // Characters for PlayerControllers added or changed (the `player_spawn` sync system, before
    // `transform`; a step does it too): a new player at the first SpawnPoint tagged "player",
    // a changed one where it stands.
    void spawnPlayers();
    // RenderPose for what the last step moved (players, their heads, dynamic bodies) and their
    // subtrees, between the last two step poses at alpha; alpha 1 removes them (the `interpolate`
    // sync system, with World::renderAlpha). The look lead (sub-project 9): an input-driven, living
    // player and its subtree are drawn turned by `pendingLook` through applyLook, its turn the
    // newest step's (never slerped) whenever alpha < 1 or a look is pending.
    void interpolate(float alpha, glm::vec2 pendingLook = glm::vec2(0.0f));
    // Whether the last interpolate saw an input-driven, living player (a window measures its look
    // latency by the lead's rule then, and keeps one frame ahead).
    [[nodiscard]] bool leadsLook() const noexcept;
    // The latest step's events, sorted (removals since the step before it first).
    [[nodiscard]] std::span<const PhysicsEvent> stepEvents() const noexcept;
    // Up to max logged events with seq > since, oldest first; dropped is how many of those the
    // log no longer holds.
    [[nodiscard]] std::vector<PhysicsEvent> eventsSince(uint64_t since, std::size_t max, uint64_t& dropped) const;
    // The seq the next event will get (events so far + 1).
    [[nodiscard]] uint64_t nextEventSeq() const noexcept;
    [[nodiscard]] uint64_t steps() const noexcept;
    // Jolt updates the last step ran (its substeps).
    [[nodiscard]] int lastSubsteps() const noexcept;
    // Where the last step's time went (physics_bench reads it).
    struct StepTiming {
        double totalMs{0.0};    // the whole step
        double playerMs{0.0};   // the players (their characters' moves)
        double joltMs{0.0};     // the substeps
        double eventsMs{0.0};   // draining, collapsing, sorting and logging the events
        double writeMs{0.0};    // the write-back
        uint32_t writes{0};     // Transforms written
    };
    [[nodiscard]] StepTiming lastStepTiming() const noexcept;
    // The closest body along the ray within max (dir need not be normalized).
    [[nodiscard]] std::optional<RayHit> raycast(glm::vec3 from, glm::vec3 dir, float max) const;
    // The nearest body along the ray that is neither `ignore`'s (its body, a character's capsule)
    // nor a trigger: a shot or a line of sight from inside the shooter.
    [[nodiscard]] std::optional<RayHit> raycastFrom(glm::vec3 from, glm::vec3 dir, float max, entt::entity ignore) const;
    // Turns a player's view by these angles now (pitch kept within 89 degrees), written as the step
    // writes it (recoil): not a placement, and drawn as part of this step's move. False for an entity
    // without a character.
    bool turnPlayer(entt::entity e, float yawDeg, float pitchDeg);
    [[nodiscard]] PhysicsInfo info() const;
    // The pairs in contact now, by a's id then b's.
    [[nodiscard]] std::vector<PhysicsContact> contacts() const;
    // Entities with a player character, in creation order.
    [[nodiscard]] std::vector<entt::entity> players() const;
    // The player as its last step left it, or nothing for an entity without a character.
    [[nodiscard]] std::optional<PlayerInfo> playerInfo(entt::entity e) const;
    // Puts the player at pos (its feet), at rest, facing yawDeg if given (else as it faced), its
    // ground found there at once: its Transform, head, PlayerState and BodyMotion are written now.
    // False for an entity without a character.
    bool teleportPlayer(entt::entity e, glm::vec3 pos, std::optional<float> yawDeg);
    // The simulation a snapshot carries (installPhysics puts these in the World's SnapshotHook):
    // the backend's bodies, contact cache and characters, the players, the contact bookkeeping.
    // A restore takes effect at the next sync, if the bodies are the snapshot's (the same
    // entities' bodies); else the components' path (teleports at rest, BodyMotion) applies.
    [[nodiscard]] std::shared_ptr<const void> saveSimulation() const;
    void restoreSimulation(const std::shared_ptr<const void>& state);
    // The backend, created on first use; tests step it.
    [[nodiscard]] physics::backend::IPhysicsBackend& backend();
    // The entity's body, or INVALID_BODY.
    [[nodiscard]] physics::backend::BodyHandle bodyOf(entt::entity e) const;
    [[nodiscard]] const std::vector<PhysicsWarning>& warnings() const noexcept;
    // Bodies built so far (each rebuild counts): a still world adds none.
    [[nodiscard]] std::size_t bodyBuilds() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Adds the sync systems "player_spawn" right before "transform", "physics" right after it and
// "interpolate" after that (before "render_extract"), the "physics" simulate system after those
// added so far, and returns the scene (once per World; a second call returns the first). The
// resolver must outlive the World.
PhysicsScene& installPhysics(World& world, AssetResolver& assets);
// The World's physics scene, or null without installPhysics.
[[nodiscard]] PhysicsScene* physicsScene(World& world);
[[nodiscard]] const PhysicsScene* physicsScene(const World& world);
// The latest physics step's events (empty without installPhysics): what a simulate system
// added after installPhysics reads in the same step.
[[nodiscard]] std::span<const PhysicsEvent> physicsEvents(const World& world);

} // namespace ohao::ecs
