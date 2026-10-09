#include "ecs/transform_system.hpp"
#include "ecs/components.hpp"

#include <cmath>
#include <vector>

namespace ohao::ecs {

namespace {

glm::mat4 parentWorld(const entt::registry& reg, entt::entity e) {
    if (const auto* p = reg.try_get<Parent>(e))
        if (reg.valid(p->entity))
            if (const auto* wt = reg.try_get<WorldTransform>(p->entity)) return wt->world;
    return glm::mat4(1.0f);
}

void recompute(entt::registry& reg, entt::entity root) {
    std::vector<entt::entity> stack{root};
    while (!stack.empty()) {
        const entt::entity e = stack.back();
        stack.pop_back();
        const glm::mat4 world = parentWorld(reg, e) * reg.get<Transform>(e).matrix();
        if (auto* wt = reg.try_get<WorldTransform>(e)) {
            wt->prevWorld = wt->world;
            wt->world = world;
            reg.emplace_or_replace<MovedLastFrame>(e);
        } else {
            reg.emplace<WorldTransform>(e, WorldTransform{world, world});
        }
        reg.emplace_or_replace<RenderDirty>(e);
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

} // namespace

void updateTransforms(entt::registry& reg) {
    // 1. Last frame's movers that did not move again: prevWorld catches up.
    std::vector<entt::entity> settled;
    for (auto e : reg.view<MovedLastFrame>())
        if (!reg.all_of<TransformDirty>(e)) settled.push_back(e);
    for (auto e : settled) {
        auto& wt = reg.get<WorldTransform>(e);
        wt.prevWorld = wt.world;
        reg.remove<MovedLastFrame>(e);
        reg.emplace_or_replace<RenderDirty>(e);
    }
    // 2. Dirty subtrees, each from its topmost dirty entity.
    std::vector<entt::entity> roots;
    for (auto e : reg.view<TransformDirty>())
        if (!hasDirtyAncestor(reg, e)) roots.push_back(e);
    for (auto e : roots)
        if (reg.valid(e)) recompute(reg, e);
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
