#include "ecs/world.hpp"
#include "core/jobs/graph.hpp"
#include "core/log.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <random>
#include <stdexcept>

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

bool accessCheckFromEnvironment() {
    const char* v = std::getenv("OHAO_ACCESS_CHECK");
#ifdef NDEBUG
    return v && *v && std::string_view(v) != "0";
#else
    return !(v && std::string_view(v) == "0");
#endif
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

World::World()
    : m_idGen{randomSeed(), 0}, m_check(accessCheckFromEnvironment()), m_graph(std::make_unique<jobs::Graph>()) {
    if (const char* order = std::getenv("OHAO_WAVE_ORDER")) m_reverse = std::string_view(order) == "reverse";
    if (const char* us = std::getenv("OHAO_WAVE_INLINE_US")) m_inlineUs = std::atof(us);
    (void)m_events.writer<RestoreEvent>(kRestoreChannel, "world");   // its channel, before any restore
    onUpdate<Transform, &World::onTransformChanged>(this, {.writes = types<TransformDirty>()});
    // The id index follows EntityId on every path (World::destroy, registry().destroy,
    // registry().clear). These hooks only touch m_byId, never a pool.
    onConstruct<EntityId, &World::onIdConstructed>(this, {.writesResources = {"world.ids"}});
    onDestroy<EntityId, &World::onIdDestroyed>(this, {.writesResources = {"world.ids"}});
    onUpdate<EntityId, &World::onIdUpdated>(this, {.writesResources = {"world.ids"}});
    // Hierarchy hooks keep Parent/Children consistent on every destroy path,
    // registry().destroy() and registry().clear() included. Pool creation order is
    // deliberate: registry().clear() empties pools newest first, so Children is
    // cleared while Parent and TransformDirty are still intact (the Children hook
    // writes to both) and they are emptied after it.
    m_registry.storage<TransformDirty>();
    onDestroy<Parent, &World::onParentDestroyed>(this, {.writes = types<Children>()});
    onDestroy<Children, &World::onChildrenDestroyed>(this, {.writes = types<Parent, TransformDirty>()});
    // The controls: the input map is rebuilt from InputActions at the next step.
    const SignalEffect bindings{.writesResources = {"world.bindings"}};
    onConstruct<InputActions, &World::onInputActionsChanged>(this, bindings);
    onUpdate<InputActions, &World::onInputActionsChanged>(this, bindings);
    onDestroy<InputActions, &World::onInputActionsChanged>(this, bindings);
    // No system: the core module installs the World's own (ecs/core.cpp), every other module its.
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

thread_local SystemContext* t_running = nullptr;   // the system this thread runs now

// Sets the system this thread runs for the length of its call.
struct Running {
    SystemContext* prev;
    explicit Running(SystemContext* ctx) : prev(t_running) { t_running = ctx; }
    ~Running() { t_running = prev; }
};

// The order of a phase's systems: each after what it names in `after` and before what it names in `before` (names
// not installed are ignored), and after its predecessors in `edges` (from, to: from runs first); among the systems
// ready at once the lesser name runs first (Kahn's algorithm), so the order depends only on the set. Returns the
// systems' indices in their order, and `edges` becomes every edge (deduplicated); a cycle throws, naming one.
template <class S>
std::vector<std::size_t> orderSystems(const std::vector<S>& systems, std::vector<std::pair<std::size_t, std::size_t>>& edges,
                                      const char* kind) {
    const std::size_t n = systems.size();
    const auto indexOf = [&](std::string_view name) {
        for (std::size_t i = 0; i < n; ++i)
            if (systems[i].name == name) return i;
        return n;
    };
    for (std::size_t i = 0; i < n; ++i) {
        for (const std::string& a : systems[i].decl.after)
            if (const std::size_t j = indexOf(a); j < n) edges.emplace_back(j, i);
        for (const std::string& b : systems[i].decl.before)
            if (const std::size_t j = indexOf(b); j < n) edges.emplace_back(i, j);
    }
    std::erase_if(edges, [](const auto& e) { return e.first == e.second; });
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    std::vector<std::vector<std::size_t>> next(n), prev(n);
    std::vector<std::size_t> waits(n, 0);
    for (const auto& [from, to] : edges) {
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
bool installed(const std::vector<S>& systems, std::string_view name) {
    return std::any_of(systems.begin(), systems.end(), [&](const S& s) { return s.name == name; });
}

void insertSorted(std::vector<TypeKey>& keys, TypeKey k) {
    const auto it = std::lower_bound(keys.begin(), keys.end(), k);
    if (it == keys.end() || *it != k) keys.insert(it, k);
}

bool containsSorted(const std::vector<TypeKey>& keys, TypeKey k) { return std::binary_search(keys.begin(), keys.end(), k); }

// A system's access with its signals' effects: what it reads (its writes included) and writes.
struct Access {
    std::vector<TypeKey> reads, writes;   // sorted
    std::vector<std::string> readsRes, writesRes;   // sorted
};

void insertName(std::vector<std::string>& names, const std::string& n) {
    const auto it = std::lower_bound(names.begin(), names.end(), n);
    if (it == names.end() || *it != n) names.insert(it, n);
}

template <class T>
const T* firstShared(const std::vector<T>& a, const std::vector<T>& b) {
    for (auto i = a.begin(), j = b.begin(); i != a.end() && j != b.end();) {
        if (*i < *j) ++i;
        else if (*j < *i) ++j;
        else return &*i;
    }
    return nullptr;
}

} // namespace

World::~World() = default;

void World::checkRaw() const {
    if (const SystemContext* c = t_running; c && c->m_world == this && !c->exclusive())
        throw std::logic_error("system '" + c->name() +
                               "' used the raw registry: a system that is not exclusive reaches components through its "
                               "SystemContext");
}

void World::addEffect(ComponentType trigger, const void* owner, SignalEffect effect) {
    m_effects.push_back({std::move(trigger), owner, std::move(effect)});
    m_simulateDirty = m_syncDirty = true;
}

void World::addAccess(std::string_view system, std::vector<ComponentType> reads, std::vector<ComponentType> writes,
                      std::vector<std::string> readsResources) {
    if (m_events.inStep()) throw std::logic_error("access added to system '" + std::string(system) + "' inside a step");
    m_added.push_back({std::string(system), std::move(reads), std::move(writes), std::move(readsResources)});
    m_simulateDirty = m_syncDirty = true;
}

void World::removeEffects(TypeKey trigger, const void* owner) {
    std::erase_if(m_effects, [&](const Effect& e) { return e.trigger.key == trigger && e.owner == owner; });
    m_simulateDirty = m_syncDirty = true;
}

// A phase's schedule: the order (from the declarations and, in the simulate phase, the event edges: a channel's
// writers before its typed readers and, when it has a cue, before every system that reads cues), each system's access
// with its signals' effects, the install check, and the waves.
template <class S>
void World::schedule(std::vector<S>& systems, std::vector<Wave>& waves, const char* kind, bool events) {
    const std::size_t n = systems.size();
    std::vector<std::pair<std::size_t, std::size_t>> edges;
    if (events) {
        const auto indexOf = [&](std::string_view name) {
            for (std::size_t i = 0; i < n; ++i)
                if (systems[i].name == name) return i;
            return n;
        };
        for (const auto& c : m_events.channels())
            for (const std::string& w : c->writers()) {
                const std::size_t from = indexOf(w);
                if (from == n) continue;
                for (const std::string& r : c->readers())
                    if (const std::size_t to = indexOf(r); to < n) edges.emplace_back(from, to);
                if (c->hasCue())
                    for (std::size_t to = 0; to < n; ++to)
                        if (systems[to].decl.readsCues) edges.emplace_back(from, to);
            }
    }
    const std::vector<std::size_t> order = orderSystems(systems, edges, kind);
    std::vector<std::size_t> at(n);
    for (std::size_t k = 0; k < n; ++k) at[order[k]] = k;
    permute(systems, order);
    // reach[i][j]: j runs after i by some chain of edges (positions in the order; edges only go forward).
    std::vector<std::vector<bool>> reach(n, std::vector<bool>(n, false));
    std::vector<std::vector<std::size_t>> next(n);
    for (const auto& [from, to] : edges) next[at[from]].push_back(at[to]);
    for (std::size_t i = n; i-- > 0;)
        for (const std::size_t j : next[i]) {
            reach[i][j] = true;
            for (std::size_t k = j + 1; k < n; ++k)
                if (reach[j][k]) reach[i][k] = true;
        }

    // Each system's access: its declaration, closed over the effects of the signals of what it writes.
    ankerl::unordered_dense::map<TypeKey, std::string> names;
    std::vector<Access> access(n);
    for (std::size_t i = 0; i < n; ++i) {
        const SystemDecl& d = systems[i].decl;
        if (d.reads.empty() && d.writes.empty() && d.readsResources.empty() && d.writesResources.empty() && !d.exclusive)
            throw std::logic_error(std::string(kind) + " system '" + systems[i].name +
                                   "' declares no access: SystemDecl's reads, writes, resources or exclusive");
        Access& a = access[i];
        const auto add = [&](std::vector<TypeKey>& into, const std::vector<ComponentType>& from) {
            for (const ComponentType& t : from) {
                insertSorted(into, t.key);
                names.try_emplace(t.key, t.name);
                t.assure(m_registry);
            }
        };
        add(a.writes, d.writes);
        add(a.reads, d.reads);
        for (const AddedAccess& x : m_added)
            if (x.system == systems[i].name) {
                add(a.writes, x.writes);
                add(a.reads, x.reads);
                for (const std::string& r : x.readsResources) insertName(a.readsRes, r);
            }
        for (const std::string& r : d.writesResources) insertName(a.writesRes, r);
        for (const std::string& r : d.readsResources) insertName(a.readsRes, r);
        for (bool grew = true; grew;) {
            grew = false;
            for (const Effect& e : m_effects) {
                if (!containsSorted(a.writes, e.trigger.key)) continue;
                for (const ComponentType& t : e.effect.writes)
                    if (!containsSorted(a.writes, t.key)) {
                        insertSorted(a.writes, t.key);
                        names.try_emplace(t.key, t.name);
                        t.assure(m_registry);
                        grew = true;
                    }
                for (const std::string& r : e.effect.writesResources) insertName(a.writesRes, r);
            }
        }
        for (const TypeKey k : a.writes) insertSorted(a.reads, k);
        for (const std::string& r : a.writesRes) insertName(a.readsRes, r);
    }

    // Two writers of one component or resource run in an order the declarations give, never the names'.
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j) {
            if (reach[i][j]) continue;
            std::string what;
            if (const TypeKey* k = firstShared(access[i].writes, access[j].writes)) what = names[*k];
            else if (const std::string* r = firstShared(access[i].writesRes, access[j].writesRes)) what = "the resource " + *r;
            else continue;
            throw std::logic_error(std::string("the ") + kind + " systems '" + systems[i].name + "' and '" +
                                   systems[j].name + "' both write " + what +
                                   " with no order between them: declare one after the other");
        }

    // The waves: runs of consecutive systems that conflict in nothing.
    const auto conflict = [&](std::size_t i, std::size_t j) {
        const Access &a = access[i], &b = access[j];
        return firstShared(a.writes, b.reads) || firstShared(b.writes, a.reads) || firstShared(a.writesRes, b.readsRes) ||
               firstShared(b.writesRes, a.readsRes);
    };
    waves.clear();
    std::size_t begin = 0;
    for (std::size_t k = 1; k <= n; ++k) {
        bool split = k == n || systems[k].decl.exclusive || systems[begin].decl.exclusive;
        for (std::size_t m = begin; !split && m < k; ++m) split = reach[m][k] || conflict(m, k);
        if (split) {
            waves.push_back({static_cast<uint32_t>(begin), static_cast<uint32_t>(k)});
            begin = k;
        }
    }
    if (n == 0) waves.clear();

    for (std::size_t i = 0; i < n; ++i) {
        S& s = systems[i];
        if (!s.ctx) s.ctx = std::make_unique<SystemContext>(*this, s.name, s.decl.exclusive);
        s.ctx->m_exclusive = s.decl.exclusive;
        s.ctx->m_check = m_check && !s.decl.exclusive;
        s.ctx->m_reads = std::move(access[i].reads);
        s.ctx->m_writes = std::move(access[i].writes);
    }
    std::string log;
    for (const Wave& w : waves) {
        log += log.empty() ? "[" : " [";
        for (uint32_t i = w.begin; i < w.end; ++i) log += (i == w.begin ? "" : ", ") + systems[i].name;
        log += "]";
    }
    OHAO_LOG(Debug, ecs, "{} waves: {}", kind, log);
}

void World::scheduleSimulate() {
    schedule(m_simulate, m_simulateWaves, "simulate", true);
    m_simulateTopology = m_events.topology();
    m_simulateDirty = false;
}

void World::scheduleSync() {
    schedule(m_sync, m_syncWaves, "sync", false);
    m_syncDirty = false;
}

template <class S>
std::vector<std::vector<std::string>> World::waveNames(const std::vector<S>& systems, const std::vector<Wave>& waves) {
    std::vector<std::vector<std::string>> out;
    for (const Wave& w : waves) {
        auto& names = out.emplace_back();
        for (uint32_t i = w.begin; i < w.end; ++i) names.push_back(systems[i].name);
    }
    return out;
}

std::vector<std::vector<std::string>> World::simulateWaves() {
    if (m_simulateDirty || m_events.topology() != m_simulateTopology) scheduleSimulate();
    return waveNames(m_simulate, m_simulateWaves);
}

std::vector<std::vector<std::string>> World::syncWaves() {
    if (m_syncDirty) scheduleSync();
    return waveNames(m_sync, m_syncWaves);
}

void World::addSimulate(std::string name, SimulateSystem fn, SystemDecl decl) {
    if (m_events.inStep()) throw std::logic_error("simulate system '" + name + "' added inside a step");
    if (installed(m_simulate, name)) throw std::logic_error("simulate system '" + name + "' installed twice");
    m_simulate.push_back({name, std::move(fn), std::move(decl), nullptr});
    try {
        scheduleSimulate();
    } catch (...) {
        m_simulate.erase(std::find_if(m_simulate.begin(), m_simulate.end(),
                                      [&](const auto& s) { return s.name == name; }));
        m_simulateDirty = true;   // the next step schedules the rest again
        throw;
    }
}

void World::addSync(std::string name, SyncSystem fn, SystemDecl decl) {
    if (installed(m_sync, name)) throw std::logic_error("sync system '" + name + "' installed twice");
    m_sync.push_back({name, std::move(fn), std::move(decl), nullptr});
    try {
        scheduleSync();
    } catch (...) {
        m_sync.erase(std::find_if(m_sync.begin(), m_sync.end(), [&](const auto& s) { return s.name == name; }));
        m_syncDirty = true;
        throw;
    }
}

void World::removeSimulate(std::string_view name) {
    if (m_events.inStep()) throw std::logic_error("simulate system '" + std::string(name) + "' removed inside a step");
    std::erase_if(m_simulate, [&](const auto& s) { return s.name == name; });
    m_events.forgetSystem(name);   // the order is computed again before the next step
    m_simulateDirty = true;
}

void World::removeSync(std::string_view name) {
    std::erase_if(m_sync, [&](const auto& s) { return s.name == name; });
    m_syncDirty = true;
}

SystemContext* World::systemContext(std::string_view name) noexcept {
    for (auto& s : m_simulate)
        if (s.name == name) return s.ctx.get();
    for (auto& s : m_sync)
        if (s.name == name) return s.ctx.get();
    return nullptr;
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

void World::simulate(float dt) {
    // New systems, effects or event handles since the last step: scheduled again (a conflict throws, before the step).
    if (m_simulateDirty || m_events.topology() != m_simulateTopology) scheduleSimulate();
    m_events.beginStep(m_events.step() + 1);
    if (dt > 0.0f) m_stepDt = dt;
    try {
        runWaves(m_simulate, m_simulateWaves, dt);
    } catch (...) {
        m_events.endStep();
        m_deferred.clear();
        throw;
    }
    m_events.endStep();
    m_pendingLook = glm::vec2(0.0f);   // the step took it
    for (std::size_t i = 0; i < m_deferred.size(); ++i) {   // between steps now: an op may defer another (at once)
        auto op = std::move(m_deferred[i]);
        op(*this);
    }
    m_deferred.clear();
}

void World::defer(std::function<void(World&)> op) {
    if (!m_events.inStep()) {
        op(*this);
        return;
    }
    if (SystemContext* c = t_running; c && c->m_world == this && c->m_staged) c->m_deferred.push_back(std::move(op));
    else m_deferred.push_back(std::move(op));
}

// Each wave in order. A wave of one runs here, unstaged. A wave of several: the longest runs here, the others at or
// over the inline threshold go to the pool and the rest run here (in order, or reversed), each with its stage; after the join the stages are flushed in
// the systems' order, and the first error in that order is rethrown (the later systems' stages dropped).
template <class S, class... A>
void World::runWaves(std::vector<S>& systems, const std::vector<Wave>& waves, A... args) {
    const auto run = [&](S& s) {
        SystemContext& c = *s.ctx;
        const Running running(&c);
        EventStage* const prev = currentStage();
        setCurrentStage(c.m_staged ? &c.m_stage : nullptr);
        const auto t0 = std::chrono::steady_clock::now();
        try {
            s.fn(c, args...);
        } catch (...) {
            c.m_error = std::current_exception();
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        c.m_meanUs += (us - c.m_meanUs) * 0.125;
        setCurrentStage(prev);
    };
    for (const Wave& w : waves) {
        if (w.end - w.begin == 1) {   // alone: unstaged and untimed (its time decides nothing)
            S& s = systems[w.begin];
            s.ctx->m_staged = false;
            const Running running(s.ctx.get());
            s.fn(*s.ctx, args...);
            continue;
        }
        const uint32_t n = w.end - w.begin;
        const auto at = [&](uint32_t k) -> S& { return systems[m_reverse ? w.end - 1 - k : w.begin + k]; };
        // The longest stays here: the others' handoffs then overlap it.
        uint32_t longest = 0;
        for (uint32_t k = 1; k < n; ++k)
            if (at(k).ctx->m_meanUs > at(longest).ctx->m_meanUs) longest = k;
        bool dispatched = false;
        for (uint32_t k = 0; k < n; ++k) {
            S& s = at(k);
            s.ctx->m_staged = true;
            s.ctx->m_dispatch = k != longest && s.ctx->m_meanUs >= m_inlineUs;
            if (s.ctx->m_dispatch) {
                S* sp = &s;
                m_graph->add(s.name.c_str(), [&run, sp] { run(*sp); });
                dispatched = true;
            }
        }
        m_events.setStaging(true);   // before the tasks are submitted: the submit publishes it to the workers
        jobs::Counter* done = dispatched ? &m_graph->run() : nullptr;
        for (uint32_t k = 0; k < n; ++k)
            if (S& s = at(k); !s.ctx->m_dispatch) run(s);
        if (done) jobs::wait(*done);
        m_events.setStaging(false);
        std::exception_ptr error;
        for (uint32_t i = w.begin; i < w.end; ++i) {
            SystemContext& c = *systems[i].ctx;
            c.m_staged = false;
            if (!error && c.m_error) error = c.m_error;
            c.m_error = nullptr;
            if (error) {
                c.m_stage.clear();
                c.m_deferred.clear();
                continue;
            }
            c.m_stage.flush();
            for (auto& op : c.m_deferred) m_deferred.push_back(std::move(op));
            c.m_deferred.clear();
        }
        if (error) std::rethrow_exception(error);
    }
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
    if (m_syncDirty) scheduleSync();
    runWaves(m_sync, m_syncWaves);
}

bool World::runSync(std::string_view name) {
    if (m_syncDirty) scheduleSync();
    for (auto& s : m_sync)
        if (s.name == name) {
            const Running running(s.ctx.get());
            s.fn(*s.ctx);
            return true;
        }
    return false;
}

SystemContext::SystemContext(World& world, std::string name, bool exclusive)
    : m_world(&world), m_reg(&world.m_registry), m_name(std::move(name)), m_exclusive(exclusive) {}

uint64_t SystemContext::idOf(entt::entity e) const { return m_world->idOf(e); }

entt::registry& SystemContext::registry() const {
    if (m_world->accessCheck() && !m_exclusive)
        throw std::logic_error("system '" + m_name +
                               "' used the raw registry: a system that is not exclusive reaches components through its "
                               "SystemContext");
    return *m_reg;
}

void SystemContext::checkRead(TypeKey key, std::string_view type) const {
    if (!containsSorted(m_reads, key))
        throw std::logic_error("system '" + m_name + "' reads " + std::string(type) + ", which it does not declare");
}

void SystemContext::checkWrite(TypeKey key, std::string_view type) const {
    if (!containsSorted(m_writes, key))
        throw std::logic_error("system '" + m_name + "' writes " + std::string(type) + ", which it declares " +
                               (containsSorted(m_reads, key) ? "only as a read" : "nowhere"));
}

} // namespace ohao::ecs
