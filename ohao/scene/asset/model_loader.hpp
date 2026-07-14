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

    /// Extension without leading dot, lowercased (e.g. "glb").
    [[nodiscard]] static std::string getExtension(std::string_view path);

    [[nodiscard]] static bool isSupportedExtension(std::string_view ext);

private:
    [[nodiscard]] static std::shared_ptr<Model> loadFBX(std::string_view path);
    [[nodiscard]] static std::shared_ptr<Model> loadAssimp(std::string_view path);
    [[nodiscard]] static std::shared_ptr<Model> loadOBJ(std::string_view path);
};

} // namespace ohao
