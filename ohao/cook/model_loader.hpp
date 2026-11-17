#pragma once

#include "cook/model.hpp"
#include "core/result.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct aiScene;
namespace Assimp { class Importer; }

namespace ohao {

// The importer: a glTF file (.glb, .gltf) read through Assimp. Only the cooker calls it (cook.cpp).
class ModelLoader {
public:
    /// The whole file as one Model, its nodes' transforms applied.
    [[nodiscard]] static Result<std::shared_ptr<Model>> loadResult(std::string_view path);

    /// Every mesh of a glTF file (.glb or .gltf) from one import, in the file's order: each untransformed (no
    /// node's transform), with only the materials its primitives use (renumbered from 0). A mesh that could not
    /// be made is null, with its error. The primitives of a mesh are found by the counts in the file's JSON
    /// (level/gltf_structure): Assimp makes one mesh per primitive, in glTF order.
    struct FileMeshes {
        std::vector<std::string> names;
        std::vector<std::shared_ptr<Model>> meshes;
        std::vector<std::string> errors;
    };
    [[nodiscard]] static Result<std::shared_ptr<const FileMeshes>> loadMeshes(std::string_view path);

    /// Extension without leading dot, lowercased (e.g. "glb").
    [[nodiscard]] static std::string getExtension(std::string_view path);

    [[nodiscard]] static bool isSupportedExtension(std::string_view ext);

private:
    [[nodiscard]] static std::shared_ptr<Model> load(std::string_view path);
    // Assimp's scene of a file, post-processed; perMesh: without the steps that split a mesh.
    [[nodiscard]] static const aiScene* importScene(Assimp::Importer& importer, const std::string& path, bool perMesh);
    [[nodiscard]] static std::shared_ptr<Model> loadAssimp(std::string_view path);
    // A Model of the scene's meshes [first, first + count) only, untransformed; nullptr: the node tree.
    struct MeshRange { uint32_t first = 0, count = 0, total = 0; };
    [[nodiscard]] static std::shared_ptr<Model> emitModel(const aiScene* scene, const std::string& path, const MeshRange* range);
};

} // namespace ohao
