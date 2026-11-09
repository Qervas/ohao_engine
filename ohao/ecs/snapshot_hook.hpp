#pragma once
// State a system keeps outside the components that a snapshot must carry to replay exactly (the physics scene's
// simulation, the audio schedule's and the vfx system's state). A system registers one hook under its name
// (addSnapshotHook); takeSnapshot (ecs/snapshot.hpp) keeps what each save returns, restoreSnapshot hands each back
// after the components, in kSnapshotOrder whatever the install order. A separate header so systems can register
// without the reflection layer.
#include <entt/entity/registry.hpp>

#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace ohao::ecs {

struct SnapshotHook {
    std::string_view name;   // a kSnapshotOrder entry
    std::function<std::shared_ptr<const void>()> save;
    std::function<void(const std::shared_ptr<const void>&)> restore;
};

// The registry's hooks, in kSnapshotOrder.
struct SnapshotHooks {
    std::vector<SnapshotHook> hooks;
};

// Registers `hook` in the registry's context at its place in kSnapshotOrder; a name not listed throws.
void addSnapshotHook(entt::registry& registry, SnapshotHook hook);

// What every hook saved, by name (Snapshot::systems). A hook with nothing saved is restored with null.
struct SystemStates {
    std::vector<std::pair<std::string_view, std::shared_ptr<const void>>> states;
};
[[nodiscard]] std::shared_ptr<const void> saveSystems(const entt::registry& registry);
void restoreSystems(const entt::registry& registry, const std::shared_ptr<const void>& saved);

} // namespace ohao::ecs
