#pragma once
// The physics module's components (physics/module.hpp): colliders, bodies, joints and the players the physics
// system moves (physics/physics_scene.hpp).
#include "ecs/components.hpp"
#include "visual/components.hpp"   // ModelAsset: a CollisionMesh's

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace ohao::ecs {

// ---- collision (physics/physics_scene.hpp; the `physics` sync system builds Jolt bodies) ----------

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

// ---- joints (sub-project 13; physics/physics_scene.hpp builds one constraint per Joint) -----------

enum class JointType : uint8_t { Fixed, Point, Hinge, Slider, SwingTwist, Distance };

// A constraint between the body this entity is part of (its own, else its nearest ancestor's) and
// `other`'s (likewise; null: the world). The anchor and axis are in this entity's frame; the other
// side is the same world point and axis when the joint is built (at load, or when either body or
// the Joint is rebuilt), except a Distance joint's, which is `other`'s origin (a rope from a hook).
// Several joints on one body: child entities with a Joint each.
struct Joint {
    JointType type{JointType::Hinge};
    entt::entity other{entt::null};
    glm::vec3 anchor{0.0f};                  // m, in this entity's frame
    glm::vec3 axis{0.0f, 1.0f, 0.0f};        // hinge, slider and swing-twist's twist axis, this entity's frame
    // Hinge: degrees this entity turns (right-handed about the axis) from the pose it was built in,
    // relative to the other; slider: m it moves along the axis; swing-twist:
    // the twist's degrees; distance: the distance's range in m. Both 0: free (hinge, slider) or the
    // built distance (distance). Swing-twist always limits its twist.
    float limitMin{0.0f};
    float limitMax{0.0f};
    float swingDeg{0.0f};                    // swing-twist: the swing cone's half angle about the axis
    float springHz{0.0f};                    // distance: a soft range (0: rigid)
    float springDamping{0.0f};
    float breakForce{0.0f};                  // N s a step that breaks it (0: never)
    float breakTorque{0.0f};                 // N m s a step (0: never)
};

// A Joint that broke: its constraint stays built but does nothing (a restore of a snapshot from
// before the break makes it whole, exactly). The physics step adds it, with a joint_break event.
struct JointState {
    bool broken{true};
};

// ---- the player (sub-project 6; the physics system moves it, physics/physics_scene.hpp) -----------

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
    uint32_t stream{0};           // its input stream (World::input; a device's events go to 0)
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

} // namespace ohao::ecs
