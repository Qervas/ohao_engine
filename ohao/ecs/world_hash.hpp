#pragma once
// world.hash (sub-project 4 spec, "Determinism"): one 64-bit value for a world's whole
// authoring state, so a lockstep run can be checked for reproducibility in one call.
//
// It hashes what a snapshot (ecs/snapshot.hpp) captures, read straight from the live
// storages in the snapshot's row (creation) order, without copying the values: the entity
// table (ids, creation sequence, parent rows, child rows in Children order), then for each
// saved component type, and the preserved unknown components, which rows have it and its
// values in row order (field by field, reflect/hash.hpp; a type whose bytes are exactly
// its fields, such as Transform, as its bytes), then the id generator state and the next
// creation sequence. Packed storage order is not hashed: it depends on the pools'
// history, not on the state. Runtime components (WorldTransform, render state, dirty
// tags) are not authoring state and are not hashed; neither is the frame number, which
// world.hash reports beside the hash.
#include "ecs/snapshot.hpp"
#include "ecs/world.hpp"
#include "reflect/registry.hpp"

#include <cstdint>

namespace ohao::ecs {

// `table` receives the entity table (its memory reused; no columns are captured).
[[nodiscard]] uint64_t hashWorld(const World& world, const reflect::TypeRegistry& types, Snapshot& table);

} // namespace ohao::ecs
