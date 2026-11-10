#include "ecs/world.hpp"
#include "ecs/fly_controller_system.hpp"
#include "ecs/spin_system.hpp"
#include "ecs/transform_system.hpp"
#include "core/log.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <stdexcept>
#include <random>

namespace ohao::ecs {

namespace {

// splitmix64 (Steele, Lea, Flood 2014): a bijective 64-bit mixer, so for a fixed seed
// the ids seed+0, seed+1, ... never repeat before 2^64 creations.
constexpr uint64_t splitmix64(uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

uint64_t randomSeed() {
    std::random_device rd;
    return (static_cast<uint64_t>(rd()) << 32) ^ rd();
}

} // namespace

glm::mat4 Transform::matrix() const {
    return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation) *
           glm::scale(glm::mat4(1.0f), scale);
}

World::World() : m_idGen{randomSeed(), 0} {
    (void)m_events.writer<RestoreEvent>(kRestoreChannel, "world");   // its channel, before any restore
    m_registry.on_update<Transform>().connect<&World::onTransformChanged>(this);
    // The id index follows EntityId on every path (World::destroy, registry().destroy,
    // registry().clear). These hooks only touch m_byId, never a pool.
    m_registry.on_construct<EntityId>().connect<&World::onIdConstructed>(this);
    m_registry.on_destroy<EntityId>().connect<&World::onIdDestroyed>(this);
    m_registry.on_update<EntityId>().connect<&World::onIdUpdated>(this);
    // Hierarchy hooks keep Parent/Children consistent on every destroy path,
    // registry().destroy() and registry().clear() included. Pool creation order is
    // deliberate: registry().clear() empties pools newest first, so Children is
    // cleared while Parent and TransformDirty are still intact (the Children hook
    // writes to both) and they are emptied after it.
    m_registry.storage<TransformDirty>();
    m_registry.on_destroy<Parent>().connect<&World::onParentDestroyed>(this);
    m_registry.on_destroy<Children>().connect<&World::onChildrenDestroyed>(this);
    // The controls: the input map is rebuilt from InputActions at the next step.
    m_registry.on_construct<InputActions>().connect<&World::onInputActionsChanged>(this);
    m_registry.on_update<InputActions>().connect<&World::onInputActionsChanged>(this);
    m_registry.on_destroy<InputActions>().connect<&World::onInputActionsChanged>(this);
    // Engine-provided behaviour, once per fixed step: they move entities, before the physics and the gameplay that
    // read where entities are.
    addSimulate("spin", [](World& w, float dt) { updateSpin(w.registry(), dt); }, {.before = {"physics"}});
    addSimulate("fly_controller", [](World& w, float dt) { updateFlyControllers(w.registry(), w.input(), dt); },
                {.before = {"physics"}});
    // Structural: transforms are propagated (the other systems declare themselves around it).
    addSync("transform", [](World& w) { updateTransforms(w.registry()); });
}

void World::onTransformChanged(entt::registry& r, entt::entity e) {
    r.emplace_or_replace<TransformDirty>(e);
}

// A child loses its Parent (detached, or destroyed): its parent forgets it.
void World::onParentDestroyed(entt::registry& r, entt::entity child) {
    const entt::entity parent = r.get<Parent>(child).entity;
    if (!r.valid(parent)) return;
    if (auto* sibs = r.try_get<Children>(parent)) std::erase(sibs->entities, child);
}

// A parent's Children goes away while children remain (only registry().destroy()
// or remove<Children>() can do that; World::destroy takes the children first):
// detach them. Their local transform is kept and becomes their world transform.
void World::onChildrenDestroyed(entt::registry& r, entt::entity parent) {
    const std::vector<entt::entity> kids = r.get<Children>(parent).entities;
    for (const entt::entity c : kids) {
        if (!r.valid(c)) continue;
        if (const auto* p = r.try_get<Parent>(c); p && p->entity == parent) {
            r.remove<Parent>(c);
            r.emplace_or_replace<TransformDirty>(c);
        }
    }
}

void World::onInputActionsChanged(entt::registry&, entt::entity) { m_bindingsDirty = true; }

InputMap& World::input(std::size_t stream) {
    stream = std::min(stream, kMaxInputStreams - 1);
    while (m_inputs.size() <= stream) m_inputs.push_back(m_inputs.front().freshCopy());
    return m_inputs[stream];
}

const InputMap& World::input(std::size_t stream) const noexcept {
    static const InputMap kNothing;
    return stream < m_inputs.size() ? m_inputs[stream] : kNothing;
}

void World::setInputs(std::vector<InputMap> inputs) {
    if (inputs.empty()) inputs.emplace_back();
    m_inputs = std::move(inputs);
}

void World::stepInput(std::span<const InputEvent> events) {
    // Every stream the events name exists before the bindings are rebuilt, so all of them get them.
    for (const InputEvent& e : events) (void)input(e.stream);
    if (m_bindingsDirty) {
        m_bindingsDirty = false;
        const auto view = m_registry.view<const InputActions>();
        for (InputMap& in : m_inputs) {
            in.clearBindings();
            if (view.empty()) continue;
            const InputActions& a = view.get<const InputActions>(view.front());
            const Binding none{InputDevice::Key, 0};   // GLFW never reports key 0
            for (const ActionBinding& b : a.actions)
                if (const auto k = keyBinding(b.key)) in.bindAction(b.action, *k);
            for (const AxisBinding& b : a.axes)
                in.bindAxis(b.axis, keyBinding(b.positive).value_or(none), keyBinding(b.negative).value_or(none));
        }
    }
    // Each stream steps every step (its presses and releases of the last step clear), with its own events.
    if (m_inputs.size() == 1) {
        m_inputs.front().beginStep(events);
        return;
    }
    thread_local std::vector<InputEvent> mine;
    for (std::size_t k = 0; k < m_inputs.size(); ++k) {
        mine.clear();
        for (const InputEvent& e : events)
            if (std::min<std::size_t>(e.stream, kMaxInputStreams - 1) == k) mine.push_back(e);
        m_inputs[k].beginStep(mine);
    }
}

void World::onIdConstructed(entt::registry& r, entt::entity e) {
    if (m_claimed) {   // create/createWithId already reserved the slot: no second hash
        *m_claimed = e;
        m_claimed = nullptr;
        return;
    }
    const uint64_t id = r.get<EntityId>(e).value;
    if (!m_byId.try_emplace(id, e).second)
        OHAO_LOG(Error, ecs, "EntityId {} emplaced on a second entity; use World::createWithId", id);
}

void World::onIdDestroyed(entt::registry& r, entt::entity e) {
    const uint64_t id = r.get<EntityId>(e).value;
    if (auto it = m_byId.find(id); it != m_byId.end() && it->second == e) m_byId.erase(it);
}

// An id edited in place (EntityId should never be edited): re-index the entity.
void World::onIdUpdated(entt::registry& r, entt::entity e) {
    for (auto it = m_byId.begin(); it != m_byId.end(); ++it)
        if (it->second == e) { m_byId.erase(it); break; }
    onIdConstructed(r, e);
}

uint64_t World::nextId() {
    for (;;) {
        const uint64_t id = splitmix64(m_idGen.seed + m_idGen.counter++);
        if (id != 0 && claim(id)) return id;
    }
}

// Reserves the index slot for `id` (one hash operation); the EntityId hook of the entity
// created next fills it. False when the id is in use.
bool World::claim(uint64_t id) {
    const auto [it, fresh] = m_byId.try_emplace(id, entt::entity{entt::null});
    if (fresh) m_claimed = &it->second;
    return fresh;
}

entt::entity World::findById(uint64_t id) const {
    const auto it = m_byId.find(id);
    return it == m_byId.end() ? entt::null : it->second;
}

uint64_t World::idOf(entt::entity e) const {
    if (!m_registry.valid(e)) return 0;
    const auto* id = m_registry.try_get<EntityId>(e);
    return id ? id->value : 0;
}

void World::reserve(std::size_t entities) {
    m_byId.reserve(entities);
    m_registry.storage<entt::entity>().reserve(entities);
    m_registry.storage<EntityId>().reserve(entities);
    m_registry.storage<Name>().reserve(entities);
    m_registry.storage<Transform>().reserve(entities);
    m_registry.storage<Children>().reserve(entities);
    m_registry.storage<TransformDirty>().reserve(entities);
    m_registry.storage<CreationOrder>().reserve(entities);
}

entt::entity World::create(std::string_view name, const Transform& local) {
    return createImpl(nextId(), name, local);
}

entt::entity World::createWithId(uint64_t id, std::string_view name, const Transform& local, entt::entity hint) {
    if (id == 0 || !claim(id)) return entt::null;
    return createImpl(id, name, local, hint);
}

entt::entity World::createImpl(uint64_t id, std::string_view name, const Transform& local, entt::entity hint) {
    const entt::entity e = hint == entt::null ? m_registry.create() : m_registry.create(hint);
    m_registry.emplace<EntityId>(e, id);
    m_registry.emplace<Name>(e, std::string(name));
    m_registry.emplace<Transform>(e, local);
    m_registry.emplace<Children>(e);
    m_registry.emplace<TransformDirty>(e);
    m_registry.emplace<CreationOrder>(e, m_nextSeq++);
    return e;
}

void World::destroy(entt::entity e) {
    if (!m_registry.valid(e)) return;
    setParent(e, entt::null);
    // Depth-first over a copy: destroying children edits the vectors we walk.
    std::vector<entt::entity> stack{e};
    std::vector<entt::entity> order;
    while (!stack.empty()) {
        const entt::entity cur = stack.back();
        stack.pop_back();
        order.push_back(cur);
        if (const auto* ch = m_registry.try_get<Children>(cur))
            stack.insert(stack.end(), ch->entities.begin(), ch->entities.end());
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) m_registry.destroy(*it);
}

bool World::setParent(entt::entity child, entt::entity parent) {
    if (!m_registry.valid(child)) return false;
    // Reject cycles: a node cannot be its own ancestor.
    if (parent != entt::null && m_registry.valid(parent)) {
        for (entt::entity a = parent; a != entt::null && m_registry.valid(a);) {
            if (a == child) return false;
            const auto* p = m_registry.try_get<Parent>(a);
            a = p ? p->entity : entt::null;
        }
    }
    if (const auto* old = m_registry.try_get<Parent>(child)) {
        if (m_registry.valid(old->entity))
            if (auto* sibs = m_registry.try_get<Children>(old->entity))
                std::erase(sibs->entities, child);
        m_registry.remove<Parent>(child);
    }
    if (parent != entt::null && m_registry.valid(parent)) {
        m_registry.emplace<Parent>(child, parent);
        m_registry.get_or_emplace<Children>(parent).entities.push_back(child);
    }
    m_registry.emplace_or_replace<TransformDirty>(child);
    return true;
}

void World::attach(entt::entity child, entt::entity parent) {
    m_registry.emplace<Parent>(child, parent);
    m_registry.get_or_emplace<Children>(parent).entities.push_back(child);
    m_registry.emplace_or_replace<TransformDirty>(child);
}

void World::setTransform(entt::entity e, const Transform& local) {
    m_registry.patch<Transform>(e, [&](Transform& t) { t = local; });
}

namespace {

// The order of a phase's systems: each after what it names in `after` and before what it names in `before` (names
// not installed are ignored), and after its predecessors in `edges` (from, to: from runs first); among the systems
// ready at once the lesser name runs first (Kahn's algorithm), so the order depends only on the set. Returns the
// systems' indices in their order; a cycle throws, naming one.
template <class S>
std::vector<std::size_t> orderSystems(const std::vector<S>& systems, std::vector<std::pair<std::size_t, std::size_t>> edges,
                                      const char* kind) {
    const std::size_t n = systems.size();
    const auto indexOf = [&](std::string_view name) {
        for (std::size_t i = 0; i < n; ++i)
            if (systems[i].name == name) return i;
        return n;
    };
    for (std::size_t i = 0; i < n; ++i) {
        for (const std::string& a : systems[i].order.after)
            if (const std::size_t j = indexOf(a); j < n) edges.emplace_back(j, i);
        for (const std::string& b : systems[i].order.before)
            if (const std::size_t j = indexOf(b); j < n) edges.emplace_back(i, j);
    }
    std::vector<std::vector<std::size_t>> next(n), prev(n);
    std::vector<std::size_t> waits(n, 0);
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    for (const auto& [from, to] : edges) {
        if (from == to) continue;
        next[from].push_back(to);
        prev[to].push_back(from);
        ++waits[to];
    }
    std::vector<std::size_t> out;
    std::vector<bool> placed(n, false);
    while (out.size() < n) {
        std::size_t pick = n;
        for (std::size_t i = 0; i < n; ++i)
            if (!placed[i] && waits[i] == 0 && (pick == n || systems[i].name < systems[pick].name)) pick = i;
        if (pick == n) {   // every unplaced system waits on an unplaced one: walk back until one repeats
            std::vector<std::size_t> walk;
            std::size_t at = 0;
            while (placed[at]) ++at;
            while (std::find(walk.begin(), walk.end(), at) == walk.end()) {
                walk.push_back(at);
                at = *std::find_if(prev[at].begin(), prev[at].end(), [&](std::size_t p) { return !placed[p]; });
            }
            std::string cycle = systems[at].name;   // in run order: each must run before the next
            for (auto it = walk.rbegin(); *it != at; ++it) cycle += " -> " + systems[*it].name;
            throw std::logic_error(std::string("the ") + kind + " systems' order has a cycle: " + cycle + " -> " +
                                   systems[at].name);
        }
        placed[pick] = true;
        out.push_back(pick);
        for (const std::size_t j : next[pick]) --waits[j];
    }
    return out;
}

template <class S>
void permute(std::vector<S>& systems, const std::vector<std::size_t>& order) {
    std::vector<S> out;
    out.reserve(systems.size());
    for (const std::size_t i : order) out.push_back(std::move(systems[i]));
    systems = std::move(out);
}

template <class S>
std::string listNames(const std::vector<S>& systems) {
    std::string out;
    for (const S& s : systems) out += (out.empty() ? "" : ", ") + s.name;
    return out;
}

template <class S>
bool installed(const std::vector<S>& systems, std::string_view name) {
    return std::any_of(systems.begin(), systems.end(), [&](const S& s) { return s.name == name; });
}

} // namespace

// The simulate order, from the declarations and the event edges: a channel's writers before its typed readers and,
// when it has a cue, before every system that reads cues.
void World::orderSimulate() {
    const auto indexOf = [&](std::string_view name) {
        for (std::size_t i = 0; i < m_simulate.size(); ++i)
            if (m_simulate[i].name == name) return i;
        return m_simulate.size();
    };
    std::vector<std::pair<std::size_t, std::size_t>> edges;
    for (const auto& c : m_events.channels())
        for (const std::string& w : c->writers()) {
            const std::size_t from = indexOf(w);
            if (from == m_simulate.size()) continue;
            for (const std::string& r : c->readers())
                if (const std::size_t to = indexOf(r); to < m_simulate.size()) edges.emplace_back(from, to);
            if (c->hasCue())
                for (std::size_t to = 0; to < m_simulate.size(); ++to)
                    if (m_simulate[to].order.readsCues) edges.emplace_back(from, to);
        }
    permute(m_simulate, orderSystems(m_simulate, std::move(edges), "simulate"));
    m_simulateTopology = m_events.topology();
    OHAO_LOG(Debug, ecs, "simulate order: {}", listNames(m_simulate));
}

void World::addSimulate(std::string name, SimulateSystem fn, SystemOrder order) {
    if (m_events.inStep()) throw std::logic_error("simulate system '" + name + "' added inside a step");
    if (installed(m_simulate, name)) throw std::logic_error("simulate system '" + name + "' installed twice");
    m_simulate.push_back({name, std::move(fn), std::move(order)});
    try {
        orderSimulate();
    } catch (...) {
        m_simulate.erase(std::find_if(m_simulate.begin(), m_simulate.end(),
                                      [&](const auto& s) { return s.name == name; }));
        m_simulateTopology = ~uint64_t{0};   // the next step orders the rest again
        throw;
    }
}

void World::removeSimulate(std::string_view name) {
    if (m_events.inStep()) throw std::logic_error("simulate system '" + std::string(name) + "' removed inside a step");
    std::erase_if(m_simulate, [&](const auto& s) { return s.name == name; });
    m_events.forgetSystem(name);   // the order is computed again before the next step
}

void World::removeSync(std::string_view name) {
    std::erase_if(m_sync, [&](const auto& s) { return s.name == name; });
    permute(m_sync, orderSystems(m_sync, {}, "sync"));
}

std::vector<std::string> World::simulateNames() const {
    std::vector<std::string> out;
    for (const auto& s : m_simulate) out.push_back(s.name);
    return out;
}

std::vector<std::string> World::syncNames() const {
    std::vector<std::string> out;
    for (const auto& s : m_sync) out.push_back(s.name);
    return out;
}

void World::addSync(std::string name, SyncSystem fn, SystemOrder order) {
    if (installed(m_sync, name)) throw std::logic_error("sync system '" + name + "' installed twice");
    m_sync.push_back({name, std::move(fn), std::move(order)});
    try {
        permute(m_sync, orderSystems(m_sync, {}, "sync"));
    } catch (...) {
        m_sync.pop_back();
        throw;
    }
    OHAO_LOG(Debug, ecs, "sync order: {}", listNames(m_sync));
}

void World::simulate(float dt) {
    if (m_events.topology() != m_simulateTopology) orderSimulate();   // new event handles since the last step
    m_events.beginStep(m_events.step() + 1);
    if (dt > 0.0f) m_stepDt = dt;
    for (auto& s : m_simulate) s.fn(*this, dt);
    m_events.endStep();
    m_pendingLook = glm::vec2(0.0f);   // the step took it
    for (std::size_t i = 0; i < m_deferred.size(); ++i) {   // between steps now: an op may defer another (at once)
        auto op = std::move(m_deferred[i]);
        op(*this);
    }
    m_deferred.clear();
}

void World::defer(std::function<void(World&)> op) {
    if (m_events.inStep()) m_deferred.push_back(std::move(op));
    else op(*this);
}

void World::sync() {
#ifndef NDEBUG
    // A Transform written through get<>() instead of patch<>() is never propagated.
    if (m_transformMismatches == 0) {
        m_transformMismatches = verifyTransforms(m_registry);
        if (m_transformMismatches != 0)
            OHAO_LOG(Warn, ecs,
                     "{} entities have a WorldTransform that disagrees with their Transform: a "
                     "Transform was written without registry.patch<Transform>() or "
                     "World::setTransform() (reported once per World)",
                     m_transformMismatches);
    }
#endif
    for (auto& s : m_sync) s.fn(*this);
}

bool World::runSync(std::string_view name) {
    for (auto& s : m_sync)
        if (s.name == name) {
            s.fn(*this);
            return true;
        }
    return false;
}

} // namespace ohao::ecs
