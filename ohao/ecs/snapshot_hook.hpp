#pragma once
// State a system keeps outside the components that a snapshot must carry to replay exactly (the physics scene's
// simulation, the audio schedule's and the vfx system's state). A system registers one hook under its name
// (addSnapshotHook); takeSnapshot (ecs/snapshot.hpp) keeps what each save returns, restoreSnapshot hands each back
// after the components, in name order whatever the install order: a hook restores its own system's state only, so
// the hooks are independent. A separate header so systems can register without the reflection layer.
#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace ohao::ecs {

// What a hook's hash is fed through: world.hash's hasher (ecs/world_hash.cpp), so a system hashes its state
// without the reflection layer. Floats count by their bits; an entity by its EntityId (0 for none).
class StateHash {
public:
    virtual ~StateHash() = default;
    virtual void u64(uint64_t v) = 0;
    virtual void f32(float v) = 0;
    virtual void string(std::string_view s) = 0;
    virtual void entity(entt::entity e) = 0;
    void u32(uint32_t v) { u64(v); }
    void boolean(bool b) { u64(b ? 1u : 0u); }
    void vec3(glm::vec3 v) { f32(v.x); f32(v.y); f32(v.z); }
};

// save keeps the system's state, restore puts one back, hash feeds the live state (what save would keep) to
// world.hash: a state that changes what later steps do is saved and hashed, whatever else it is. A log's seq is
// history, not state: a restore never rewinds a log, so neither its counter nor a reference into it is hashed.
struct SnapshotHook {
    std::string_view name;   // its system's
    std::function<std::shared_ptr<const void>()> save;
    std::function<void(const std::shared_ptr<const void>&)> restore;
    std::function<void(StateHash&)> hash;
};

// The registry's hooks, by name.
struct SnapshotHooks {
    std::vector<SnapshotHook> hooks;
};

// Registers `hook` in the registry's context, at its name's place; a name registered twice throws.
void addSnapshotHook(entt::registry& registry, SnapshotHook hook);

// What every hook saved, by name (Snapshot::systems). A hook with nothing saved is restored with null.
struct SystemStates {
    std::vector<std::pair<std::string_view, std::shared_ptr<const void>>> states;
};
[[nodiscard]] std::shared_ptr<const void> saveSystems(const entt::registry& registry);
void restoreSystems(const entt::registry& registry, const std::shared_ptr<const void>& saved);
// Every hook's live state, by name, each after its name (world.hash).
void hashSystems(const entt::registry& registry, StateHash& h);

} // namespace ohao::ecs
