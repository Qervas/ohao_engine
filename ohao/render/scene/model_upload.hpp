#pragma once
#include "render/scene/render_scene_types.hpp"

#include <glm/glm.hpp>

#include <optional>
#include <string_view>
#include <vector>

namespace ohao { class BindlessTextureManager; class Model; }

namespace ohao::render {

class RenderScene;

struct UploadedModel {
    MeshHandle mesh;
    // Indexed by the model's material index (= SubmeshDesc::materialSlot).
    std::vector<MaterialHandle> materials;
};

// Registers the model's mesh (one BLAS), its materials and textures in the scene.
// Materials and textures are derived exactly as the old deferred path did
// (GBufferPass::execute + VulkanRenderer::uploadDeferredTextures): one material per
// materialColors entry, submeshes from contiguous runs of materialPerTriangle. A model
// without materialColors gets one material, `overrideMaterial` or the default.
// `textures` may be null: the materials then carry no textures.
UploadedModel uploadModel(RenderScene& scene, BindlessTextureManager* textures, const Model& model,
                          std::string_view debugName, const MaterialDesc* overrideMaterial = nullptr);

// uploadModel in two parts, for callers that share a mesh between entities and register
// materials themselves (ecs instantiate). uploadModel == uploadModelMesh + registering
// each of modelMaterials.
//   uploadModelMesh: the geometry, submeshes from material runs, one BLAS.
//   modelMaterials: one desc per model material (its textures uploaded), or for a model
//     without materials the override (or MaterialDesc{}); each with its legacy RT tint.
MeshHandle uploadModelMesh(RenderScene& scene, const Model& model, std::string_view debugName);
[[nodiscard]] std::vector<MaterialDesc> modelMaterials(BindlessTextureManager* textures, const Model& model,
                                                       const MaterialDesc* overrideMaterial);

} // namespace ohao::render
