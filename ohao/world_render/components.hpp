#pragma once
// The world_render module's public component (world_render/module.hpp): the GPU mesh and materials an entity renders
// with, which instantiate builds from its MeshSource and Material (visual/components.hpp).
#include "core/render_handles.hpp"

#include <vector>
#include "ecs/storage.hpp"   // OHAO_EXTERN_STORAGE: the storages below are compiled once

namespace ohao::ecs {

// One material per submesh, in submesh order. Fewer entries than submeshes is legal:
// the missing submeshes render with the default material.
struct MeshRenderer {
    render::MeshHandle mesh{};
    std::vector<render::MaterialHandle> materials;
    bool visible{true};
};

} // namespace ohao::ecs

// The storages of these components, compiled once (world_render/storage.cpp).
OHAO_EXTERN_STORAGE(ohao::ecs::MeshRenderer);
