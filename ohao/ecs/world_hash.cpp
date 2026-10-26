#include "ecs/world_hash.hpp"

#include "ecs/scene_types.hpp"
#include "reflect/hash.hpp"

#include <vector>

namespace ohao::ecs {

namespace {

template <class T>
void hashArray(reflect::Hasher& h, const std::vector<T>& v) {
    h.u64(v.size());
    h.words(v.data(), v.size() * sizeof(T));
}

} // namespace

uint64_t hashWorld(const World& world, const reflect::TypeRegistry& types, Snapshot& table) {
    thread_local std::vector<uint32_t> rowOf;
    captureEntityTable(world, table, rowOf);
    const WorldEntityIds ids(world);
    reflect::Hasher h;
    h.entityIds = &ids;
    hashArray(h, table.ids);
    hashArray(h, table.order);
    hashArray(h, table.parent);
    hashArray(h, table.childBegin);
    hashArray(h, table.children);
    const entt::registry& reg = world.registry();
    for (const reflect::TypeInfo* t : snapshotTypes(types, false)) componentOps(*t)->hash(reg, rowOf, *t, h);
    h.u64(table.idGen.seed);
    h.u64(table.idGen.counter);
    h.u64(table.nextSeq);
    return h.finish();
}

} // namespace ohao::ecs
