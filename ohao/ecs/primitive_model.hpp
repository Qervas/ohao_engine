#pragma once
// The geometry a primitive MeshSource describes: what instantiate uploads, and what the physics system fits
// colliders to. Null for a ModelAsset, which the asset resolver loads.
#include "ecs/components.hpp"

#include <memory>

namespace ohao { class Model; }

namespace ohao::ecs {

[[nodiscard]] std::shared_ptr<const Model> primitiveModel(const MeshSource& source);

} // namespace ohao::ecs
