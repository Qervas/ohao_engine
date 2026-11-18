#pragma once
#include "core/input.hpp"          // Key
#include "reflect/asset_ref.hpp"   // a plain struct; no reflection machinery
#include "core/render_handles.hpp"

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

// Change tracking: Transform, MeshRenderer and Light
// are picked up by the render extract only when written through
// registry.patch<T>() or registry.replace<T>() (or emplaced/removed). A plain
// get<T>() write fires no signal and never reaches the renderer.

// One material per submesh, in submesh order. Fewer entries than submeshes is legal:
// the missing submeshes render with the default material.
struct MeshRenderer {
    render::MeshHandle mesh{};
    std::vector<render::MaterialHandle> materials;
    bool visible{true};
};

// The RenderScene instance slot of a renderable entity. Owned by the render extract (world_render/render_extract.hpp):
// it emplaces and removes it; nothing else should write it.
struct RenderSlot { uint32_t slot; };

// Monotonic creation sequence, set by World::create. Unlike entity ids it is never
// recycled, so it orders lights (and new instances) deterministically.
struct CreationOrder { uint64_t seq{0}; };


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

// A file a component names (each such field marked .asset(kind) in its registration).
using AssetRef = reflect::AssetRef;   // {path (project-relative, '/'), guid (empty until the asset DB)}

// Instantiate's bookkeeping (world_render/instantiate.hpp): tagged when MeshSource or Material change.
struct InstantiateDirty {};

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

// Runtime, render only: where the renderer draws an entity a simulation step moved, between its
// last two step poses at the World's render alpha (World::setRenderAlpha; sub-project 6 spec,
// "Interpolation"). Set and removed by the `interpolate` sync system; the render extract and
// the scene view's camera read it in place of WorldTransform. Never saved or hashed: the step's
// pose stays in Transform and WorldTransform.
struct RenderPose {
    glm::mat4 world{1.0f};
    glm::mat4 prevWorld{1.0f};   // what the last frame drew (motion vectors)
};

// The world's effect set, one per world (sub-project 12): a `.vfx.json` naming the effects, decals
// and tracers and which combat events spawn them (vfx/vfx_set.hpp). Without one, hosts use the
// default set (today's muzzle, impact and dust presets as data).
struct VfxSet {
    AssetRef asset;   // a .vfx.json, project-relative
};

// A placed effect at its entity's pose (sub-project 12; ecs/vfx.hpp): while `active`, `rate` spawn
// records a second by step count from the step it became active, or one when it becomes active with
// rate 0. `effect` names an effect of the world's set.
struct VfxEmitter {
    std::string effect;
    float rate{0.0f};
    uint32_t seed{0};
    bool active{true};
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
