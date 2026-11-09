#pragma once
// Instantiate: builds each entity's MeshRenderer from its MeshSource and Material, so scene files and
// commands describe meshes as data. The same data path as spawnModel.
//
//   - Trigger: constructing or replacing MeshSource or Material tags the entity
//     InstantiateDirty; removing Material re-instantiates with the default
//     material; removing MeshSource (or destroying the entity) removes what instantiate
//     made and releases the mesh.
//   - Meshes are cached per distinct MeshSource value with a user count, and released
//     (RenderScene::releaseMesh) when the last user goes. Materials are registered once
//     per distinct value (the material table is fixed-size and never frees).
//   - Failures (missing or unreadable asset, empty geometry) leave InstantiateError
//     {code, message} on the entity and no MeshRenderer; each is logged once.
//   - Dirty entities are processed in creation order, so meshes register in a
//     deterministic order.
#include "ecs/asset_resolver.hpp"
#include "ecs/world.hpp"
#include "core/render_handles.hpp"
#include "render/scene/render_scene_types.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace ohao { class BindlessTextureManager; class Model; }
namespace ohao::render { class RenderScene; }

namespace ohao::ecs {

// What instantiate needs from the renderer. installInstantiate(world, scene, ...) uses
// the RenderScene; tests and tools pass their own.
class InstantiateBackend {
public:
    virtual ~InstantiateBackend() = default;
    // The model's geometry as one mesh (render::uploadModelMesh). Invalid on failure.
    [[nodiscard]] virtual render::MeshHandle uploadMesh(const Model& model, std::string_view debugName) = 0;
    virtual void releaseMesh(render::MeshHandle mesh) = 0;
    // A model's own materials, textures uploaded (render::modelMaterials), registered.
    [[nodiscard]] virtual std::vector<render::MaterialHandle> modelMaterials(const Model& model) = 0;
    // One material; the backend adds the legacy RT tint (render::modelMaterials' rule).
    [[nodiscard]] virtual render::MaterialHandle material(const render::MaterialDesc& desc) = 0;
};

// Adds the "instantiate" system before "transform" (so a new mesh renders in the frame
// its MeshSource appears). Install once per World, after installRenderExtract; the
// scene, texture manager and resolver must outlive the World.
void installInstantiate(World& world, render::RenderScene& scene, BindlessTextureManager* textures,
                        AssetResolver& assets);
void installInstantiate(World& world, std::shared_ptr<InstantiateBackend> backend, AssetResolver& assets);

// Entities whose mesh could not be built (they carry InstantiateError), in creation order.
[[nodiscard]] std::vector<entt::entity> instantiateErrors(const World& world);

} // namespace ohao::ecs
