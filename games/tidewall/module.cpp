#include "tidewall/module.hpp"
#include "tidewall/walker.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace ohao::tidewall {

namespace {

class TidewallModule final : public game::Module {
public:
    [[nodiscard]] std::string_view name() const override { return "tidewall"; }
    // Its player's body, the level's colliders and the navmesh its walkers follow; the characters' drivers, the mix, the effects' cues and what it
    // presents come with the parts that add them (docs/plans/2026-11-18-tidewall.md).
    [[nodiscard]] std::span<const std::string_view> dependencies() const override {
        static constexpr std::array<std::string_view, 7> kDeps{"physics", "navigation", "animation", "audio", "vfx",
                                                              "world_render", "visual"};
        return kDeps;
    }
    void registerTypes(reflect::TypeRegistry& types) const override { registerWalkerTypes(types); }
    // The walkers (N1): the player is the engine's controller.
    void install(ecs::World& world, const ecs::Services&) const override {
        const auto installed = world.simulateNames();
        if (std::find(installed.begin(), installed.end(), "walkers") != installed.end()) return;
        installWalkers(world);
    }
};

} // namespace

const game::Module& module() {
    static const TidewallModule m;
    return m;
}

} // namespace ohao::tidewall

// The module's entry (game/loader.hpp): the one symbol a host looks up.
OHAO_GAME_ENTRY const ohao::game::Module* ohao_game_module() {
    return &ohao::tidewall::module();
}
