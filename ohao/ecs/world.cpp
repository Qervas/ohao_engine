#include "ecs/world.hpp"
#include "ecs/transform_system.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <iostream>

namespace ohao::ecs {

glm::mat4 Transform::matrix() const {
    return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation) *
           glm::scale(glm::mat4(1.0f), scale);
}

World::World() {
    m_registry.on_update<Transform>().connect<&World::onTransformChanged>(this);
    // Hierarchy hooks keep Parent/Children consistent on every destroy path,
    // registry().destroy() and registry().clear() included. Pool creation order is
    // deliberate: registry().clear() empties pools newest first, so Children is
    // cleared while Parent and TransformDirty are still intact (the Children hook
    // writes to both) and they are emptied after it.
    m_registry.storage<TransformDirty>();
    m_registry.on_destroy<Parent>().connect<&World::onParentDestroyed>(this);
    m_registry.on_destroy<Children>().connect<&World::onChildrenDestroyed>(this);
    // Structural: transforms are always propagated before any other system runs.
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

entt::entity World::create(std::string_view name, const Transform& local) {
    const entt::entity e = m_registry.create();
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
            std::cerr << "[ecs] " << m_transformMismatches
                      << " entities have a WorldTransform that disagrees with their Transform: a "
                         "Transform was written without registry.patch<Transform>() or "
                         "World::setTransform() (reported once per World)\n";
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

std::vector<std::string> World::systemNames() const {
    std::vector<std::string> out;
    for (const auto& [name, fn] : m_systems) out.push_back(name);
    return out;
}

} // namespace ohao::ecs
