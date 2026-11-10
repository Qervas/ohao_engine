#pragma once
// The combat HUD (sub-project 7 spec, "Presentation"): what the `hud` pass draws for the
// input-driven player, built each frame from the world and the combat log. Health (a bar and a
// number), the magazine ("24/30", "RELOADING"), a crosshair whose arms open with the cone (spread
// plus bloom, at the camera's field of view), a hit marker for 0.15 s after the player's round
// damaged something, a red vignette for 0.3 s after the player took damage (stronger the more it
// took), and the round's screens: dead (a red wash, "DEAD", "RESTART 3") and won ("CLEARED",
// "ENTER TO RESTART"). Times are counted in combat steps, so a frame of a lockstep world and its
// headless capture show the same HUD. Never saved or hashed: the World's context holds it.
#include "ecs/combat.hpp"
#include "render/hud/hud_list.hpp"

#include <cstdint>

namespace ohao::ecs {

class World;

struct HudState {
    bool player{false};          // an input-driven character with Health: the HUD shows
    float health{0.0f}, maxHealth{0.0f};
    bool armed{false};           // it has a Weapon
    uint32_t ammo{0}, magazine{0};
    bool reloading{false};
    float coneDeg{0.0f};         // spread plus bloom now
    float crosshairGap{0.0f};    // px from the screen's centre to each arm's inner end
    bool hitMarker{false};       // the player's round damaged something within kHitMarkerTime
    float hurt{0.0f};            // the vignette's alpha, 0..kHurtMaxAlpha
    RoundPhase phase{RoundPhase::Playing};
    bool round{false};           // the world has a running round
    int restartIn{0};            // whole seconds left (rounded up) while dead
};

class CombatHud {
public:
    static constexpr float kHitMarkerTime = 0.15f;   // s
    static constexpr float kHurtTime = 0.3f;         // s
    static constexpr float kHurtMaxAlpha = 0.55f;

    // Reads the slice's events since the last call (the World's log): the player's last damaging round and the
    // last damage it took (a restore, a round's restart too, forgets both).
    void observe(const World& world);
    // The HUD's content for a width x height frame, the world as it is now.
    [[nodiscard]] HudState state(const World& world, uint32_t width, uint32_t height) const;
    // Its items: everything at fixed positions scaled with the frame's height.
    [[nodiscard]] static render::HudList layout(const HudState& s, uint32_t width, uint32_t height);

private:
    uint64_t m_cursor{0};   // the last seq read; 0: the first observe reads the history from its oldest event
    uint64_t m_hitStep{0}, m_hurtStep{0};   // World steps (1 = the first); 0: none
    float m_hurtAmount{0.0f};
};

// The world's HUD (one in its context, made on first use), observed up to now and laid out.
[[nodiscard]] render::HudList combatHud(World& world, uint32_t width, uint32_t height);
[[nodiscard]] CombatHud& combatHudOf(World& world);

// Registers the slice's presentation (world_render/present.hpp): this HUD and the grenades in flight. A second
// call does nothing. With installSlice until the slice is a module (step 3i).
void installCombatPresentation(World& world);

} // namespace ohao::ecs
