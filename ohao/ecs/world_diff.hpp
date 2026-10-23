#pragma once
// world.diff (sub-project 4 spec, "Method table"): what changed in the world since a
// snapshot, entity by entity and component by component, without writing anything. The
// API turns it into field paths with old and new values (ohao/api).
#include "ecs/snapshot.hpp"
#include "ecs/world.hpp"

#include <cstdint>
#include <vector>

namespace ohao::ecs {

struct ComponentDiff {
    const reflect::TypeInfo* type{nullptr};
    const void* before{nullptr};   // the snapshot's value; null: the entity did not have it
    const void* after{nullptr};    // the live value; null: the entity no longer has it
};

struct EntityDiff {
    uint64_t id{0};
    entt::entity entity{entt::null};   // live
    std::vector<ComponentDiff> components;   // in the snapshot's column order
    bool parentChanged{false};
    uint64_t parentBefore{0};   // parent ids; 0: a root
    uint64_t parentAfter{0};
};

struct WorldDiff {
    std::vector<EntityDiff> changed;    // entities in both, in the snapshot's row (creation) order
    std::vector<uint64_t> created;      // in the world, not the snapshot, in creation order
    std::vector<uint64_t> destroyed;    // in the snapshot, not the world, in row order
};

// `snapshot` must come from a world with the same registrations. Children order and
// CreationOrder are not compared (neither is a field of the file form).
void diffWorld(const World& world, const Snapshot& snapshot, WorldDiff& out);

} // namespace ohao::ecs
