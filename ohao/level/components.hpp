#pragma once
// The level module's components (level/module.hpp): a level file placed in the world (Level), the entities its nodes
// become (LevelNode), its attach points (Socket) and the re-expansion tag (LevelDirty). A level's spawn points are the
// core's SpawnPoint (ecs/components.hpp): physics and the games read them too.
#include "reflect/asset_ref.hpp"

#include <entt/entity/entity.hpp>

#include <cstdint>
#include <string>

namespace ohao::ecs {

using AssetRef = reflect::AssetRef;   // as ecs/components.hpp names it

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

// An attach point (Unreal's SOCKET_ empties), at the entity's transform.
struct Socket { std::string name; };      // at most 64 characters

// Tag: Level changed; the level system re-expands it this frame.
struct LevelDirty {};

} // namespace ohao::ecs
