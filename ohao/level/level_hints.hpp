#pragma once
// Level hints (sub-project 5, docs/specs/2026-10-26-level-pipeline-design.md, "Extras: the
// hint schema"): what a glTF node's extras, its name and the default scene's extras say about
// the entity the node becomes.
//
//   - Keys: col, body, mass, fric, rest, trig, surf, vis, spawn, socket, verb, vfx. The default scene's extras
//     may set col and body for every node that does not.
//   - Component passthrough: a capitalised key naming a saved component, with an object
//     value, is that component, read as a scene file reads it (same diagnostics).
//   - Unreal naming: a mesh node UCX_<target>[_NN] reads as {"col":"convex","vis":0}; an
//     empty SOCKET_<name> as {"socket":"<name>"}. Explicit extras win over both.
//   - Strictness: an unknown lower-case key is W_LEVEL_EXTRAS (another tool's property); an
//     unknown capitalised key and a bad value are E_LEVEL_EXTRAS. Every diagnostic's path is
//     the node's, and its message names the node and the key. A bad key is dropped; the rest
//     of the node's hints stand.
//
// Load-time only: reflection reads the passed-through components.
#include "ecs/components.hpp"
#include "level/gltf_structure.hpp"
#include "reflect/decode.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ohao::reflect {
struct TypeInfo;
class TypeRegistry;
} // namespace ohao::reflect

namespace ohao::level {

struct LevelHints {
    std::optional<ecs::ColliderShape> col;   // unset: the default (mesh on mesh nodes, none elsewhere)
    std::optional<ecs::Motion> body;         // unset: static
    std::optional<float> mass, fric, rest;   // unset: 10 kg (dynamic only), 0.6, 0
    bool trigger = false;                    // trig 1: the collider is a trigger volume
    bool visible = true;                     // vis 0: collision only, no MeshSource
    std::string spawn, socket;               // empty: none
    std::string surface;                     // surf: the collider's surface (footsteps); empty: none
    std::string verb;                        // verb: ReverbZone{verb} on the node (a trigger box unless col says otherwise)
    std::string vfx;                         // vfx: a VfxEmitter{vfx} on the node (one spawn where it stands; sub-project 12)
    // Passed-through components, in key order: the type and its value as compact JSON.
    std::vector<std::pair<const reflect::TypeInfo*, std::string>> components;
};

// The hints of node `node` (an index into s.nodes). Diagnostics are appended to `out`; the
// default scene's extras are judged with node 0, so each problem there is reported once.
[[nodiscard]] LevelHints parseHints(const GltfStructure& s, int node, const reflect::TypeRegistry& types,
                                    std::vector<reflect::Diagnostic>& out);

// The hint keys as a JSON Schema for one node's extras: `ohao schema level-extras`, checked in
// as tests/level/extras_schema.json. Pretty, ending with a newline.
[[nodiscard]] std::string extrasJsonSchema();

} // namespace ohao::level
