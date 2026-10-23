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
    std::optional<EmissiveProxy> emissive;
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
// each of modelMaterials + emissiveProxy.
//   uploadModelMesh: the geometry, submeshes from material runs, one BLAS.
//   modelMaterials: one desc per model material (its textures uploaded), or for a model
//     without materials the override (or MaterialDesc{}); each with its legacy RT tint.
MeshHandle uploadModelMesh(RenderScene& scene, const Model& model, std::string_view debugName);
[[nodiscard]] std::vector<MaterialDesc> modelMaterials(BindlessTextureManager* textures, const Model& model,
                                                       const MaterialDesc* overrideMaterial);

// The emissive auto-light (VulkanRenderer::cacheEmissiveLights): the first material
// with an emissive texture index decides; nullopt unless its bright-pixel power > 0.1.
[[nodiscard]] std::optional<EmissiveProxy> emissiveProxy(const Model& model);

// LEGACY (see MaterialDesc::legacyRtTintTex): the bindless index of a 1x1
// R8G8B8A8_UNORM texture holding `baseColor` encoded to sRGB, exactly as the old RT
// texture array built it. One texture per distinct encoded color, shared by name.
// kNoTexture without a texture manager or when the upload fails.
[[nodiscard]] uint32_t legacyRtTintTexture(BindlessTextureManager* textures, const glm::vec3& baseColor);

// LEGACY RT GI material word (InstanceGpu::giAlbedoPacked), ported from
// VulkanRenderer::buildBLASTLAS. `overrideMaterial` is the entity's override material, if any.
[[nodiscard]] glm::vec4 giMaterialLegacyPacked(const Model& model, const MaterialDesc* overrideMaterial);
// What that word needs from the model, kept by callers that cache a model's mesh but
// not the model itself.
struct GiModelInputs {
    bool hasMaterials{false};
    float roughness0{0.0f};   // material 0, when hasMaterials
    float metallic0{0.0f};
    bool manyVertices{false};   // > 100 vertices
};
[[nodiscard]] GiModelInputs giModelInputs(const Model& model);
[[nodiscard]] glm::vec4 giMaterialLegacyPacked(const GiModelInputs& model, const MaterialDesc* overrideMaterial);

} // namespace ohao::render
