#pragma once
// The mesh of a MeshSource that is a primitive (a sphere, a box, ...), made in code: what instantiate uploads and
// what a mesh collider is built from. Null for a ModelAsset (the asset resolver loads those).
#include "asset/cooked.hpp"
#include "visual/components.hpp"

#include <memory>

namespace ohao::ecs {

[[nodiscard]] std::shared_ptr<const cooked::Mesh> primitiveMesh(const MeshSource& source);

} // namespace ohao::ecs
