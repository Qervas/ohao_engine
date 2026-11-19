#include "tidewall/module.hpp"

#include <array>
#include <string_view>

namespace ohao::tidewall {

namespace {

class TidewallModule final : public game::Module {
public:
    [[nodiscard]] std::string_view name() const override { return "tidewall"; }
    // Its player's body and the level's colliders; the characters' drivers, the mix, the effects' cues and what it
    // presents come with the parts that add them (docs/plans/2026-11-18-tidewall.md).
    [[nodiscard]] std::span<const std::string_view> dependencies() const override {
        static constexpr std::array<std::string_view, 6> kDeps{"physics", "animation", "audio", "vfx", "world_render", "visual"};
        return kDeps;
    }
    // No systems yet: the yard is walked by the engine's player controller alone.
    void install(ecs::World&, const ecs::Services&) const override {}
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
