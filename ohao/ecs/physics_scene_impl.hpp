// PhysicsScene::Impl: the physics scene's state and its member functions' declarations. Private to
// ohao/ecs/physics_scene*.cpp: physics_scene.cpp (bodies, sync, snapshots, the public API),
// physics_scene_joints.cpp, physics_scene_step.cpp, physics_scene_players.cpp.
#pragma once

#include "ecs/physics_scene.hpp"

#include "core/log.hpp"
#include "ecs/instantiate.hpp"
#include "ecs/snapshot_hook.hpp"
#include "ecs/transform_system.hpp"
#include "asset/model.hpp"

#include <ankerl/unordered_dense.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <utility>



namespace ohao::ecs {

namespace pb = physics::backend;

// Helpers the Impl's files share (each its own internal copy).
namespace {

struct PhysicsInstalled {
    PhysicsScene* scene{nullptr};
};

// A world matrix as translation, rotation and (signed) scale: a mirror shows as a negative x.
struct Placement {
    glm::vec3 position{0.0f};
    glm::quat rotation{1, 0, 0, 0};
    glm::vec3 scale{1.0f};
};

Placement decompose(const glm::mat4& m) {
    Placement p;
    p.position = glm::vec3(m[3]);
    glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    p.scale = {glm::length(c0), glm::length(c1), glm::length(c2)};
    if (glm::determinant(glm::mat3(m)) < 0.0f) p.scale.x = -p.scale.x;
    const auto unit = [](glm::vec3 c, float s) { return std::fabs(s) > 1e-12f ? c / s : c; };
    p.rotation = glm::normalize(glm::quat_cast(glm::mat3(unit(c0, p.scale.x), unit(c1, p.scale.y), unit(c2, p.scale.z))));
    return p;
}

bool differs(glm::vec3 a, glm::vec3 b, float eps) {
    return std::fabs(a.x - b.x) > eps || std::fabs(a.y - b.y) > eps || std::fabs(a.z - b.z) > eps;
}

float maxAbs(glm::vec3 v) { return std::max({std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}); }

// The scale's axes differ by more than 1%.
bool nonUniform(glm::vec3 s) {
    const glm::vec3 a = glm::abs(s);
    return maxAbs(s) - std::min({a.x, a.y, a.z}) > 0.01f * maxAbs(s);
}

// The same rotation within eps per component (q and -q are the same rotation).
bool sameRotation(glm::quat a, glm::quat b, float eps) {
    if (glm::dot(a, b) < 0.0f) b = -b;
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.z - b.z) <= eps &&
           std::fabs(a.w - b.w) <= eps;
}

uint64_t pairKey(pb::BodyHandle x, pb::BodyHandle y) {
    const pb::BodyHandle lo = std::min(x, y), hi = std::max(x, y);
    return (uint64_t(lo) << 32) | hi;
}

// Begins and enters sort before ends and exits within a step; a player's footsteps and landings
// after both.
int kindRank(PhysicsEvent::Kind k) {
    switch (k) {
        case PhysicsEvent::Kind::ContactBegin:
        case PhysicsEvent::Kind::TriggerEnter: return 0;
        case PhysicsEvent::Kind::ContactEnd:
        case PhysicsEvent::Kind::TriggerExit: return 1;
        case PhysicsEvent::Kind::Footstep:
        case PhysicsEvent::Kind::Land: return 2;
        case PhysicsEvent::Kind::JointBreak: return 3;
    }
    return 4;
}

pb::MotionType toBackend(Motion m) {
    switch (m) {
        case Motion::Dynamic: return pb::MotionType::DYNAMIC;
        case Motion::Kinematic: return pb::MotionType::KINEMATIC;
        case Motion::Static: break;
    }
    return pb::MotionType::STATIC;
}

} // namespace

struct PhysicsScene::Impl {
    struct BodyRecord {
        pb::BodyHandle handle{pb::INVALID_BODY};
        Motion motion{Motion::Static};
        enum class Kind : uint8_t { Box, Sphere, Capsule, Convex, Mesh, Compound } kind{Kind::Box};
        glm::vec3 scale{1.0f};   // the world scale the shapes were baked with
        bool hasParts{false};
        bool trigger{false};     // a sensor
        // Dynamic bodies: the world pose last written to the entity (or the body was placed at),
        // so physics' own write-back is told apart from a teleport; awake after the last step.
        glm::vec3 wpos{0.0f};
        glm::quat wrot{1, 0, 0, 0};
        bool awake{true};
    };
    // A body pair in contact (or a body inside a trigger): its sub-shape contacts, who is a and b.
    struct Pair {
        uint32_t count{0};
        bool trigger{false};
        // Jolt removes a sleeping body's contacts (and adds them again when it wakes): such a
        // pair stays, dormant, until its bodies wake apart.
        bool dormant{false};
        entt::entity a{entt::null}, b{entt::null};
        uint64_t aId{0}, bId{0};
        glm::vec3 point{0.0f}, normal{0.0f};
        uint64_t since{0};   // the step it began in
    };

    entt::registry& reg;
    AssetResolver& assets;
    std::unique_ptr<pb::IPhysicsBackend> backend;
    ankerl::unordered_dense::map<entt::entity, BodyRecord> bodies;
    ankerl::unordered_dense::map<pb::BodyHandle, entt::entity> entityOf;
    ankerl::unordered_dense::map<entt::entity, entt::entity> partOf;   // part -> the body it was built into
    std::vector<entt::entity> touched;
    std::vector<PhysicsWarning> warnings;
    std::set<std::pair<entt::entity, std::string>> warned;
    double buildMs{0.0};
    std::size_t builds{0};
    bool removing{false};
    // Events (spec "Events").
    ankerl::unordered_dense::map<uint64_t, Pair> pairs;
    std::vector<PhysicsEvent> pending;      // ends from bodies removed since the last step
    std::vector<PhysicsEvent> stepEvents;   // the latest step's, published
    std::vector<PhysicsEvent> log;          // a ring of kEventLog, by seq
    std::size_t dormant{0};                 // pairs kept through their bodies' sleep
    uint64_t nextSeq{1};
    uint64_t steps{0};
    int lastSubsteps{0};
    bool warnedLongStep{false};
    PhysicsScene::StepTiming timing;
    float stepDt{0.0f};                            // the step's dt while it runs; 0 outside a step
    std::vector<pb::BodyHandle> kinematicMoved;    // moved by velocity this step; stopped after it
    // BodyMotion written by anything but this scene (a snapshot's restore): bodies take theirs in
    // the next sync, players in their next step (after its teleport).
    std::vector<entt::entity> motionTouched;
    ankerl::unordered_dense::set<entt::entity> playerMotion;
    bool writingMotion{false};
    // Transforms written by anything but this scene since the last step (an edit, a restore):
    // such a body or player is placed there however close it is to where the scene last put it.
    // Moved through a parent, a pose further than 1e-4 from the scene's own counts too.
    ankerl::unordered_dense::set<entt::entity> posedExternally;
    bool writingPose{false};

    // The simulation a snapshot carries (SnapshotHook): the backend's state (bodies, contact
    // cache, characters), which body is whose, the players and the contact bookkeeping. A restore
    // hands it back; the next sync puts it in place if the bodies are the same ones, else the
    // components' path (teleports and BodyMotion) does what it can.
    struct SavedSimulation;
    std::shared_ptr<const SavedSimulation> savedPending;
    // A restore since the players last spawned: a PlayerController it put back (a dead enemy's,
    // combat's round restart) gets its character where the restored entity stands, not at a spawn.
    bool restoredSinceSpawn{false};

    // A player (spec "The player"): its character, and what the step last made of it.
    struct PlayerRecord {
        pb::CharacterHandle character{pb::INVALID_CHARACTER};
        pb::BodyHandle inner{pb::INVALID_BODY};
        glm::vec3 written{0.0f};                // the Transform position last written
        glm::quat writtenRot{1, 0, 0, 0};
        glm::vec3 horizontal{0.0f};             // m/s
        float vy{0.0f};                         // m/s, the vertical velocity at the step's end
        float yawDeg{0.0f}, pitchDeg{0.0f};
        // The head (spec "The camera"): the child holding the Camera, and its local pose last written.
        entt::entity head{entt::null};
        glm::vec3 headPos{0.0f};
        glm::quat headRot{1, 0, 0, 0};
        // Footsteps (spec "Footsteps"): ground walked since the last footstep, the highest point
        // since it last stood on the ground, and whether it did at the last step's end.
        float stride{0.0f};
        float airTop{0.0f};
        bool grounded{false};
        entt::entity ground{entt::null};   // the body it stood on at the last step's end
        uint64_t footsteps{0};
        // Made or teleported, not stepped since: its ground is found again when bodies are built
        // (a play scene spawns its player before the level's bodies exist).
        bool placed{true};
    };
    static constexpr float kWalkStride = 0.75f;     // m between footsteps walking
    static constexpr float kSprintStride = 0.95f;   // and sprinting
    static constexpr float kLandDrop = 0.3f;        // m fallen before touching the ground is a land
    ankerl::unordered_dense::map<entt::entity, PlayerRecord> players;
    std::vector<entt::entity> playersTouched;  // PlayerController added or changed
    // Characters whose PlayerController went (a dead enemy's), out of the world but kept with the
    // controller they were made for: a restore that puts the controller back gets the same
    // character, so the snapshot's simulation restores exactly. Destroyed with their entity.
    struct Parked {
        PlayerRecord record;
        PlayerController made;
    };
    ankerl::unordered_dense::map<entt::entity, Parked> parked;

    // Render interpolation (spec "Interpolation"): what the last step moved, from where to where
    // (world poses, without scale). An entry the next step does not move is dropped after it.
    struct Mover {
        glm::vec3 fromPos{0.0f}, toPos{0.0f};
        glm::quat fromRot{1, 0, 0, 0}, toRot{1, 0, 0, 0};
        bool moved{false};
    };
    ankerl::unordered_dense::map<entt::entity, Mover> movers;
    ankerl::unordered_dense::set<entt::entity> drawn;   // given a RenderPose by the last interpolate

    // Joints (sub-project 13): one constraint per Joint, between the bodies its entity and `other`
    // belong to. Built after the bodies in creation order, so the constraint list (which Jolt's
    // solver and its saved state order by) is the same on every run.
    struct JointRecord {
        pb::ConstraintHandle handle{pb::INVALID_CONSTRAINT};
        entt::entity a{entt::null}, b{entt::null};   // the body entities (b null: the world)
    };
    ankerl::unordered_dense::map<entt::entity, JointRecord> joints;
    ankerl::unordered_dense::map<pb::ConstraintHandle, entt::entity> jointOf;
    std::vector<entt::entity> jointsTouched;   // Joint changed, or a body it joins rebuilt: rebuild
    std::vector<entt::entity> jointsBroken;    // JointState changed by anything but a step: re-enable or disable
    bool writingJoint{false};

    Impl(entt::registry& r, AssetResolver& a);

    ~Impl();

    void touchPlayer(entt::registry&, entt::entity e) { playersTouched.push_back(e); }
    void touchJoint(entt::registry&, entt::entity e) { jointsTouched.push_back(e); }
    void touchJointState(entt::registry&, entt::entity e);
    void touchMotion(entt::registry&, entt::entity e);
    void touchPose(entt::registry&, entt::entity e);
    // This scene's own Transform write (the write-back, a player's step).
    template <class F>
    void writePose(entt::entity e, F&& f) {
        writingPose = true;
        reg.patch<Transform>(e, std::forward<F>(f));
        writingPose = false;
    }
    // This scene's own BodyMotion write (not a restore's), only when it changes.
    void writeMotion(entt::entity e, const BodyMotion& m);
    // A restored body's motion: its velocities, awake, or asleep at rest (after the restore's
    // teleport, which left it at rest).
    void applyMotion(entt::entity e);
    // A player's character leaves the world with its PlayerController (parked; destroyed with its
    // entity); its trigger pairs end.
    void onPlayerGone(entt::registry&, entt::entity e) { removePlayer(e, true); }
    void removePlayer(entt::entity e, bool park);
    void dropParked(entt::entity e);
    // A parked character for a controller like the one it was made for (a restore's), back in the
    // world; else the parked one goes and false.
    bool unpark(entt::entity e, const PlayerController& pc, PlayerRecord& out);

    template <class T>
    void connect() {
        reg.on_construct<T>().template connect<&Impl::touch>(this);
        reg.on_update<T>().template connect<&Impl::touch>(this);
        reg.on_destroy<T>().template connect<&Impl::touch>(this);
    }
    template <class T>
    void disconnect() {
        reg.on_construct<T>().disconnect(this);
        reg.on_update<T>().disconnect(this);
        reg.on_destroy<T>().disconnect(this);
    }
    // Only queued: the entity may be in the middle of being destroyed.
    void touch(entt::registry&, entt::entity e) { touched.push_back(e); }
    // A body entity destroyed (or its PhysicsBody removed by hand); not the scene's own removal.
    void onBodyGone(entt::registry&, entt::entity e);

    pb::IPhysicsBackend& ensureBackend();

    void warn(entt::entity e, std::string code, std::string message);

    [[nodiscard]] uint64_t seqOf(entt::entity e) const;

    [[nodiscard]] bool ownCollider(entt::entity e) const;

    // The mesh a collider is fitted to: a CollisionMesh, else the entity's MeshSource.
    [[nodiscard]] std::shared_ptr<const Model> meshOf(entt::entity e);

    // One collider as a backend shape in the body's frame (scale baked), or nullopt.
    std::optional<pb::ShapeInfo> shapeOf(entt::entity e, const Collider& c, glm::vec3 scale, Motion motion,
                                         std::vector<std::shared_ptr<FittedCollider>>& keep, BodyRecord::Kind& kind);

    // A removed body's pairs end now (Jolt's later OnContactRemoved for it no longer maps).
    void endPairsOf(pb::BodyHandle h);

    [[nodiscard]] PhysicsEvent endEvent(const Pair& p) const;

    void destroyBody(entt::entity e);

    void build(entt::entity e, const std::vector<entt::entity>& parts);

    void syncBodies();

    // ---- joints (sub-project 13) -------------------------------------------------------------------

    // The body an entity is part of: its own, a ColliderPart's target, else its nearest ancestor's.
    [[nodiscard]] entt::entity bodyOwner(entt::entity e) const;

    void destroyJoint(entt::entity e);

    // Before a body goes: its joints go first (Jolt must not keep a constraint on a removed body)
    // and are built again after the bodies.
    void destroyJointsOf(entt::entity body);

    void sortBySeq(std::vector<entt::entity>& list) const;

    void syncJoints();

    void buildJoint(entt::entity e);

    // After the substeps: a broken constraint's entity gets JointState and a joint_break event.
    void publishBrokenJoints(std::vector<PhysicsEvent>& fresh);

    // Every joint's constraint and its entity's id, by handle.
    [[nodiscard]] std::vector<std::pair<pb::ConstraintHandle, uint64_t>> jointIdentity() const;

    // ---- snapshots (SnapshotHook) ----------------------------------------------------------------

    struct SavedPlayer {
        uint64_t id{0}, headId{0}, groundId{0};
        PlayerRecord record;
    };

    // Every body (players' inner bodies too) and its entity's id, by handle.
    [[nodiscard]] std::vector<std::pair<pb::BodyHandle, uint64_t>> bodyIdentity() const;

    [[nodiscard]] std::shared_ptr<const void> saveSimulation() const;
    void restoreSimulation(const std::shared_ptr<const void>& state);
    bool restoreSaved();

    // Bodies up to date, then the motion a restore wrote: into the bodies (after their
    // teleports); players take theirs in their step, after its own teleport.
    void sync();

    // ---- the step (sub-project 6) ---------------------------------------------------------------

    // The step's contact and trigger events, from the backend's queue (filled by Jolt's callbacks
    // during the substeps, on whatever thread; drained here on the simulating one).
    void drainContacts(std::vector<PhysicsEvent>& out);

    // Every moving body of the pair asleep (a static one never wakes).
    [[nodiscard]] bool asleep(uint64_t key) const;

    [[nodiscard]] uint64_t idOf(entt::entity e) const;

    // Every dynamic body awake now or at the end of the last step: its pose into its entity's
    // Transform, through its parent's world transform (the local scale is kept), and its motion
    // into BodyMotion (asleep at rest, once, as it falls asleep).
    void writeBack();

    void publish(std::vector<PhysicsEvent>& fresh);

    // ---- the player (spec "The player") -----------------------------------------------------------

    static float yawOf(const glm::quat& q);
    static glm::quat yawQuat(float deg) { return glm::angleAxis(glm::radians(deg), glm::vec3(0.0f, 1.0f, 0.0f)); }
    static glm::quat pitchQuat(float deg) { return glm::angleAxis(glm::radians(deg), glm::vec3(1.0f, 0.0f, 0.0f)); }
    // Up positive, from the rotation's -Z.
    static float pitchOf(const glm::quat& q);

    // An entity's world matrix from the Transforms up its chain: right after a level expands,
    // before the transform system has given its nodes a WorldTransform.
    [[nodiscard]] glm::mat4 worldOf(entt::entity e) const;

    // The player's head: its first child (creation order) holding a Camera, else one named Head.
    [[nodiscard]] entt::entity headOf(entt::entity e) const;

    void recordMove(entt::entity e, glm::vec3 fromPos, glm::quat fromRot, glm::vec3 toPos, glm::quat toRot);

    // The spawn point of a character: the k-th character with its PlayerController.spawn tag (in
    // creation order) takes the k-th SpawnPoint with that tag (in creation order); null if it has
    // no tag or no point is left for it.
    [[nodiscard]] entt::entity spawnFor(entt::entity e) const;

    // A character for a new PlayerController, at the player spawn (or where the entity is, with
    // W_PLAYER_NO_SPAWN); for a changed one, where the player stands, facing as it faced; for one
    // a restore put back (`restored`), as the restored entity and its head stand.
    void createPlayer(entt::entity e, bool restored);

    // A placed player (made or teleported) on the ground the backend found where it is.
    void standWhereItIs(PlayerRecord& p);
    [[nodiscard]] entt::entity groundEntity(const pb::CharacterState& st) const;

    bool teleport(entt::entity e, glm::vec3 pos, std::optional<float> yawDeg);

    // The player's Transform (feet, yaw), its head and PlayerState. `track`: a move of the step,
    // drawn between where it was and where it is (else a placement).
    void writePlayer(entt::entity e, PlayerRecord& p, glm::vec3 pos, glm::vec3 velocity, bool onGround, bool track);

    // Recoil: the view turned after the step moved the player. A turn inside a step keeps the step's
    // move (drawn from where the step started to the turned pose); outside one it is a placement.
    bool turnPlayer(entt::entity e, float dYaw, float dPitch);

    // The head at eyeHeight above the feet, pitched; written only when it changes.
    void writeHead(entt::entity e, PlayerRecord& p, glm::vec3 fromPos, glm::quat fromRot, bool track);

    // Characters for PlayerControllers added or changed since: the `player_spawn` sync system
    // (before `transform`, so a play scene's first frame is drawn from the spawn) and each step.
    void spawnPlayers();

    // A player's footstep or landing on the ground body `ground` (spec "Footsteps").
    PhysicsEvent playerEvent(PhysicsEvent::Kind kind, entt::entity e, const pb::CharacterState& at, float speed) const;

    // Every player, once per step, before the bodies step (spec: the character moves first); its
    // footsteps and landing go into `out`.
    void updatePlayers(const InputMap& input, float dt, std::vector<PhysicsEvent>& out);

    void step(const InputMap& input, float dt);

    // RenderPose for what the last step moved, between its last two step poses at alpha, and for
    // their subtrees (a descendant that moves by itself has its own entry). Alpha 1 (or outside
    // [0, 1)) draws the step's poses: every RenderPose goes. The step's state is not touched.
    void interpolate(float alpha, glm::vec2 pendingLook);

    // The look lead (sub-project 9, spec "The look lead"): an input-driven, living player is drawn
    // turned by the look its steps have not consumed, through applyLook, the step's own arithmetic:
    // its yaw on the body, its pitch on the head, the rest of the subtree carried along. Between
    // steps (realtime) its turn is drawn at the newest step's, never slerped: only the position is
    // interpolated. In lockstep (alpha 1) with nothing pending, nothing is drawn but the step's poses.
    // Presentation only: RenderPose, never Transform.
    bool leadsLook{false};   // an input-driven, living player exists (the last interpolate saw one)

    void lookLead(bool between, float alpha, glm::vec2 pending, const ankerl::unordered_dense::set<entt::entity>& was);

    // A drawn pose for x this frame; its previous one is what the last frame drew of it.
    void setDrawn(entt::entity x, const glm::mat4& world, const ankerl::unordered_dense::set<entt::entity>& was);

    [[nodiscard]] std::vector<PhysicsEvent> since(uint64_t after, std::size_t max, uint64_t& dropped) const;
};

} // namespace ohao::ecs
