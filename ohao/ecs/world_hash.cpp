#include "ecs/world_hash.hpp"

#include "ecs/scene_types.hpp"
#include "ecs/snapshot_hook.hpp"
#include "reflect/hash.hpp"

#include <stdexcept>
#include <vector>

namespace ohao::ecs {

namespace {

template <class T>
void hashArray(reflect::Hasher& h, const std::vector<T>& v) {
    h.u64(v.size());
    h.words(v.data(), v.size() * sizeof(T));
}

// A hook's StateHash over world.hash's hasher.
class HookHash final : public StateHash {
public:
    HookHash(reflect::Hasher& h, const World& w) : m_h(h), m_world(w) {}
    void u64(uint64_t v) override { m_h.u64(v); }
    void f32(float v) override { m_h.f32(v); }
    void string(std::string_view s) override { m_h.string(s); }
    void entity(entt::entity e) override {
        m_h.u64(e != entt::null && m_world.registry().valid(e) ? m_world.idOf(e) : 0);
    }

private:
    reflect::Hasher& m_h;
    const World& m_world;
};

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
    // What the components do not hold (step 3b, complete state): the systems' registered state and the input.
    HookHash hooks(h, world);
    hashSystems(reg, hooks);
    h.u64(world.inputs().size());
    for (const InputMap& in : world.inputs()) in.hashState(h);
    // The World's step and the events the next step reads (step 3c), by channel name and payload; never a seq.
    h.u64(world.steps());
    world.events().visitWindow([&](const EventChannelBase& c, std::size_t i) {
        const reflect::TypeInfo* t = types.find(c.type());
        if (!t) throw std::logic_error("world.hash: event channel '" + c.name() + "' has an unregistered payload type");
        h.string(c.name());
        reflect::hashStruct(c.payload(i), h, *t);
    });
    return h.finish();
}

} // namespace ohao::ecs
