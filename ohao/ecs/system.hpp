#pragma once
// What a system declares (step 4b-4; docs/plans/2026-11-18-step4b-jobs.md, "Declared accesses and waves"): where it
// runs among its phase's others, and what it touches. The World orders, checks and groups its systems into waves
// from these (ecs/world.hpp); a system reaches components through its SystemContext, which the check mode holds to
// the declaration.
#include "core/type_key.hpp"

#include <entt/entt.hpp>

#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ohao::ecs {

// A component type a system or a signal names: its key, its name for messages, and how its storage is made (the World
// makes every declared storage between steps, so no system creates one inside a wave).
struct ComponentType {
    TypeKey key{0};
    std::string name;
    void (*assure)(entt::registry&){nullptr};
};

template <class C>
[[nodiscard]] ComponentType componentType() {
    using T = std::remove_const_t<C>;
    return {typeKey<T>(), std::string(typeName<T>()), [](entt::registry& r) { (void)r.storage<T>(); }};
}

// ecs::types<Health, Transform>(): the component types of a SystemDecl's reads or writes.
template <class... C>
[[nodiscard]] std::vector<ComponentType> types() {
    return {componentType<C>()...};
}

// Where a system runs and what it touches.
//   after, before  it runs after every system it names in `after` and before every one in `before`, when that system
//                  is installed (a name not installed is no constraint). In the simulate phase every event writer
//                  also runs before the channel's typed readers (EventLog::writer/reader name them) and, when the
//                  channel has a cue, before every system that reads cues.
//   readsCues      it reads channels by their cues, which its data binds (the audio schedule, vfx): it runs after the
//                  writers of every channel with a cue, whatever its data binds, so loading data never moves a system.
//   reads, writes  the component types it reads and writes (a write may also read). An emplace, a remove, a patch
//                  and a mutable get are writes; a system's writes also carry what their signals' handlers write
//                  (World::onConstruct, onUpdate, onDestroy).
//   readsResources, writesResources
//                  named state that is not a component: "physics.scene", "audio.mix", a system's own state that
//                  another reads.
//   exclusive      it runs alone in its wave and may use the raw registry: creating or destroying entities, or
//                  calling a subsystem with registry use of its own (physics, the level, instantiate), makes a
//                  system exclusive. It still declares what it writes, for the install check.
// Every system declares something: one with no reads, writes, resources and not exclusive is refused at install.
struct SystemDecl {
    std::vector<std::string> after;
    std::vector<std::string> before;
    bool readsCues{false};
    std::vector<ComponentType> reads;
    std::vector<ComponentType> writes;
    std::vector<std::string> readsResources;
    std::vector<std::string> writesResources;
    bool exclusive{false};
};

// What a signal's handler writes when it runs: it joins the access of every system that writes the signal's
// component.
struct SignalEffect {
    std::vector<ComponentType> writes;
    std::vector<std::string> writesResources;
};

} // namespace ohao::ecs
