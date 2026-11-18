#include "slice/module.hpp"

#include "slice/arms_anim.hpp"
#include "slice/combat.hpp"
#include "slice/enemy_anim.hpp"

#include "game/api.hpp"

#include <array>
#include <string_view>

namespace ohao::slice {

namespace {

struct SliceInstalled {};

class SliceModule final : public game::Module {
public:
    [[nodiscard]] std::string_view name() const override { return "slice"; }
    // Its players and bodies, the enemies' and arms' drivers, the mix it asks for, its effects' cues, what it
    // presents, the meshes and camera it reads.
    [[nodiscard]] std::span<const std::string_view> dependencies() const override {
        static constexpr std::array<std::string_view, 6> kDeps{"physics", "animation", "audio", "vfx", "world_render", "visual"};
        return kDeps;
    }
    void registerTypes(reflect::TypeRegistry& types) const override { registerSliceTypes(types); }
    void install(World& world, const Services&) const override {
        auto& ctx = world.context();
        if (ctx.contains<SliceInstalled>()) return;
        ctx.emplace<SliceInstalled>();
        installCombat(world);   // its channels, the round, the enemies, the view model, the dead player's rule
        AnimationSystem& animation = *animationSystem(world);   // a dependency: installed before the game
        registerEnemyDriver(animation, world.events());
        registerArmsDriver(animation, world.events());
        installSlicePresentation(world);
    }
    void uninstall(World& world) const override {
        forgetHud(world);
        uninstallCombat(world);
        world.context().erase<SliceInstalled>();
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
