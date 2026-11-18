#pragma once
#include "core/input.hpp"
#include "ecs/components.hpp"
#include "ecs/events.hpp"
#include "ecs/system.hpp"

#include <ankerl/unordered_dense.h>
#include <entt/entt.hpp>

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ohao::jobs {
class Graph;
}

namespace ohao::ecs {

// A restore put the world back to another moment (restoreSnapshot): `world.restore`, in the history only (emitted
// before the snapshot's next-step events come back, so no step reads it); the HUD and agents read it there.
inline constexpr const char* kRestoreChannel = "world.restore";
struct RestoreEvent {
    uint64_t step{0};   // the World's step the world went back to
};

class World;
class SystemContext;

// The game world: an EnTT registry plus the systems of two phases (sub-project 4 spec, "System phases").
//   simulate  behaviour (spin, physics, gameplay): once per fixed step, with the fixed dt. simulate(dt) runs them.
//   sync      derived state (instantiate, transform, the render extract): once before each render and after each
//             batch of steps, with no dt. sync() runs them.
// A frame's simulation is then independent of how often it is rendered: a realtime session and a lockstep one that
// run the same steps reach the same state. Each phase's order is computed from what its systems declare
// (SystemDecl, the event edges), ties going to the lesser name: a function of the systems installed, never of who
// installed them or when.
//
// Waves (step 4b): a phase runs as waves, each a run of consecutive systems in that order that conflict in nothing
// (no component or resource one writes and another touches, no order between them, none exclusive). A wave of
// several runs on the job pool: the longest (by its measured mean) runs on the calling thread, any other whose mean is
// at least kInlineUs goes to a worker, and the rest run on the calling thread in order; their events and defers wait in each system's stage (EventStage) and are flushed in
// the systems' order once the wave has run, so the step is what one thread gives at any thread count. A wave of one
// runs inline, unstaged. OHAO_WAVE_ORDER=reverse runs each wave's systems in reverse (a dependency the declarations
// miss then shows as a different world.hash). A system's
// access is its declaration plus what the handlers of the signals it writes write (onConstruct, onUpdate, onDestroy).
// Checked at install: a cycle, two writers of one component or resource with no order between them, and a system
// that declares nothing each throw std::logic_error and install nothing.
//
// The check mode (OHAO_ACCESS_CHECK=1; on in Debug unless 0): a system that is not exclusive reaches components only
// through its SystemContext, within its declaration; any other access, or World::registry() while it runs, throws
// std::logic_error naming the system and the component.
class World {
public:
    // A system shorter than this (its mean, measured) runs on the calling thread: a worker's wake (2.7 us measured in
    // 4b-1) and the join cost what it would save.
    static constexpr double kInlineUs = 5.0;

    using SimulateSystem = std::function<void(SystemContext&, float dt)>;
    using SyncSystem = std::function<void(SystemContext&)>;

    World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    ~World();

    // The raw registry: between steps, and for exclusive systems. In the check mode it throws while a system that is
    // not exclusive runs on this thread (that system reaches components through its SystemContext).
    [[nodiscard]] entt::registry& registry() {
        if (m_check) [[unlikely]] checkRaw();
        return m_registry;
    }
    [[nodiscard]] const entt::registry& registry() const {
        if (m_check) [[unlikely]] checkRaw();
        return m_registry;
    }
    // The registry's context entries (installed subsystems, requests between systems). Never checked: an entry is
    // named state a system declares as a resource.
    [[nodiscard]] entt::registry::context& context() noexcept { return m_registry.ctx(); }
    [[nodiscard]] const entt::registry::context& context() const noexcept { return m_registry.ctx(); }
    // Whether the check mode is on (OHAO_ACCESS_CHECK, read at construction).
    [[nodiscard]] bool accessCheck() const noexcept { return m_check; }

    // A signal's handler, connected to the registry: fn runs when a C is constructed, updated (patch, replace) or
    // destroyed. `effect` is what the handler writes, which joins the access of every system that writes C.
    // Instance: the handler's object (Fn a member of it), or omitted for a free function.
    template <class C, auto Fn, class Instance>
    void onConstruct(Instance* instance, SignalEffect effect) {
        m_registry.on_construct<C>().template connect<Fn>(instance);
        addEffect(componentType<C>(), instance, std::move(effect));
    }
    template <class C, auto Fn, class Instance>
    void onUpdate(Instance* instance, SignalEffect effect) {
        m_registry.on_update<C>().template connect<Fn>(instance);
        addEffect(componentType<C>(), instance, std::move(effect));
    }
    template <class C, auto Fn, class Instance>
    void onDestroy(Instance* instance, SignalEffect effect) {
        m_registry.on_destroy<C>().template connect<Fn>(instance);
        addEffect(componentType<C>(), instance, std::move(effect));
    }
    template <class C, auto Fn>
    void onConstruct(SignalEffect effect) {
        m_registry.on_construct<C>().template connect<Fn>();
        addEffect(componentType<C>(), &kHandlerTag<Fn>, std::move(effect));
    }
    template <class C, auto Fn>
    void onDestroy(SignalEffect effect) {
        m_registry.on_destroy<C>().template connect<Fn>();
        addEffect(componentType<C>(), &kHandlerTag<Fn>, std::move(effect));
    }
    // Every handler of `instance` on C's signals goes, with its effects.
    template <class C, class Instance>
    void disconnect(Instance* instance) {
        m_registry.on_construct<C>().disconnect(instance);
        m_registry.on_update<C>().disconnect(instance);
        m_registry.on_destroy<C>().disconnect(instance);
        removeEffects(typeKey<C>(), instance);
    }
    template <class C, auto Fn>
    void disconnect() {
        m_registry.on_construct<C>().template disconnect<Fn>();
        m_registry.on_update<C>().template disconnect<Fn>();
        m_registry.on_destroy<C>().template disconnect<Fn>();
        removeEffects(typeKey<C>(), &kHandlerTag<Fn>);
    }

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

    // Installs a system at the place its declaration and the others' give it. A name already installed, an order with
    // a cycle, two unordered writers, a declaration of nothing, or a simulate system added inside a step throws
    // std::logic_error and installs nothing.
    void addSimulate(std::string name, SimulateSystem fn, SystemDecl decl);
    void addSync(std::string name, SyncSystem fn, SystemDecl decl);
    // Removes a system (a game's, before its next build installs: game/reload.hpp); a simulate system also leaves the
    // event log (EventLog::forgetSystem). Between steps; a name not installed does nothing.
    void removeSimulate(std::string_view name);
    void removeSync(std::string_view name);
    [[nodiscard]] std::vector<std::string> simulateNames() const;   // in their order
    [[nodiscard]] std::vector<std::string> syncNames() const;
    // An installed system's context (null when none of that name): a subsystem that also answers between steps (an
    // API's query) reads the registry through its system's, so one declaration covers both.
    [[nodiscard]] SystemContext* systemContext(std::string_view name) noexcept;
    // Each phase's waves, in order, each its systems in order (computed again first when something changed).
    [[nodiscard]] std::vector<std::vector<std::string>> simulateWaves();
    [[nodiscard]] std::vector<std::vector<std::string>> syncWaves();
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
    friend class SystemContext;
    void onTransformChanged(entt::registry& r, entt::entity e);
    void onParentDestroyed(entt::registry& r, entt::entity child);
    void onChildrenDestroyed(entt::registry& r, entt::entity parent);
    void onIdConstructed(entt::registry& r, entt::entity e);
    void onIdDestroyed(entt::registry& r, entt::entity e);
    void onIdUpdated(entt::registry& r, entt::entity e);
    void onInputActionsChanged(entt::registry& r, entt::entity e);
    void checkRaw() const;
    void addEffect(ComponentType trigger, const void* owner, SignalEffect effect);
    void removeEffects(TypeKey trigger, const void* owner);
    template <auto Fn>
    static constexpr char kHandlerTag = 0;   // a free handler's identity among the effects
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
        SystemDecl decl;
        std::unique_ptr<SystemContext> ctx;
    };
    struct Effect {
        ComponentType trigger;
        const void* owner{nullptr};
        SignalEffect effect;
    };
    struct Wave {
        uint32_t begin{0}, end{0};   // the systems [begin, end) of the phase, in order
    };
    template <class S>
    void schedule(std::vector<S>& systems, std::vector<Wave>& waves, const char* kind, bool events);
    template <class S, class... A>
    void runWaves(std::vector<S>& systems, const std::vector<Wave>& waves, A... args);
    void scheduleSimulate();
    void scheduleSync();
    template <class S>
    static std::vector<std::vector<std::string>> waveNames(const std::vector<S>& systems, const std::vector<Wave>& waves);
    // Before the systems: their subsystems disconnect their handlers (and effects) as they go.
    std::vector<Effect> m_effects;
    bool m_simulateDirty{true}, m_syncDirty{true};   // a system or an effect came or went since the phase's schedule
    bool m_check{false};
    bool m_reverse{false};   // OHAO_WAVE_ORDER=reverse
    double m_inlineUs{kInlineUs};   // OHAO_WAVE_INLINE_US, which exists to measure it
    std::unique_ptr<jobs::Graph> m_graph;   // a wave's tasks
    std::vector<System<SimulateSystem>> m_simulate;   // in their order
    std::vector<System<SyncSystem>> m_sync;
    std::vector<Wave> m_simulateWaves, m_syncWaves;
    uint64_t m_simulateTopology{~uint64_t{0}};   // the event log's topology m_simulate was scheduled with
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

// The registry as a system sees it (step 4b-4): EnTT's names, each checked against the system's declaration in the
// check mode. Reads: valid, all_of, any_of, get, try_get, view with const types, exclude. Writes: get_mut, try_get_mut,
// view with mutable types, patch, replace, emplace, emplace_or_replace, get_or_emplace, remove. An emplace or a remove
// is a write of that component's storage alone; creating or destroying an entity is the raw registry's, an exclusive
// system's. The World, the events, the clock and the input come from world().
class SystemContext {
public:
    SystemContext(World& world, std::string name, bool exclusive);

    [[nodiscard]] World& world() const noexcept { return *m_world; }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] bool exclusive() const noexcept { return m_exclusive; }

    [[nodiscard]] bool valid(entt::entity e) const { return m_reg->valid(e); }
    // The entity's EntityId (0 when invalid or none): ids change only with entities, so reading one is no access.
    [[nodiscard]] uint64_t idOf(entt::entity e) const;
    template <class... C>
    [[nodiscard]] bool all_of(entt::entity e) const {
        (read<C>(), ...);
        return m_reg->all_of<std::remove_const_t<C>...>(e);
    }
    template <class... C>
    [[nodiscard]] bool any_of(entt::entity e) const {
        (read<C>(), ...);
        return m_reg->any_of<std::remove_const_t<C>...>(e);
    }
    template <class C>
    [[nodiscard]] const std::remove_const_t<C>& get(entt::entity e) const {
        read<C>();
        return std::as_const(*m_reg).get<std::remove_const_t<C>>(e);
    }
    template <class C>
    [[nodiscard]] const std::remove_const_t<C>* try_get(entt::entity e) const {
        read<C>();
        return std::as_const(*m_reg).try_get<std::remove_const_t<C>>(e);
    }
    // view<const A, B>(entt::exclude<X>): A read, B written, X read.
    template <class... C, class... X>
    [[nodiscard]] auto view(entt::exclude_t<X...> = entt::exclude_t<X...>{}) const {
        (touch<C>(), ...);
        (read<X>(), ...);
        return m_reg->view<C...>(entt::exclude<X...>);
    }

    template <class C>
    [[nodiscard]] C& get_mut(entt::entity e) const {
        write<C>();
        return m_reg->get<C>(e);
    }
    template <class C>
    [[nodiscard]] C* try_get_mut(entt::entity e) const {
        write<C>();
        return m_reg->try_get<C>(e);
    }
    template <class C, class... F>
    decltype(auto) patch(entt::entity e, F&&... fn) const {
        write<C>();
        return m_reg->patch<C>(e, std::forward<F>(fn)...);
    }
    template <class C, class... A>
    decltype(auto) replace(entt::entity e, A&&... args) const {
        write<C>();
        return m_reg->replace<C>(e, std::forward<A>(args)...);
    }
    template <class C, class... A>
    decltype(auto) emplace(entt::entity e, A&&... args) const {
        write<C>();
        return m_reg->emplace<C>(e, std::forward<A>(args)...);
    }
    template <class C, class... A>
    decltype(auto) emplace_or_replace(entt::entity e, A&&... args) const {
        write<C>();
        return m_reg->emplace_or_replace<C>(e, std::forward<A>(args)...);
    }
    template <class C, class... A>
    decltype(auto) get_or_emplace(entt::entity e, A&&... args) const {
        write<C>();
        return m_reg->get_or_emplace<C>(e, std::forward<A>(args)...);
    }
    template <class... C>
    std::size_t remove(entt::entity e) const {
        (write<C>(), ...);
        return m_reg->remove<C...>(e);
    }

    // Context entries: never checked (a resource the system declares).
    [[nodiscard]] entt::registry::context& ctx() const noexcept { return m_reg->ctx(); }
    // The raw registry: an exclusive system's (the check mode throws for any other).
    [[nodiscard]] entt::registry& registry() const;

private:
    friend class World;
    template <class C>
    void read() const {
        if (m_check) [[unlikely]] checkRead(typeKey<std::remove_const_t<C>>(), typeName<std::remove_const_t<C>>());
    }
    template <class C>
    void write() const {
        if (m_check) [[unlikely]] checkWrite(typeKey<std::remove_const_t<C>>(), typeName<std::remove_const_t<C>>());
    }
    template <class C>
    void touch() const {
        if constexpr (std::is_const_v<C>) read<C>();
        else write<C>();
    }
    void checkRead(TypeKey key, std::string_view type) const;
    void checkWrite(TypeKey key, std::string_view type) const;

    World* m_world;
    entt::registry* m_reg;
    std::string m_name;
    bool m_exclusive;
    bool m_check{false};
    // Running in a wave of several: its emits and defers wait here until the wave's end.
    bool m_staged{false};
    bool m_dispatch{false};   // this wave, on a worker
    EventStage m_stage;
    std::vector<std::function<void(World&)>> m_deferred;
    std::exception_ptr m_error;   // what it threw in its wave (rethrown after the wave, in order)
    double m_meanUs{0.0};         // its time, a running mean (the inline rule)
    std::vector<TypeKey> m_reads;    // sorted: what it may read (its reads and writes, with its signals' effects)
    std::vector<TypeKey> m_writes;   // sorted
};


} // namespace ohao::ecs
