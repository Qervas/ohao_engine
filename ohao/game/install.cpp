#include "game/install.hpp"

#include "game/module.hpp"

#include "ecs/animation.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/snapshot_hook.hpp"
#include "ecs/world.hpp"
#include "reflect/registry.hpp"
#include "world_render/present.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>

namespace ohao::game {

namespace {

// What a game's install added (installGame), in the World's context.
struct InstalledGame {
    const Module* module{nullptr};
    std::vector<std::string> simulate, sync, hooks, drivers;
    std::vector<uint32_t> presenters;
    bool inputControl{false};
};

// Everything installGame watches, as it is now.
struct Inventory {
    std::vector<std::string> simulate, sync, hooks, drivers;
    std::vector<uint32_t> presenters;
    bool inputControl{false};
};

Inventory inventory(ecs::World& world) {
    Inventory v;
    v.simulate = world.simulateNames();
    v.sync = world.syncNames();
    for (const std::string_view h : ecs::snapshotHookNames(world.registry())) v.hooks.emplace_back(h);
    if (const ecs::AnimationSystem* a = ecs::animationSystem(world)) v.drivers = a->driverNames();
    v.presenters = ecs::presenterIds(world);
    if (const ecs::PhysicsScene* p = ecs::physicsScene(world)) v.inputControl = p->hasInputControl();
    return v;
}

template <class T>
std::vector<T> added(const std::vector<T>& before, const std::vector<T>& after) {
    std::vector<T> out;
    for (const T& x : after)
        if (std::find(before.begin(), before.end(), x) == before.end()) out.push_back(x);
    return out;
}

InstalledGame diff(const Module& module, const Inventory& before, const Inventory& after) {
    InstalledGame g;
    g.module = &module;
    g.simulate = added(before.simulate, after.simulate);
    g.sync = added(before.sync, after.sync);
    g.hooks = added(before.hooks, after.hooks);
    g.drivers = added(before.drivers, after.drivers);
    g.presenters = added(before.presenters, after.presenters);
    g.inputControl = after.inputControl && !before.inputControl;
    return g;
}

void remove(ecs::World& world, const InstalledGame& g) {
    for (const std::string& s : g.simulate) world.removeSimulate(s);
    for (const std::string& s : g.sync) world.removeSync(s);
    for (const std::string& h : g.hooks) ecs::removeSnapshotHook(world.registry(), h);
    if (ecs::AnimationSystem* a = ecs::animationSystem(world))
        for (const std::string& d : g.drivers) a->removeDriver(d);
    for (const uint32_t id : g.presenters) ecs::removePresenter(world, id);
    if (g.inputControl)
        if (ecs::PhysicsScene* p = ecs::physicsScene(world)) p->setInputControl({});
}

// What a reflected type is, as two builds must agree on it: its layout and, for an enum, its values.
std::string typeMismatch(const reflect::TypeRegistry& live, const reflect::TypeRegistry& next) {
    for (const reflect::TypeInfo* t : live.types()) {
        const reflect::TypeInfo* n = next.find(t->name);
        if (!n) return "the new build has no type " + t->name;
        if (n->layout != t->layout) return t->name + "'s layout changed";
    }
    for (const reflect::TypeInfo* n : next.types())
        if (!live.find(n->name)) return "the new build adds type " + n->name;
    for (const reflect::EnumInfo* e : live.enums()) {
        const reflect::EnumInfo* n = next.findEnum(e->name);
        if (!n) return "the new build has no enum " + e->name;
        if (n->values.size() != e->values.size()) return e->name + "'s values changed";
        for (std::size_t i = 0; i < e->values.size(); ++i)
            if (n->values[i].name != e->values[i].name || n->values[i].value != e->values[i].value)
                return e->name + "'s values changed";
    }
    return {};
}

} // namespace

void installGame(ecs::World& world, const Module& module) {
    if (world.registry().ctx().contains<InstalledGame>()) throw std::logic_error("a game is installed already");
    const Inventory before = inventory(world);
    try {
        module.install(world);
    } catch (...) {
        remove(world, diff(module, before, inventory(world)));
        module.uninstall(world);
        throw;
    }
    world.registry().ctx().emplace<InstalledGame>(diff(module, before, inventory(world)));
}

void uninstallGame(ecs::World& world) {
    auto& ctx = world.registry().ctx();
    const auto* g = ctx.find<InstalledGame>();
    if (!g) return;
    const InstalledGame record = *g;
    ctx.erase<InstalledGame>();
    remove(world, record);
    record.module->uninstall(world);
}

const Module* installedGame(const ecs::World& world) {
    const auto* g = world.registry().ctx().find<InstalledGame>();
    return g ? g->module : nullptr;
}

bool reloadGame(ecs::World& world, const reflect::TypeRegistry& types, const Module& next, ReloadReport& report,
                std::string& error) {
    const auto* g = world.registry().ctx().find<InstalledGame>();
    if (!g) {
        error = "no game is installed";
        return false;
    }
    if (world.events().inStep()) {
        error = "a reload runs between steps";
        return false;
    }
    // The types: the new build's, beside the engine's, against the registry the world's data was made with.
    {
        reflect::TypeRegistry scratch;
        ecs::registerSceneTypes(scratch);
        next.registerTypes(scratch);
        if (!scratch.errors().empty()) {
            error = "the new build's types do not register: " + scratch.errors().front();
            return false;
        }
        if (std::string why = typeMismatch(types, scratch); !why.empty()) {
            error = why + ": its data cannot be read in place; start a new session";
            return false;
        }
    }
    // The old build's hook states, each with its layout (the record goes with the uninstall: copied first).
    const Module& old = *g->module;
    const std::vector<std::string> oldHooks = g->hooks;
    struct Carried {
        uint64_t layout;
        std::shared_ptr<const void> state;
    };
    std::map<std::string, Carried> states;
    for (const std::string& name : oldHooks) {
        const ecs::SnapshotHook* h = ecs::findSnapshotHook(world.registry(), name);
        if (!h) continue;
        if (h->layout == 0) {
            error = "snapshot hook '" + name + "' declares no state layout (SnapshotHook::layout): its state cannot cross";
            return false;
        }
        states[name] = {h->layout, h->save ? h->save() : nullptr};
    }
    const auto restoreInto = [&](std::vector<std::string>* carried) {
        for (const auto& [name, c] : states)
            if (const ecs::SnapshotHook* h = ecs::findSnapshotHook(world.registry(), name); h && h->restore) {
                h->restore(c.state);
                if (carried) carried->push_back(name);
            }
    };
    const auto rollBack = [&](std::string why) {
        uninstallGame(world);
        installGame(world, old);
        restoreInto(nullptr);
        error = std::move(why) + "; the old build is installed again";
        return false;
    };
    uninstallGame(world);
    try {
        installGame(world, next);
    } catch (const std::exception& e) {
        return rollBack(std::string("the new build's install threw: ") + e.what());
    }
    for (const auto& [name, c] : states) {
        const ecs::SnapshotHook* h = ecs::findSnapshotHook(world.registry(), name);
        if (!h) return rollBack("the new build has no snapshot hook '" + name + "'");
        if (h->layout != c.layout) return rollBack("the state of snapshot hook '" + name + "' changed layout");
    }
    restoreInto(&report.carried);
    for (const std::string_view h : ecs::snapshotHookNames(world.registry()))
        if (const auto* now = world.registry().ctx().find<InstalledGame>();
            now && std::find(now->hooks.begin(), now->hooks.end(), std::string(h)) != now->hooks.end() &&
            !states.contains(std::string(h)))
            report.fresh.emplace_back(h);
    return true;
}

} // namespace ohao::game
