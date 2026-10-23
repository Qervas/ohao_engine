#include "ecs/world.hpp"
#include "ecs/spin_system.hpp"
#include "ecs/transform_system.hpp"
#include "core/log.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
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
    // Engine-provided behaviour first: these move entities, so they run before the
    // transform system and their changes reach the GPU the same update.
    addSystem("spin", [](World& w, float dt) { updateSpin(w.registry(), dt); });
    // Structural: transforms are propagated before every host system (instantiate is
    // inserted right before it, the render extract right after).
    addSystem("transform", [](World& w, float) { updateTransforms(w.registry()); });
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

entt::entity World::createWithId(uint64_t id, std::string_view name, const Transform& local) {
    if (id == 0 || !claim(id)) return entt::null;
    return createImpl(id, name, local);
}

entt::entity World::createImpl(uint64_t id, std::string_view name, const Transform& local) {
    const entt::entity e = m_registry.create();
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

void World::addSystem(std::string name, System fn) {
    m_systems.emplace_back(std::move(name), std::move(fn));
}

void World::update(float dt) {
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
    for (auto& [name, fn] : m_systems) fn(*this, dt);
}

void World::addSystemAfter(std::string_view after, std::string name, System fn) {
    auto it = std::find_if(m_systems.begin(), m_systems.end(),
                           [&](const auto& s) { return s.first == after; });
    if (it != m_systems.end()) ++it;
    m_systems.emplace(it, std::move(name), std::move(fn));
}

void World::addSystemBefore(std::string_view before, std::string name, System fn) {
    const auto it = std::find_if(m_systems.begin(), m_systems.end(),
                                 [&](const auto& s) { return s.first == before; });
    m_systems.emplace(it, std::move(name), std::move(fn));
}

std::vector<std::string> World::systemNames() const {
    std::vector<std::string> out;
    for (const auto& [name, fn] : m_systems) out.push_back(name);
    return out;
}

} // namespace ohao::ecs
