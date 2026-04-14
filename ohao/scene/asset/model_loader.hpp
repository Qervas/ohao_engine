#pragma once

#include "model.hpp"
#include <string>
#include <memory>

namespace ohao {

// Unified model loader — handles FBX, GLB, GLTF, OBJ transparently.
// Tries the best loader for each format:
//   FBX  → ufbx (correct rotation orders, animation evaluation)
//   GLB  → Assimp (ufbx can't read GLB)
//   GLTF → Assimp (consistent with GLB path)
//   OBJ  → native parser (no skeleton/animation)
//
// All animated formats produce the same output:
//   - Full node tree in Skeleton (for correct joint matrices)
//   - ufbx scene pointer (FBX) or Assimp node tree (GLB/GLTF)
//   - Per-frame evaluation via Skeleton::evaluate(time)
class ModelLoader {
public:
    // Load any supported model format. Returns nullptr on failure.
    static std::shared_ptr<Model> load(const std::string& path);

private:
    // Format-specific loaders
    static std::shared_ptr<Model> loadFBX(const std::string& path);   // ufbx
    static std::shared_ptr<Model> loadAssimp(const std::string& path); // GLB, GLTF, Collada
    static std::shared_ptr<Model> loadOBJ(const std::string& path);   // native

    // Detect format from extension
    static std::string getExtension(const std::string& path);
};

} // namespace ohao
