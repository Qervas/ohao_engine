#include "ecs/slice.hpp"

#include "ecs/animation.hpp"
#include "ecs/arms_anim.hpp"
#include "ecs/combat.hpp"
#include "ecs/enemy_anim.hpp"
#include "ecs/world.hpp"

namespace ohao::ecs {

namespace {
struct SliceInstalled {};
} // namespace

void installSlice(World& world) {
    auto& ctx = world.registry().ctx();
    if (ctx.contains<SliceInstalled>()) return;
    ctx.emplace<SliceInstalled>();
    installCombat(world);   // its channels, the round, the enemies and the view model
    if (AnimationSystem* animation = animationSystem(world)) {
        registerEnemyDriver(*animation, world.events());
        registerArmsDriver(*animation, world.events());
    }
}

} // namespace ohao::ecs
