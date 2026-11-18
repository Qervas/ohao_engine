#pragma once
// Instantiate: builds each entity's MeshRenderer from its MeshSource and Material, so scene files and
// commands describe meshes as data. 
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

namespace ohao { class BindlessTextureManager; }
namespace ohao::render { class RenderScene; }

namespace ohao::ecs {


// Entities whose mesh could not be built (they carry InstantiateError), in creation order.
[[nodiscard]] std::vector<entt::entity> instantiateErrors(const World& world);

} // namespace ohao::ecs
