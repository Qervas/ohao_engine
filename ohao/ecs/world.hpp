#pragma once
#include "core/input.hpp"
#include "ecs/components.hpp"
#include "ecs/events.hpp"

#include <ankerl/unordered_dense.h>
#include <entt/entt.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::ecs {

// A restore put the world back to another moment (restoreSnapshot): `world.restore`, in the history only (emitted
// before the snapshot's next-step events come back, so no step reads it); the HUD and agents read it there.
inline constexpr const char* kRestoreChannel = "world.restore";
struct RestoreEvent {
    uint64_t step{0};   // the World's step the world went back to
};

// Where a system runs among its phase's others (step 3h): it runs after every system it names in `after` and before
// every one in `before`, when that system is installed (a name not installed is no constraint). In the simulate
// phase every event writer also runs before the channel's typed readers (EventLog::writer/reader name them) and,
// when the channel has a cue, before every system that reads cues.
struct SystemOrder {
    std::vector<std::string> after;
    std::vector<std::string> before;
    // It reads channels by their cues, which its data binds (the audio schedule, vfx): it runs after the writers of
    // every channel with a cue, whatever its data binds, so loading data never moves a system.
    bool readsCues{false};
};

// The game world: an EnTT registry plus the systems of two phases (sub-project 4 spec, "System phases").
//   simulate  behaviour (spin, physics, gameplay): once per fixed step, with the fixed dt. simulate(dt) runs them.
//   sync      derived state (instantiate, transform, the render extract): once before each render and after each
//             batch of steps, with no dt. sync() runs them.
// A frame's simulation is then independent of how often it is rendered: a realtime session and a lockstep one that
// run the same steps reach the same state. Each phase's order is computed from what its systems declare
// (SystemOrder, the event edges), ties going to the lesser name: a function of the systems installed, never of who
// installed them or when.
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
    // creates nothing when id is 0 or already in use. `hint`: the handle to take when it is free (a snapshot
    // restore recreates an entity as it was, so references held as handles stay valid).
    entt::entity createWithId(uint64_t id, std::string_view name, const Transform& local = {},
                              entt::entity hint = entt::null);

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

    // Installs a system at the place its order and the others' give it. A name already installed, an order with a
    // cycle, or a simulate system added inside a step throws std::logic_error and installs nothing.
    void addSimulate(std::string name, SimulateSystem fn, SystemOrder order = {});
    void addSync(std::string name, SyncSystem fn, SystemOrder order = {});
    // Removes a system (a game's, before its next build installs: game/reload.hpp); a simulate system also leaves the
    // event log (EventLog::forgetSystem). Between steps; a name not installed does nothing.
    void removeSimulate(std::string_view name);
    void removeSync(std::string_view name);
    [[nodiscard]] std::vector<std::string> simulateNames() const;   // in their order
    [[nodiscard]] std::vector<std::string> syncNames() const;
    // One fixed step: every simulate system, in order. The step's events become history after it (ecs/events.hpp),
    // then what the step deferred runs. When systems took new event handles since the last step, the order is
    // computed again first; a cycle then throws std::logic_error, before the step.
    void simulate(float dt);
    // Work that must happen between steps (a snapshot, a restore: step 3d): asked during a step, it runs after the
    // step's last system, in the order asked, with the step's events already history; asked between steps, at once.
    // Nothing snapshots or restores the world inside a step.
    void defer(std::function<void(World&)> op);
    // The steps simulated (saved and hashed): the step running, or the last one between steps.
    [[nodiscard]] uint64_t steps() const noexcept { return m_events.step(); }
    // The World's clock (step 3e): the fixed dt of the step running, else of the last one (1/60 s before any). A
    // system reads time from it, never from a constant; durations in data (seconds, milliseconds) become steps of
    // it. The host's configuration, so never saved or hashed.
    [[nodiscard]] float stepDt() const noexcept { return m_stepDt; }
    void setSteps(uint64_t steps) noexcept { m_events.setStep(steps); }   // a restore
    // The world's events (ecs/events.hpp): channels, the step's events, the history.
    [[nodiscard]] EventLog& events() noexcept { return m_events; }
    [[nodiscard]] const EventLog& events() const noexcept { return m_events; }
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

    // The world's input (core/input.hpp), read by simulate systems. Hosts step it once per
    // simulation step, before simulate(), with that step's events (stepInput). Its bindings
    // follow the InputActions singleton: rebuilt at the next stepInput whenever one is set,
    // changed or removed. A world without InputActions keeps bindings made in code.
    // One stream per player (step 3f): a PlayerController reads `stream` (PlayerController.stream), an event goes to
    // its InputEvent::stream, a device's to 0. A stream is made on first use with stream 0's bindings; one never
    // made reads as nothing held.
    static constexpr std::size_t kMaxInputStreams = 8;
    [[nodiscard]] InputMap& input(std::size_t stream = 0);
    [[nodiscard]] const InputMap& input(std::size_t stream = 0) const noexcept;
    [[nodiscard]] std::span<const InputMap> inputs() const noexcept { return m_inputs; }
    void setInputs(std::vector<InputMap> inputs);   // a restore's (at least one)
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
    // The wall time (s) since the last frame a realtime host drew, for presentation that moves at
    // render rate (the gun's sway, bob and kick, ecs/view_model.hpp). 0, the default, is a lockstep
    // or headless frame: presentation at rest, so captures stay the step's.
    void setPresentationDt(float seconds) noexcept { m_presentationDt = seconds; }
    [[nodiscard]] float presentationDt() const noexcept { return m_presentationDt; }
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
    entt::entity createImpl(uint64_t id, std::string_view name, const Transform& local, entt::entity hint = entt::null);
    uint64_t nextId();
    bool claim(uint64_t id);

    entt::registry m_registry;
    ankerl::unordered_dense::map<uint64_t, entt::entity> m_byId;
    entt::entity* m_claimed{nullptr};   // the slot claim() reserved for the entity being created
    IdGenState m_idGen;
    template <class Fn>
    struct System {
        std::string name;
        Fn fn;
        SystemOrder order;
    };
    void orderSimulate();
    std::vector<System<SimulateSystem>> m_simulate;   // in their order
    std::vector<System<SyncSystem>> m_sync;
    uint64_t m_simulateTopology{~uint64_t{0}};   // the event log's topology m_simulate was ordered with
    std::vector<InputMap> m_inputs = std::vector<InputMap>(1);   // by stream
    EventLog m_events;
    std::vector<std::function<void(World&)>> m_deferred;
    float m_stepDt{1.0f / 60.0f};
    bool m_bindingsDirty{false};
    float m_renderAlpha{1.0f};
    glm::vec2 m_pendingLook{0.0f};
    float m_presentationDt{0.0f};
    uint64_t m_nextSeq{0};
    std::size_t m_transformMismatches{0};
};

} // namespace ohao::ecs
