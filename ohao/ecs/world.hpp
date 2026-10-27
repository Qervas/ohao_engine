#pragma once
#include "core/input.hpp"
#include "ecs/components.hpp"

#include <ankerl/unordered_dense.h>
#include <entt/entt.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::ecs {

// The game world: an EnTT registry plus two explicitly ordered lists of systems, one per
// phase (sub-project 4 spec, "System phases"). Systems run in exactly the order they were
// added -- no discovery, no sorting.
//   simulate  behaviour (spin, later physics and gameplay): once per fixed step, with the
//             fixed dt. simulate(dt) runs them.
//   sync      derived state (instantiate, transform, the render extract): once before each
//             render and after each batch of steps, with no dt. sync() runs them.
// A frame's simulation is then independent of how often it is rendered: a realtime session
// and a lockstep one that run the same steps reach the same state.
class World {
public:
    using SimulateSystem = std::function<void(World&, float dt)>;
    using SyncSystem = std::function<void(World&)>;

    World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    [[nodiscard]] entt::registry& registry() noexcept { return m_registry; }
    [[nodiscard]] const entt::registry& registry() const noexcept { return m_registry; }

    // Creates an entity with a fresh EntityId, Name, Transform (tagged TransformDirty), an
    // empty Children and its CreationOrder.
    entt::entity create(std::string_view name, const Transform& local = {});
    // Same, with a caller-chosen id (from a file or a command). Returns entt::null and
    // creates nothing when id is 0 or already in use.
    entt::entity createWithId(uint64_t id, std::string_view name, const Transform& local = {});

    // Stable ids. Generated ids are splitmix64(seed + counter++), skipping 0 and ids in
    // use. The seed comes from std::random_device unless seedIds() fixes it; the whole
    // generator state is the (seed, counter) pair, so restoring it repeats future ids.
    struct IdGenState {
        uint64_t seed{0};
        uint64_t counter{0};
        bool operator==(const IdGenState&) const = default;
    };
    void seedIds(uint64_t seed) noexcept { m_idGen = IdGenState{seed, 0}; }
    [[nodiscard]] IdGenState idGenState() const noexcept { return m_idGen; }
    void setIdGenState(IdGenState s) noexcept { m_idGen = s; }
    // entt::null when no live entity has this id.
    [[nodiscard]] entt::entity findById(uint64_t id) const;
    // 0 when e is invalid or has no EntityId.
    [[nodiscard]] uint64_t idOf(entt::entity e) const;
    [[nodiscard]] std::size_t idCount() const noexcept { return m_byId.size(); }
    // Room for `entities` entities in the id index and the per-entity storages, before a
    // bulk create (scene load, snapshot restore).
    void reserve(std::size_t entities);
    // The CreationOrder the next create() assigns. Restoring it (with idGenState) undoes
    // the bookkeeping of creates that were rolled back.
    [[nodiscard]] uint64_t nextCreationSeq() const noexcept { return m_nextSeq; }
    void setNextCreationSeq(uint64_t seq) noexcept { m_nextSeq = seq; }
    // Destroys the entity and its whole subtree, detaching it from its parent first.
    // This is the preferred path. registry().destroy(e) is also kept consistent, with
    // different semantics: e's parent forgets it, and e's children are DETACHED (they
    // become roots and keep their local transforms) rather than destroyed.
    void destroy(entt::entity e);
    // parent == entt::null detaches. The local transform is kept (world changes).
    // Returns false and changes nothing if child is invalid or parent == child or a
    // descendant of child (a cycle).
    bool setParent(entt::entity child, entt::entity parent);
    // setParent for hierarchies already known to be acyclic (a validated scene file, a
    // snapshot): `child` has no parent, `parent` is valid and not `child`'s descendant.
    // No ancestor walk, so bulk loads stay linear.
    void attach(entt::entity child, entt::entity parent);
    void setTransform(entt::entity e, const Transform& local);

    void addSimulate(std::string name, SimulateSystem fn);
    void addSync(std::string name, SyncSystem fn);
    // Inserts right after / before the sync system named `anchor` (appends if there is none).
    void addSyncAfter(std::string_view anchor, std::string name, SyncSystem fn);
    void addSyncBefore(std::string_view anchor, std::string name, SyncSystem fn);
    // One fixed step: every simulate system, in order.
    void simulate(float dt);
    // Every sync system, in order.
    void sync();
    // One sync system by name (false when there is none): a session expands levels right
    // after scene.load this way, without running the rest of sync.
    bool runSync(std::string_view name);
    // simulate(dt) then sync(): one step and its derived state, for tests and tools.
    void update(float dt) {
        simulate(dt);
        sync();
    }
    [[nodiscard]] std::vector<std::string> simulateNames() const;
    [[nodiscard]] std::vector<std::string> syncNames() const;

    // The world's input (core/input.hpp), read by simulate systems. Hosts step it once per
    // simulation step, before simulate(), with that step's events (stepInput). Its bindings
    // follow the InputActions singleton: rebuilt at the next stepInput whenever one is set,
    // changed or removed. A world without InputActions keeps bindings made in code.
    [[nodiscard]] InputMap& input() noexcept { return m_input; }
    [[nodiscard]] const InputMap& input() const noexcept { return m_input; }
    void stepInput(std::span<const InputEvent> events);
    // How far past the last step the next render shows, as a fraction of a fixed step: the
    // `interpolate` sync system draws what the steps move between their last two step poses
    // at this fraction. 1 (the default) is the last step's pose exactly: lockstep and
    // headless hosts leave it there; a realtime host sets the timeline's alpha each frame.
    void setRenderAlpha(float alpha) noexcept { m_renderAlpha = alpha; }
    [[nodiscard]] float renderAlpha() const noexcept { return m_renderAlpha; }
    // The look the steps have not consumed yet (the mouse's movement since the last step, in look
    // units): the next render draws the input-driven player turned by it (the look lead, sub-project
    // 9), as the next step will turn it. Presentation only: no step reads it, and it is never saved,
    // hashed or snapshotted. A realtime host sets it each frame; simulate() clears it (the step
    // consumed it); lockstep and headless hosts leave it at 0.
    void setPendingLook(glm::vec2 units) noexcept { m_pendingLook = units; }
    [[nodiscard]] glm::vec2 pendingLook() const noexcept { return m_pendingLook; }
    // Debug builds: before running sync systems, sync() checks every untagged entity's
    // WorldTransform against its local Transform (verifyTransforms) and logs the first
    // nonzero result once per World. This is that result (0: none seen; always 0 in
    // Release, where the check is compiled out).
    [[nodiscard]] std::size_t transformMismatches() const noexcept { return m_transformMismatches; }

private:
    void onTransformChanged(entt::registry& r, entt::entity e);
    void onParentDestroyed(entt::registry& r, entt::entity child);
    void onChildrenDestroyed(entt::registry& r, entt::entity parent);
    void onIdConstructed(entt::registry& r, entt::entity e);
    void onIdDestroyed(entt::registry& r, entt::entity e);
    void onIdUpdated(entt::registry& r, entt::entity e);
    void onInputActionsChanged(entt::registry& r, entt::entity e);
    entt::entity createImpl(uint64_t id, std::string_view name, const Transform& local);
    uint64_t nextId();
    bool claim(uint64_t id);

    entt::registry m_registry;
    ankerl::unordered_dense::map<uint64_t, entt::entity> m_byId;
    entt::entity* m_claimed{nullptr};   // the slot claim() reserved for the entity being created
    IdGenState m_idGen;
    std::vector<std::pair<std::string, SimulateSystem>> m_simulate;
    std::vector<std::pair<std::string, SyncSystem>> m_sync;
    InputMap m_input;
    bool m_bindingsDirty{false};
    float m_renderAlpha{1.0f};
    glm::vec2 m_pendingLook{0.0f};
    uint64_t m_nextSeq{0};
    std::size_t m_transformMismatches{0};
};

} // namespace ohao::ecs
