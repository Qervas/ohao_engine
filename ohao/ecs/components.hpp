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

// Collision hints read from a level node's extras (col, body). The Collider and RigidBody
// components that use them are step 4's.
enum class ColliderShape : uint8_t { Box, Sphere, Capsule, Convex, Mesh, None };
enum class Motion : uint8_t { Static, Dynamic, Kinematic };

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
