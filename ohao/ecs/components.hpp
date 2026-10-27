#pragma once
#include "core/input.hpp"          // Key
#include "reflect/asset_ref.hpp"   // a plain struct; no reflection machinery
#include "render/scene/handles.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ohao::ecs {

using ohao::Key;

// The entity's stable identity: never recycled, survives save/load and snapshots, and is
// what files, commands and (later) prefabs and scripts refer to. Assigned by
// World::create / createWithId; 0 is invalid. Do not emplace or edit it directly.
struct EntityId { uint64_t value{0}; };

struct Name { std::string value; };

// Local transform. Write it with registry.patch<Transform>() or World::setTransform();
// a plain get<>() write is not tracked (see verifyTransforms).
struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    [[nodiscard]] glm::mat4 matrix() const;
};

// world: this frame's object-to-world. prevWorld: last frame's (motion vectors).
struct WorldTransform {
    glm::mat4 world{1.0f};
    glm::mat4 prevWorld{1.0f};
};

struct Parent { entt::entity entity{entt::null}; };
struct Children { std::vector<entt::entity> entities; };

// Change tracking: like Transform, MeshRenderer, Light, EmissiveProxyLight and
// GiMaterialLegacy are picked up by the render extract only when written through
// registry.patch<T>() or registry.replace<T>() (or emplaced/removed). A plain
// get<T>() write fires no signal and never reaches the renderer.

// One material per submesh, in submesh order. Fewer entries than submeshes is legal:
// the missing submeshes render with the default material.
struct MeshRenderer {
    render::MeshHandle mesh{};
    std::vector<render::MaterialHandle> materials;
    bool visible{true};
};

enum class LightType : uint8_t { Point, Directional, Spot, AreaRect };

// Position and direction come from WorldTransform: direction is the entity's -Z.
struct Light {
    LightType type{LightType::Point};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    float range{10.0f};
    float radius{0.1f};
    float innerConeDeg{30.0f};
    float outerConeDeg{45.0f};
    glm::vec3 areaEdge1{1.0f, 0.0f, 0.0f};   // AreaRect, local space
    glm::vec3 areaEdge2{0.0f, 0.0f, 1.0f};
    bool castShadows{true};
};

// A point light approximating a mesh's emissive texture (ported from the renderer's
// emissive auto-light). localCenter/radius are in the entity's model space.
struct EmissiveProxyLight {
    glm::vec3 localCenter{0.0f};
    float radius{0.0f};
    glm::vec3 color{1.0f};
    float intensity{0.0f};
};

// Monotonic creation sequence, set by World::create. Unlike entity ids it is never
// recycled, so it orders lights (and new instances) deterministically.
struct CreationOrder { uint64_t seq{0}; };

// LEGACY: RT GI's per-instance material word (see render::InstanceGpu::giAlbedoPacked).
// rgb = albedo, a = packed roughness/metallic/vertex-count rule. Set by spawnModel.
struct GiMaterialLegacy { glm::vec4 packed{0.73f, 0.73f, 0.73f, 0.95f}; };

// Components a scene file carried that this build does not know (newer engine, other
// tool). Kept as canonical compact JSON and written back on save, sorted by name.
struct PreservedComponent {
    std::string name;
    uint32_t version{1};
    std::string json;
};
struct PreservedComponents { std::vector<PreservedComponent> entries; };

struct TransformDirty {};
struct RenderDirty {};
struct MovedLastFrame {};

// ---- meshes from data (instantiate, ecs/instantiate.hpp) ------------------------------------

using AssetRef = reflect::AssetRef;   // {path (project-relative, '/'), guid (empty until the asset DB)}

struct ModelAsset   { AssetRef asset; std::string mesh; };   // mesh: one glTF mesh (name or #index); empty: the file
struct SpherePrim   { float radius{0.5f}; int32_t sectors{32}; int32_t stacks{16}; };
struct CubePrim     { float size{1.0f}; };
struct PlatformPrim { float width{2.0f}; float height{0.4f}; float depth{2.0f}; };
struct CylinderPrim { float radius{0.5f}; float height{1.0f}; int32_t segments{16}; };
struct ConePrim     { float radius{0.5f}; float height{1.0f}; int32_t segments{16}; };
// The examples' builders, made reusable (vertex color = color, uv 0, tangent (1,0,0,1)).
// Defaults: a unit quad facing +Z, a unit box around the origin.
struct QuadPrim {
    glm::vec3 a{-0.5f, -0.5f, 0.0f};
    glm::vec3 b{0.5f, -0.5f, 0.0f};
    glm::vec3 c{0.5f, 0.5f, 0.0f};
    glm::vec3 d{-0.5f, 0.5f, 0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec3 color{1.0f};
};
struct BoxPrim { glm::vec3 lo{-0.5f}; glm::vec3 hi{0.5f}; glm::vec3 color{1.0f}; };

// Where an entity's mesh comes from. Instantiate builds MeshRenderer (and the derived
// GiMaterialLegacy / EmissiveProxyLight) from it; meshes are shared per distinct value.
struct MeshSource {
    std::variant<ModelAsset, SpherePrim, CubePrim, PlatformPrim, CylinderPrim, ConePrim, QuadPrim, BoxPrim> source;
};

// The material of a mesh without its own materials (primitives); for a model with
// materials it only feeds the legacy RT GI word. Same rules as uploadModel's override.
struct MaterialOverride {
    glm::vec3 baseColor{0.8f};
    float roughness{0.5f};
    float metallic{0.0f};
    float emissiveStrength{0.0f};
    float subsurface{0.0f};   // 0: none; > 0 the screen-space subsurface blur's strength (skin, wax)
};

// Instantiate's bookkeeping: tagged when MeshSource/MaterialOverride change; the error
// when a mesh could not be built (no MeshRenderer then).
struct InstantiateDirty {};
struct InstantiateError {
    std::string code;      // E_ASSET_NOT_FOUND, E_ASSET_INVALID, E_MESH_INVALID
    std::string message;
};

// ---- the view (ecs/scene_view.hpp; applied to a renderer by the host) ------------------------

// The scene camera, one per world. Its pose is the entity's world transform: the view
// looks along -Z with +Y up (roll is ignored; the renderer camera is pitch/yaw).
struct Camera {
    float fovDeg{45.0f};       // vertical field of view
    float nearPlane{0.1f};
    float farPlane{100.0f};
    bool active{true};         // false: the host keeps its own camera
};

// Image-based lighting, one per world. No hdri: the renderer's default sky/black.
struct Environment {
    std::optional<AssetRef> hdri;
    float intensity{1.0f};
};

// ---- engine-provided behaviour (ecs/spin_system.hpp; run by every World) -------------------

// Turns the entity about an axis of its own local frame at a constant rate. The spin
// system writes Transform::rotation each update, before the transform system.
struct Spin {
    glm::vec3 axis{0.0f, 1.0f, 0.0f};   // local frame; normalized when applied; zero: no turn
    float degreesPerSecond{90.0f};      // negative turns the other way
};

// ---- levels (sub-project 5; ohao/level/level_system.hpp) -------------------------------------

// A level file (glTF) placed in the world. The `level` sync system expands it into one child
// entity per node of the file's default scene (LevelNode), with deterministic ids; those
// entities are the file's and are never saved, so a scene file carries only this line.
struct Level {
    AssetRef asset;            // a .glb or .gltf, project-relative
    float lightScale{1.0f};    // multiplies every KHR_lights_punctual intensity
    bool collision{true};      // false: a render-only level (step 4 builds no bodies for it)
};

// Runtime only: this entity is node `index` (depth-first order of the default scene) of the
// level on entity `level`. Its id is level::levelNodeId(level's id, index).
struct LevelNode {
    entt::entity level{entt::null};
    uint32_t index{0};
};

// A named place to spawn something; it faces its local -Z. Agents find them with
// query --has SpawnPoint, in creation (node) order.
struct SpawnPoint { std::string tag; };   // "player", "enemy", ... (at most 32 characters)

// An attach point (Unreal's SOCKET_ empties), at the entity's transform.
struct Socket { std::string name; };      // at most 64 characters

// Tag: Level changed; the level system re-expands it this frame.
struct LevelDirty {};

// ---- collision (ecs/physics_scene.hpp; the `physics` sync system builds Jolt bodies) ----------

enum class ColliderShape : uint8_t { Box, Sphere, Capsule, Convex, Mesh, None };
enum class Motion : uint8_t { Static, Dynamic, Kinematic };

// The entity collides. size 0 fits the shape to the entity's mesh (its MeshSource, or a
// level node's CollisionMesh), in its own frame, scaled by its world scale.
struct Collider {
    ColliderShape shape{ColliderShape::Box};
    glm::vec3 size{0.0f};     // box: half extents; sphere: x = radius; capsule: x = radius, y = half height
    glm::vec3 offset{0.0f};   // the shape's centre in the entity's frame (with an explicit size)
    bool trigger{false};      // a sensor: reports bodies entering and leaving it, blocks nothing
    std::string surface;      // what it is made of, for footsteps ("concrete", "metal"); empty: "default"
};

// How the entity's body moves. A Collider without a RigidBody is static.
struct RigidBody {
    Motion motion{Motion::Static};
    float mass{10.0f};         // kg, dynamic bodies
    float friction{0.6f};
    float restitution{0.0f};
};

// Runtime: this entity's Collider is part of another entity's body (a level's UCX_ hulls of
// their target): one body, a compound shape, placed by the body entity.
struct ColliderPart { entt::entity body{entt::null}; };

// Runtime: the mesh a level node's collider is fitted to, when the node is not drawn (vis 0,
// UCX_ hulls) and so has no MeshSource.
struct CollisionMesh { ModelAsset source; };

// Runtime: the entity's body in the physics backend (physics::backend::BodyHandle).
struct PhysicsBody { uint32_t handle{UINT32_MAX}; };

// Tag: the transform system moved an entity that has a body (or is part of one); the physics
// system moves the body to it.
struct PhysicsMoved {};

// ---- the player (sub-project 6; the physics system moves it, ecs/physics_scene.hpp) -----------

// A first-person player: a Jolt CharacterVirtual capsule standing on its Transform's position
// (the feet), turned about +Y by its yaw. Walks from the named axes and actions, once per fixed
// step, before the bodies step. Spawns at the first SpawnPoint tagged "player" (creation order).
struct PlayerController {
    float radius{0.3f};           // m
    float height{1.8f};           // m, feet to the top of the capsule
    float eyeHeight{1.62f};       // m, above the feet (the camera, step 3)
    float walkSpeed{4.0f};        // m/s
    float sprintSpeed{6.5f};      // m/s, while `sprint` is held
    float acceleration{40.0f};    // m/s², toward the wanted horizontal velocity on the ground
    float airControl{0.3f};       // fraction of the acceleration in the air
    float jumpSpeed{4.9f};        // m/s up (a 1.22 m jump)
    float maxSlopeDeg{45.0f};     // steeper ground is a wall
    float stepHeight{0.35f};      // m, steps climbed without jumping
    float mass{80.0f};            // kg
    float pushForce{200.0f};      // N, the most it pushes a dynamic body with
    float lookDegPerUnit{0.1f};   // turn per unit of mouse movement (or of a look axis a step)
    std::string moveForward{"move_forward"};
    std::string moveRight{"move_right"};
    std::string jump{"jump"};
    std::string sprint{"sprint"};
    std::string lookX{"look_x"};
    std::string lookY{"look_y"};
    // Sub-project 7: where it spawns and what moves it. The k-th character with a tag (creation
    // order) takes the k-th SpawnPoint with that tag; empty: it starts where it stands.
    std::string spawn{"player"};
    bool input{true};             // false: moved by its MoveIntent (an AI), not by the world's input
};

// The move a character without input (PlayerController.input false) makes each step: written by
// an AI (the enemy AI) or an agent. Absent: it stands still, facing as it faces.
struct MoveIntent {
    glm::vec2 move{0.0f};     // x right, y forward, in its own frame; each within -1..1
    float yawDeg{0.0f};       // the facing to take, about +Y from -Z
    float pitchDeg{0.0f};     // the look up (positive) or down, within ±89
    bool jump{false};
    bool sprint{false};
};

// Runtime, render only: where the renderer draws an entity a simulation step moved, between its
// last two step poses at the World's render alpha (World::setRenderAlpha; sub-project 6 spec,
// "Interpolation"). Set and removed by the `interpolate` sync system; the render extract and
// the scene view's camera read it in place of WorldTransform. Never saved or hashed: the step's
// pose stays in Transform and WorldTransform.
struct RenderPose {
    glm::mat4 world{1.0f};
    glm::mat4 prevWorld{1.0f};   // what the last frame drew (motion vectors)
};

// Runtime: the player as the physics system left it after the last step.
struct PlayerState {
    glm::vec3 velocity{0.0f};     // m/s, world
    bool onGround{false};
    float yawDeg{0.0f};           // about +Y from -Z, counter-clockwise seen from above
    float pitchDeg{0.0f};         // up positive, within ±89
    uint32_t character{UINT32_MAX};   // physics::backend::CharacterHandle
};

// Runtime, snapshotted (sub-project 6 spec, "Snapshots and replay"): the motion a body carries
// into the next step, which the saved components do not hold. The physics step writes it for
// every dynamic body awake during the step (and once more as it falls asleep) and for every
// player whose velocity changed; a write by anything else (a snapshot's restore) puts it into the
// body. Never saved or hashed; snapshots hold it. A snapshot also carries the whole simulation
// (PhysicsScene::saveSimulation: contact cache, sleep timers, characters), which a restore uses
// when the bodies are the same ones; BodyMotion is what a restore uses when they are not (a body
// rebuilt since), and what agents read.
struct BodyMotion {
    glm::vec3 linear{0.0f};    // m/s, world; a player's velocity
    glm::vec3 angular{0.0f};   // rad/s, world
    bool asleep{false};        // the body sleeps (its velocities are then zero)
};

// ---- combat (sub-project 7; ecs/combat.hpp, the `combat` simulate system after `physics`) ------

// Hit points. Damage takes current down to 0, which is dead: the dead take no more damage and
// do not fire.
struct Health {
    float max{100.0f};
    float current{100.0f};
};

// A hitscan weapon. A player fires it from its eye along its look, by the World's input (`fire`
// held or pressed, `reload` pressed); anything else from its own origin along its -Z, by its
// WeaponTrigger. Each round is one ray to `range`; what it hits takes `damage` if it (or an
// ancestor) has Health.
struct Weapon {
    float damage{25.0f};                   // hp a round
    float fireRate{10.0f};                 // Hz, rounds a second
    bool automatic{true};                  // fires while held; false: one round a press
    uint32_t magazine{30};                 // rounds a magazine (reserve is unlimited)
    float reloadTime{1.5f};                // s
    float range{100.0f};                   // m
    float spreadDeg{0.25f};                // the cone's half angle at rest
    float bloomPerShotDeg{0.35f};          // added to the cone by each round
    float bloomMaxDeg{3.0f};               // the most bloom adds
    float bloomRecoveryDegPerSec{6.0f};    // how fast bloom falls once the weapon is idle
    float recoilPitchDeg{0.35f};           // view kick up a round
    float recoilYawDeg{0.15f};             // view kick sideways a round, at most (seeded, either way)
    float recoilRecoveryDegPerSec{6.0f};   // how fast the vertical kick comes back once idle
    uint32_t seed{1};                      // the spread's and recoil's random numbers
    std::string fire{"fire"};
    std::string reload{"reload"};
};

// What the weapon has and is doing. The combat step writes it (and emplaces it, a full magazine,
// for a Weapon without one); saved, so files, world.hash and snapshots hold a fight exactly.
struct WeaponState {
    uint32_t ammo{30};          // rounds in the magazine
    float cooldown{0.0f};       // s until the next round may fire
    float reloading{0.0f};      // s of reload left; 0: not reloading
    float bloomDeg{0.0f};       // added to the spread cone now
    float kickDeg{0.0f};        // vertical view kick not yet recovered
    uint32_t shots{0};          // rounds fired: the index of the next round's random numbers
    bool triggerHeld{false};    // the trigger at the last step (one round a press)
    float flash{0.0f};          // s of muzzle flash left (the Muzzle's light is on while > 0)
};

// A weapon's muzzle flash: on a Light below the weapon's entity (its child, or deeper). The combat
// step turns the light to `intensity` for `time` after each round and to 0 otherwise; a light at 0
// has no emitter mesh, so the flash is the light's mesh while it is on.
struct Muzzle {
    float intensity{6.0f};   // the Light's intensity while the flash is on
    float time{0.05f};       // s the flash stays on after a round
};

// The trigger of a weapon that is not a player's: an enemy's AI, a turret, a test pulls it.
struct WeaponTrigger {
    bool pull{false};     // held
    bool reload{false};   // reload now (cleared when acted on)
};

// An enemy: a character (PlayerController, input false) with Health and a Weapon that the enemy
// AI drives, at the start of each combat step: idle until it sees the player (or is hit), then
// chases the place it last saw it, and in range with a clear line attacks in bursts.
struct Enemy {
    float sightRange{30.0f};       // m
    float fovDeg{140.0f};          // the cone it notices the player in while idle
    float attackRange{14.0f};      // m
    float reactionTime{0.35f};     // s from first sight to first round
    float aimErrorDeg{2.5f};       // at most this off the player, each round (seeded)
    uint32_t burst{3};             // rounds a burst
    float burstPause{0.8f};        // s between bursts
    float loseTime{3.0f};          // s it chases without seeing the player before giving up
    float turnRateDegPerSec{360.0f};
};

enum class EnemyMode : uint8_t { Idle, Chase, Attack, Dead };

// What an enemy is doing. The enemy AI writes it (and adds it to an Enemy without one); saved,
// so files, world.hash and snapshots hold the AI exactly.
struct EnemyState {
    EnemyMode mode{EnemyMode::Idle};
    glm::vec3 lastSeen{0.0f};      // where it last saw the player (its feet)
    float lostFor{0.0f};           // s since it last saw the player
    float reaction{0.0f};          // s left before it may fire
    uint32_t burstLeft{0};         // rounds left in this burst
    float pause{0.0f};             // s left of the pause between bursts
    float lastHealth{-1.0f};       // its health at the last step (a fall wakes it); < 0: unknown
    uint32_t shotsSeen{0};         // its weapon's WeaponState.shots at the last step (counts a burst)
};

// The round's rules, one per world. A world with one keeps the round's start (a snapshot through
// the host's RoundHook, at the end of its first combat step) and goes back to it restartDelay
// after the player dies, or when `restart` is pressed; every Enemy dead wins the round.
struct Round {
    float restartDelay{3.0f};       // s from the player's death to the restart
    std::string restart{"restart"}; // action that restarts the round now
};

// ---- input (core/input.hpp; the World's InputMap, read by simulate systems) -----------------

struct ActionBinding {
    std::string action;   // "jump"
    Key key{Key::None};   // "Space"
};
struct AxisBinding {
    std::string axis;          // "move_forward"
    Key positive{Key::None};   // +1 while held
    Key negative{Key::None};   // -1 while held
};
// The world's controls, one per world: action and axis names over keys, so a scene file
// carries them and an agent edits them like any component. The World's InputMap is
// rebuilt from it whenever it is set, changed or removed.
struct InputActions {
    std::vector<ActionBinding> actions;
    std::vector<AxisBinding> axes;
};

// Flies the entity from the named axes and actions (the fly_controller system, once per
// fixed step): forward along its -Z, right along its +X, up along world +Y, at moveSpeed
// (times fastMultiplier while `fast` is held); while `look` is held the mouse turns it,
// yaw about world +Y and pitch about its own +X. Put on the Camera entity, it is a fly
// camera.
struct FlyController {
    float moveSpeed{3.0f};
    float fastMultiplier{4.0f};
    float lookDegPerUnit{0.1f};
    std::string moveForward{"move_forward"};
    std::string moveRight{"move_right"};
    std::string moveUp{"move_up"};
    std::string look{"look"};
    std::string fast{"fast"};
};

} // namespace ohao::ecs
