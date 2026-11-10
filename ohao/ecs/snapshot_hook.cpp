#include "ecs/snapshot_hook.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace ohao::ecs {

void addSnapshotHook(entt::registry& registry, SnapshotHook hook) {
    auto& hooks = registry.ctx().contains<SnapshotHooks>() ? registry.ctx().get<SnapshotHooks>().hooks
                                                           : registry.ctx().emplace<SnapshotHooks>().hooks;
    const auto at = std::find_if(hooks.begin(), hooks.end(), [&](const SnapshotHook& h) { return h.name >= hook.name; });
    if (at != hooks.end() && at->name == hook.name)
        throw std::logic_error("snapshot hook '" + std::string(hook.name) + "' registered twice");
    hooks.insert(at, std::move(hook));
}

std::shared_ptr<const void> saveSystems(const entt::registry& registry) {
    const auto* hooks = registry.ctx().find<SnapshotHooks>();
    if (!hooks || hooks->hooks.empty()) return nullptr;
    auto out = std::make_shared<SystemStates>();
    for (const SnapshotHook& h : hooks->hooks) out->states.emplace_back(h.name, h.save ? h.save() : nullptr);
    return out;
}

void hashSystems(const entt::registry& registry, StateHash& h) {
    const auto* hooks = registry.ctx().find<SnapshotHooks>();
    if (!hooks) return;
    for (const SnapshotHook& hook : hooks->hooks) {
        if (!hook.hash) continue;
        h.string(hook.name);
        hook.hash(h);
    }
}

void restoreSystems(const entt::registry& registry, const std::shared_ptr<const void>& saved) {
    const auto* hooks = registry.ctx().find<SnapshotHooks>();
    if (!hooks) return;
    const auto* states = static_cast<const SystemStates*>(saved.get());
    for (const SnapshotHook& h : hooks->hooks) {
        if (!h.restore) continue;
        std::shared_ptr<const void> state;
        if (states)
            for (const auto& [name, s] : states->states)
                if (name == h.name) state = s;
        h.restore(state);
    }
}

} // namespace ohao::ecs
