#pragma once
// A cooked mesh (asset/cooked.hpp) into the render scene: its geometry, and its materials with their textures.
#include "asset/cooked.hpp"
#include "render/scene/render_scene_types.hpp"

#include <functional>
#include <string_view>
#include <vector>

namespace ohao { class BindlessTextureManager; }

namespace ohao::render {

class RenderScene;

// The geometry as one mesh (one BLAS), with a submesh per run of triangles of one material slot.
MeshHandle uploadMesh(RenderScene& scene, const cooked::Mesh& mesh, std::string_view debugName);

// Opens a texture a material names (ecs::AssetResolver::texture).
using TextureSource = std::function<Result<cooked::Texture>(const ContentHash&)>;

// One desc per material of the mesh. Each texture is the table entry of its content, uploaded the first time any
// mesh asks for it (BindlessTextureManager::contentTexture); `held` lists one hold per texture a material uses, for
// the caller to give back (releaseContent) when the materials go. `textures` null: the materials carry no textures.
struct MeshMaterials {
    std::vector<MaterialDesc> descs;
    std::vector<uint32_t> held;   // table indices
};
[[nodiscard]] MeshMaterials meshMaterials(BindlessTextureManager* textures, const cooked::Mesh& mesh,
                                          const TextureSource& open);

} // namespace ohao::render
