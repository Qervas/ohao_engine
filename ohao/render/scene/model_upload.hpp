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

// The emissive auto-light (VulkanRenderer::cacheEmissiveLights): the first material
// with an emissive texture index decides; nullopt unless its bright-pixel power > 0.1.
[[nodiscard]] std::optional<EmissiveProxy> emissiveProxy(const Model& model);

// LEGACY RT GI material word (InstanceGpu::giAlbedoPacked), ported from
// VulkanRenderer::buildBLASTLAS. `overrideMaterial` is the actor's material, if any.
[[nodiscard]] glm::vec4 giMaterialLegacyPacked(const Model& model, const MaterialDesc* overrideMaterial);

} // namespace ohao::render
