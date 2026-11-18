#include "physics/player.hpp"

#include "physics/components.hpp"
#include "ecs/world.hpp"

#include <cstdint>

namespace ohao::ecs {

namespace {

template <class Reg, class IdOf>
entt::entity lowestPlayer(const Reg& reg, IdOf idOf, std::size_t stream) {
    entt::entity best = entt::null;
    uint64_t bestId = UINT64_MAX;
    for (auto [e, pc] : reg.template view<const PlayerController>().each()) {
        if (!pc.input || pc.stream != stream) continue;
        if (const uint64_t id = idOf(e); id < bestId) bestId = id, best = e;
    }
    return best;
}

} // namespace

entt::entity playerOf(const World& world, std::size_t stream) {
    return lowestPlayer(world.registry(), [&](entt::entity e) { return world.idOf(e); }, stream);
}

entt::entity playerOf(const SystemContext& ctx, std::size_t stream) {
    return lowestPlayer(ctx, [&](entt::entity e) { return ctx.idOf(e); }, stream);
}

} // namespace ohao::ecs
