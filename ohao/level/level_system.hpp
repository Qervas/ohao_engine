#pragma once
// The `level` sync system (sub-project 5, docs/specs/2026-10-26-level-pipeline-design.md,
// "glTF -> world mapping"). A Level component becomes one child entity per node of its file's
// default scene, depth first in file order:
//
//   - Parent (the level entity for a root node), Name, Transform (the node's TRS), and
//     LevelNode{level, index}. Its id is levelNodeId(level's id, index), so the same file gives
//     the same ids in every process and on every reload.
//   - A mesh node gets MeshSource{ModelAsset{asset, "#<mesh index>"}} unless it is collision
//     only (vis 0, or a UCX_ hull): instantiate's (path, mesh) cache shares one GPU mesh among
//     the nodes that use a glTF mesh.
//   - A KHR_lights_punctual light becomes a Light (intensity x lightScale, range 10 m where the
//     file leaves it infinite); a perspective camera a Camera (far 200 m where infinite),
//     active only when no other camera is in the world.
//   - spawn and socket hints become SpawnPoint and Socket; passed-through components (level_hints)
//     are set last, so they win.
//
// When a Level is added or changed the level is re-expanded in place: a node whose entity
// still exists keeps it, takes back the file's name, transform, parent and components, and
// loses any saved component the file does not give it; nodes no longer in the file are
// destroyed, new ones created. An unchanged file therefore leaves world.hash as it was.
// Removing the Level (or its entity) destroys its nodes. Saving skips them (scene_io).
//
// Problems with the file or its hints leave InstantiateError on the level entity (the file's
// error, or E_LEVEL_EXTRAS with the count) and are kept for levelDiagnostics; each is logged.
// A file's structure is read once per path and cached for the World's lifetime.
//
// Load-time code: it reads files and decodes components through reflection, so it lives here,
// outside the ECS system sources reflect_tests keeps free of reflect/. It runs only for a Level
// that was added or changed, never per frame.
#include "ecs/asset_resolver.hpp"
#include "ecs/world.hpp"
#include "reflect/decode.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ohao::reflect { class TypeRegistry; }

namespace ohao::level {

// splitmix64 of the pair: deterministic, never 0.
[[nodiscard]] uint64_t levelNodeId(uint64_t levelId, uint32_t index) noexcept;

// Adds the "level" sync system right before "instantiate" (or "transform" when instantiate is
// not installed; installInstantiate, called later, still lands after it). Once per World; the
// resolver and registry must outlive it.
void installLevels(ecs::World& world, ecs::AssetResolver& assets, const reflect::TypeRegistry& types);

// The warnings and errors of the level entity's last expansion (empty for none, or for a
// world without the level system).
[[nodiscard]] std::vector<reflect::Diagnostic> levelDiagnostics(const ecs::World& world, entt::entity level);

// Whether installLevels ran on this World.
[[nodiscard]] bool levelsInstalled(const ecs::World& world);

// What level.info reports from the level's file and its last expansion (spawns, colliders and
// bodies the caller reads from the world). Counts are of the default scene's nodes: meshes is
// the mesh nodes, uniqueMeshes the glTF meshes they use, triangles over every mesh node.
struct LevelSummary {
    std::string path;            // the Level's asset path
    bool loaded{false};          // false: the file did not load (its error is on the level entity)
    uint32_t nodes{0}, meshes{0}, uniqueMeshes{0}, lights{0}, cameras{0};
    uint64_t triangles{0};
    double loadMs{0.0};          // the last expansion: the file's structure, the nodes and their components
};
// nullopt: no level system, or the entity has not been expanded (not a Level, or not synced yet).
[[nodiscard]] std::optional<LevelSummary> levelSummary(const ecs::World& world, entt::entity level);

// level.reload: forgets the level's file (its structure here, its meshes in the asset resolver),
// so the next "level" sync re-reads it and re-expands the level in place, then touches its nodes'
// colliders so the physics system rebuilds their bodies. An unchanged file keeps world.hash.
// Meshes already uploaded to the GPU are kept (instantiate caches them per (file, mesh)).
// False without the level system or when `level` has no Level.
bool reloadLevel(ecs::World& world, entt::entity level);

} // namespace ohao::level
