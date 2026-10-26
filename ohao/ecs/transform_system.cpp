#include "ecs/transform_system.hpp"
#include "ecs/components.hpp"

#include <ankerl/unordered_dense.h>

#include <cmath>
#include <vector>

namespace ohao::ecs {

namespace {

// The entities propagateTransforms recomputed since the last render: their prevWorld is the
// frame's start and stays so until updateTransforms (which clears this) has used it.
struct StepCaptured {
    ankerl::unordered_dense::set<entt::entity> entities;
};

glm::mat4 parentWorld(const entt::registry& reg, entt::entity e) {
    if (const auto* p = reg.try_get<Parent>(e))
        if (reg.valid(p->entity))
            if (const auto* wt = reg.try_get<WorldTransform>(p->entity)) return wt->world;
    return glm::mat4(1.0f);
}

// captured: null for a render's update; inside a step, the set of entities whose prevWorld
// is already the frame's start.
void recompute(entt::registry& reg, entt::entity root, StepCaptured* step, const StepCaptured* kept) {
    std::vector<entt::entity> stack{root};
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        const glm::mat4 world = parentWorld(reg, e) * reg.get<Transform>(e).matrix();
        if (auto* wt = reg.try_get<WorldTransform>(e)) {
            if (step) {
                if (step->entities.insert(e).second) wt->prevWorld = wt->world;
            } else if (!kept || !kept->entities.contains(e)) {
                wt->prevWorld = wt->world;
            }
            wt->world = world;
            reg.emplace_or_replace<MovedLastFrame>(e);
        } else {
            reg.emplace<WorldTransform>(e, WorldTransform{world, world});
        }
        reg.emplace_or_replace<RenderDirty>(e);
        if (reg.any_of<PhysicsBody, ColliderPart>(e)) reg.emplace_or_replace<PhysicsMoved>(e);   // the physics system follows
        reg.remove<TransformDirty>(e);
        if (const auto* ch = reg.try_get<Children>(e))
            stack.insert(stack.end(), ch->entities.begin(), ch->entities.end());
    }
}

bool hasDirtyAncestor(const entt::registry& reg, entt::entity e) {
    for (const auto* p = reg.try_get<Parent>(e); p && reg.valid(p->entity);
         p = reg.try_get<Parent>(p->entity))
        if (reg.all_of<TransformDirty>(p->entity)) return true;
    return false;
}

void recomputeDirty(entt::registry& reg, StepCaptured* step, const StepCaptured* kept) {
    std::vector<entt::entity> roots;
    for (auto e : reg.view<TransformDirty>())
        if (!hasDirtyAncestor(reg, e)) roots.push_back(e);
    for (auto e : roots)
        if (reg.valid(e)) recompute(reg, e, step, kept);
}

} // namespace

void updateTransforms(entt::registry& reg) {
    StepCaptured* kept = reg.ctx().find<StepCaptured>();
    if (kept && kept->entities.empty()) kept = nullptr;
    // 1. Last frame's movers that did not move again: prevWorld catches up. An entity a step
    // of this frame moved keeps the prevWorld that step captured.
    std::vector<entt::entity> settled;
    for (auto e : reg.view<MovedLastFrame>())
        if (!reg.all_of<TransformDirty>(e) && (!kept || !kept->entities.contains(e))) settled.push_back(e);
    for (auto e : settled) {
        auto& wt = reg.get<WorldTransform>(e);
        wt.prevWorld = wt.world;
        reg.remove<MovedLastFrame>(e);
        reg.emplace_or_replace<RenderDirty>(e);
    }
    // 2. Dirty subtrees, each from its topmost dirty entity.
    recomputeDirty(reg, nullptr, kept);
    if (kept) kept->entities.clear();
}

void propagateTransforms(entt::registry& reg) {
    if (reg.view<TransformDirty>().empty()) return;
    auto* captured = reg.ctx().find<StepCaptured>();
    if (!captured) captured = &reg.ctx().emplace<StepCaptured>();
    recomputeDirty(reg, captured, nullptr);
}

std::size_t verifyTransforms(const entt::registry& reg) {
    std::size_t bad = 0;
    for (auto [e, t, wt] : reg.view<const Transform, const WorldTransform>().each()) {
        if (reg.all_of<TransformDirty>(e)) continue;
        const glm::mat4 expect = parentWorld(reg, e) * t.matrix();
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (std::fabs(expect[c][r] - wt.world[c][r]) > 1e-4f) { ++bad; goto next; }
    next:;
    }
    return bad;
}

} // namespace ohao::ecs
