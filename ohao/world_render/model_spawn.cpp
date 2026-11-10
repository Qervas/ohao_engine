#include "world_render/model_spawn.hpp"

#include "asset/model.hpp"
#include "render/scene/model_upload.hpp"

#include <utility>

namespace ohao::ecs {

SpawnedModel uploadModel(render::RenderScene& scene, BindlessTextureManager* textures, const Model& model,
                         std::string_view debugName, const render::MaterialDesc* overrideMaterial) {
    render::UploadedModel up = render::uploadModel(scene, textures, model, debugName, overrideMaterial);
    return SpawnedModel{up.mesh, std::move(up.materials)};
}

void attachModel(World& world, entt::entity e, render::RenderScene& scene, BindlessTextureManager* textures,
                 const Model& model, std::string_view debugName, const render::MaterialDesc* overrideMaterial) {
    SpawnedModel up = ecs::uploadModel(scene, textures, model, debugName, overrideMaterial);
    auto& reg = world.registry();
    reg.emplace_or_replace<MeshRenderer>(e, MeshRenderer{up.mesh, std::move(up.materials), true});
}

entt::entity spawnModel(World& world, render::RenderScene& scene, BindlessTextureManager* textures,
                        const Model& model, std::string_view name, const Transform& transform,
                        const render::MaterialDesc* overrideMaterial) {
    const entt::entity e = world.create(name, transform);
    attachModel(world, e, scene, textures, model, name, overrideMaterial);
    return e;
}

} // namespace ohao::ecs
