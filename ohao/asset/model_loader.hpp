#pragma once

#include "model.hpp"
#include "core/result.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace ohao {

// Unified model loader — handles FBX, GLB, GLTF, OBJ transparently.
// Tries the best loader for each format:
//   FBX  → ufbx (correct rotation orders), retried through Assimp for cleanup
//   GLB  → Assimp (ufbx can't read GLB)
//   GLTF → Assimp (consistent with GLB path)
//   OBJ  → native parser
//
// Static geometry only — skeletal animation has been removed.
class ModelLoader {
public:
    /// Load any supported model format. Returns nullptr on failure.
    [[nodiscard]] static std::shared_ptr<Model> load(std::string_view path);

    /// Same as load(), with an error string on failure.
    [[nodiscard]] static Result<std::shared_ptr<Model>> loadResult(std::string_view path);

    /// One mesh of a glTF file (.glb or .gltf), by its name or "#<index>", untransformed (no
    /// node's transform), with only the materials its primitives use (renumbered from 0).
    /// The mesh's primitives are found by the counts in the file's JSON (level/gltf_structure):
    /// Assimp makes one mesh per primitive, in glTF order.
    [[nodiscard]] static Result<std::shared_ptr<Model>> loadMesh(std::string_view path, std::string_view mesh);

    /// Extension without leading dot, lowercased (e.g. "glb").
    [[nodiscard]] static std::string getExtension(std::string_view path);

    [[nodiscard]] static bool isSupportedExtension(std::string_view ext);

private:
    [[nodiscard]] static std::shared_ptr<Model> loadFBX(std::string_view path);
    // Aiscene meshes [first, first + count) only, untransformed; nullptr: the node tree.
    struct MeshRange { uint32_t first = 0, count = 0, total = 0; };
    [[nodiscard]] static std::shared_ptr<Model> loadAssimp(std::string_view path, const MeshRange* range = nullptr);
    [[nodiscard]] static std::shared_ptr<Model> loadOBJ(std::string_view path);
};

} // namespace ohao
