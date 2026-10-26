#pragma once
// A glTF file's structure, read from its JSON (the level pipeline, sub-project 5,
// docs/specs/2026-10-26-level-pipeline-design.md, "glTF → world mapping"): the default
// scene's node tree with each node's TRS, mesh, camera and KHR_lights_punctual light,
// its `extras` verbatim, the lights and cameras themselves, and each mesh's primitive and
// triangle counts. Geometry and materials are not read here: ModelLoader loads them
// through Assimp, one glTF mesh at a time (loadMesh), using these primitive counts.
#include "core/result.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ohao::level {

struct GltfNode {
    std::string name;              // "" when unnamed; the path uses "#<index>"
    int parent = -1;               // node index, -1 at the scene root
    glm::vec3 translation{0.0f};
    glm::quat rotation{1, 0, 0, 0};
    glm::vec3 scale{1.0f};
    int mesh = -1, camera = -1, light = -1;   // light: KHR_lights_punctual index
    std::string extrasJson;        // the node's extras, compact JSON ("" if none)
};

struct GltfLight {
    enum class Type { Point, Spot, Directional } type{Type::Point};
    glm::vec3 color{1};
    float intensity = 1, range = 0, innerCone = 0, outerCone = 0.785398f;   // range 0: infinite
};

struct GltfCamera {
    float yfovRad = 0.8f, znear = 0.1f, zfar = 0.0f;   // zfar 0 = infinite
};

struct GltfMesh {
    std::string name;
    uint32_t primitives = 0;
    uint64_t triangles = 0;   // what loadMesh emits: triangles, strips and fans; lines and points are none
};

struct GltfStructure {
    std::vector<GltfNode> nodes;   // depth first from the default scene's roots, parents first
    std::vector<GltfMesh> meshes;
    std::vector<GltfLight> lights;
    std::vector<GltfCamera> cameras;
    std::string sceneExtrasJson, generator;
    std::vector<std::string> unsupported;   // "skins", "animations", ... (one entry per kind)
};

// code: E_ASSET_INVALID (not glTF 2.0, malformed JSON, an index out of range or a cycle),
// E_LEVEL_NODE (a node transform that is not translation, rotation and scale: shear or
// perspective), E_LEVEL_UNSUPPORTED (Draco or meshopt compression). path: the node's path
// for node errors, else "".
struct GltfError {
    std::string code, path, message;
};

// A .glb (its JSON chunk) or a .gltf, told apart by the binary header.
[[nodiscard]] Result<GltfStructure, GltfError> readGltfStructure(std::span<const uint8_t> fileBytes);

// "/Room/WallNorth": the names from the scene root down, "#<index>" for an unnamed node.
[[nodiscard]] std::string nodePath(const GltfStructure& s, int node);

} // namespace ohao::level
