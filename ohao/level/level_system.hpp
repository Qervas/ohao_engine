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

} // namespace ohao::level
