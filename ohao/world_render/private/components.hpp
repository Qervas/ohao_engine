#pragma once
// The world_render module's own bookkeeping components: never written outside it.
#include <cstdint>
#include "ecs/storage.hpp"   // OHAO_EXTERN_STORAGE: the storages below are compiled once

namespace ohao::ecs {

// The RenderScene instance slot of a renderable entity. Owned by the render extract: it emplaces and removes it.
struct RenderSlot { uint32_t slot; };

// Instantiate's bookkeeping (world_render/instantiate.hpp): tagged when MeshSource or Material change.
struct InstantiateDirty {};

} // namespace ohao::ecs

// The storages of these components, compiled once (world_render/storage.cpp).
OHAO_EXTERN_STORAGE(ohao::ecs::InstantiateDirty);
OHAO_EXTERN_STORAGE(ohao::ecs::RenderSlot);
