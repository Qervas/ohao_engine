#include "ecs/spawn.hpp"
#include "render/scene/model_upload.hpp"
#include "scene/asset/model.hpp"

#include <cmath>
#include <utility>

namespace ohao::ecs {

namespace {

std::optional<EmissiveProxyLight> toComponent(const std::optional<render::EmissiveProxy>& p) {
    if (!p) return std::nullopt;
    return EmissiveProxyLight{p->localCenter, p->radius, p->color, p->intensity};
}

} // namespace

SpawnedModel uploadModel(render::RenderScene& scene, BindlessTextureManager* textures, const Model& model,
                         std::string_view debugName, const render::MaterialDesc* overrideMaterial) {
    render::UploadedModel up = render::uploadModel(scene, textures, model, debugName, overrideMaterial);
    return SpawnedModel{up.mesh, std::move(up.materials), toComponent(up.emissive)};
}

void attachModel(World& world, entt::entity e, render::RenderScene& scene, BindlessTextureManager* textures,
                 const Model& model, std::string_view debugName, const render::MaterialDesc* overrideMaterial) {
    SpawnedModel up = ecs::uploadModel(scene, textures, model, debugName, overrideMaterial);
    auto& reg = world.registry();
    reg.emplace_or_replace<MeshRenderer>(e, MeshRenderer{up.mesh, std::move(up.materials), true});
    if (up.emissive) reg.emplace_or_replace<EmissiveProxyLight>(e, *up.emissive);
}

entt::entity spawnModel(World& world, render::RenderScene& scene, BindlessTextureManager* textures,
                        const Model& model, std::string_view name, const Transform& transform,
                        const render::MaterialDesc* overrideMaterial) {
    const entt::entity e = world.create(name, transform);
    attachModel(world, e, scene, textures, model, name, overrideMaterial);
    return e;
}

entt::entity spawnLight(World& world, std::string_view name, const Light& light, const Transform& transform) {
    const entt::entity e = world.create(name, transform);
    world.registry().emplace<Light>(e, light);
    return e;
}

glm::quat lookRotation(glm::vec3 direction) {
    const glm::vec3 f = glm::normalize(direction);
    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(f.y) > 0.999f) up = glm::vec3(0.0f, 0.0f, f.y > 0.0f ? 1.0f : -1.0f);
    const glm::vec3 right = glm::normalize(glm::cross(f, up));
    const glm::vec3 trueUp = glm::cross(right, f);
    // Columns: local X -> right, Y -> up, Z -> -forward.
    return glm::normalize(glm::quat_cast(glm::mat3(right, trueUp, -f)));
}

} // namespace ohao::ecs
