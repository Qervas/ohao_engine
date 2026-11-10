#include "slice/module.hpp"

#include "slice/arms_anim.hpp"
#include "slice/combat.hpp"
#include "slice/enemy_anim.hpp"

#include "game/api.hpp"

namespace ohao::slice {

namespace {

struct SliceInstalled {};

class SliceModule final : public game::Module {
public:
    [[nodiscard]] std::string_view name() const override { return "slice"; }
    void registerTypes(reflect::TypeRegistry& types) const override { registerSliceTypes(types); }
    void install(World& world) const override {
        auto& ctx = world.registry().ctx();
        if (ctx.contains<SliceInstalled>()) return;
        ctx.emplace<SliceInstalled>();
        installCombat(world);   // its channels, the round, the enemies, the view model, the dead player's rule
        if (AnimationSystem* animation = animationSystem(world)) {
            registerEnemyDriver(*animation, world.events());
            registerArmsDriver(*animation, world.events());
        }
        installSlicePresentation(world);
    }
    void uninstall(World& world) const override {
        forgetHud(world);
        uninstallCombat(world);
        world.registry().ctx().erase<SliceInstalled>();
    }
    [[nodiscard]] std::span<const game::Method> methods() const override { return sliceMethods(); }
};

} // namespace

const game::Module& module() {
    static const SliceModule m;
    return m;
}

} // namespace ohao::slice

// The module's entry (game/loader.hpp): the one symbol a host looks up.
extern "C" __attribute__((visibility("default"))) const ohao::game::Module* ohao_game_module() {
    return &ohao::slice::module();
}
