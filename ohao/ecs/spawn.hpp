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

// render::uploadModel (render/scene/model_upload.hpp) with the emissive proxy as an
// ECS component. See there for the material/texture/submesh derivation rules.
SpawnedModel uploadModel(render::RenderScene& scene, BindlessTextureManager* textures,
                         const Model& model, std::string_view debugName,
                         const render::MaterialDesc* overrideMaterial = nullptr);

// uploadModel, then MeshRenderer, GiMaterialLegacy and (when the model has a bright
// emissive texture) EmissiveProxyLight on `e`. An in-memory model: the entity has no
// MeshSource, so it saves without a mesh (W_UNSAVEABLE_MESH).
void attachModel(World& world, entt::entity e, render::RenderScene& scene, BindlessTextureManager* textures,
                 const Model& model, std::string_view debugName,
                 const render::MaterialDesc* overrideMaterial = nullptr);

// world.create, then attachModel.
entt::entity spawnModel(World& world, render::RenderScene& scene, BindlessTextureManager* textures,
                        const Model& model, std::string_view name, const Transform& transform,
                        const render::MaterialDesc* overrideMaterial = nullptr);

// An entity with a Light. Position and direction (-Z) come from the transform.
entt::entity spawnLight(World& world, std::string_view name, const Light& light, const Transform& transform);

// Rotation taking -Z onto `direction` (need not be normalized). Up is +Y; for a
// vertical direction the right vector stays +X.
[[nodiscard]] glm::quat lookRotation(glm::vec3 direction);

} // namespace ohao::ecs
