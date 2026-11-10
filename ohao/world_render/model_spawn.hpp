#pragma once
// A model uploaded to the RenderScene and drawn by an entity: in-memory models (examples, tools) that have no
// MeshSource. Scene files describe meshes as MeshSource, which instantiate builds.
#include "ecs/world.hpp"
#include "render/scene/render_scene_types.hpp"

#include <entt/entt.hpp>

#include <string_view>
#include <vector>

namespace ohao { class BindlessTextureManager; class Model; }
namespace ohao::render { class RenderScene; }

namespace ohao::ecs {

struct SpawnedModel {
    render::MeshHandle mesh;
    // Indexed by the model's material index (= SubmeshDesc::materialSlot).
    std::vector<render::MaterialHandle> materials;
};

// render::uploadModel (render/scene/model_upload.hpp). See there for the material/texture/submesh
// derivation rules.
SpawnedModel uploadModel(render::RenderScene& scene, BindlessTextureManager* textures,
                         const Model& model, std::string_view debugName,
                         const render::MaterialDesc* overrideMaterial = nullptr);

// uploadModel, then MeshRenderer on `e`. An in-memory model: the entity has no MeshSource, so it
// saves without a mesh (W_UNSAVEABLE_MESH).
void attachModel(World& world, entt::entity e, render::RenderScene& scene, BindlessTextureManager* textures,
                 const Model& model, std::string_view debugName,
                 const render::MaterialDesc* overrideMaterial = nullptr);

// world.create, then attachModel.
entt::entity spawnModel(World& world, render::RenderScene& scene, BindlessTextureManager* textures,
                        const Model& model, std::string_view name, const Transform& transform,
                        const render::MaterialDesc* overrideMaterial = nullptr);

} // namespace ohao::ecs
