#pragma once
// State a system keeps outside the components that a snapshot must carry to replay exactly: the
// physics scene's simulation (its backend's bodies, contact cache and characters;
// ecs/physics_scene.hpp). A system that has some puts a SnapshotHook in the registry's context:
// takeSnapshot (ecs/snapshot.hpp) keeps what save returns, restoreSnapshot hands it back after the
// components. A separate header so systems can install one without the reflection layer.
#include <functional>
#include <memory>

namespace ohao::ecs {

struct SnapshotHook {
    std::function<std::shared_ptr<const void>()> save;
    std::function<void(const std::shared_ptr<const void>&)> restore;
};

} // namespace ohao::ecs
