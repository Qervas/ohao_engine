#include "ecs/player.hpp"

#include "ecs/components.hpp"
#include "ecs/world.hpp"

#include <cstdint>

namespace ohao::ecs {

entt::entity playerOf(const World& world, std::size_t stream) {
    entt::entity best = entt::null;
    uint64_t bestId = UINT64_MAX;
    for (auto [e, pc] : world.registry().view<const PlayerController>().each()) {
        if (!pc.input || pc.stream != stream) continue;
        if (const uint64_t id = world.idOf(e); id < bestId) bestId = id, best = e;
    }
    return best;
}

} // namespace ohao::ecs
