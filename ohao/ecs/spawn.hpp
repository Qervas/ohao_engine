#pragma once
#include "ecs/world.hpp"
#include "render/scene/render_scene_types.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>
#include <string_view>
#include <vector>

namespace ohao { class BindlessTextureManager; class Model; }
namespace ohao::render { class RenderScene; }

namespace ohao::ecs {

struct SpawnedModel {
    render::MeshHandle mesh;
    // Indexed by the model's material index (= SubmeshDesc::materialSlot).
    std::vector<render::MaterialHandle> materials;
    std::optional<EmissiveProxyLight> emissive;
};

// Registers the model's mesh (one BLAS), its materials and textures in the scene.
// Materials and textures are derived exactly as the old deferred path did
// (GBufferPass::execute + VulkanRenderer::uploadDeferredTextures): one material per
// materialColors entry, submeshes from contiguous runs of materialPerTriangle. A model
// without materialColors gets one material, `overrideMaterial` or the default.
// `textures` may be null: the materials then carry no textures.
SpawnedModel uploadModel(render::RenderScene& scene, BindlessTextureManager* textures,
                         const Model& model, std::string_view debugName,
                         const render::MaterialDesc* overrideMaterial = nullptr);

// uploadModel, then an entity with MeshRenderer, GiMaterialLegacy and (when the model
// has a bright emissive texture) EmissiveProxyLight.
entt::entity spawnModel(World& world, render::RenderScene& scene, BindlessTextureManager* textures,
                        const Model& model, std::string_view name, const Transform& transform,
                        const render::MaterialDesc* overrideMaterial = nullptr);

// An entity with a Light. Position and direction (-Z) come from the transform.
entt::entity spawnLight(World& world, std::string_view name, const Light& light, const Transform& transform);

// Rotation taking -Z onto `direction` (need not be normalized). Up is +Y; for a
// vertical direction the right vector stays +X.
[[nodiscard]] glm::quat lookRotation(glm::vec3 direction);

} // namespace ohao::ecs
