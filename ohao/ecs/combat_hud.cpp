#include "ecs/combat_hud.hpp"

#include "ecs/components.hpp"
#include "ecs/scene_view.hpp"
#include "ecs/world.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace ohao::ecs {

namespace {

struct CombatHudHolder {
    std::shared_ptr<CombatHud> hud;
};

// The input-driven character with the lowest id (the round's player).
entt::entity playerOf(const World& world) {
    entt::entity best = entt::null;
    uint64_t bestId = UINT64_MAX;
    for (auto [e, pc] : world.registry().view<const PlayerController>().each()) {
        if (!pc.input) continue;
        const uint64_t id = world.idOf(e);
        if (id < bestId) {
            bestId = id;
            best = e;
        }
    }
    return best;
}

constexpr float kTimeEps = 1e-5f;

} // namespace

void CombatHud::observe(const World& world) {
    const CombatSystem* combat = combatSystem(world);
    if (!combat) return;
    const entt::entity player = playerOf(world);
    const uint64_t playerId = player != entt::null ? world.idOf(player) : 0;
    for (;;) {
        uint64_t dropped = 0;
        const auto events = combat->eventsSince(m_cursor, 256, dropped);
        if (events.empty()) break;
        for (const CombatEvent& e : events) {
            m_cursor = e.seq + 1;
            if (e.kind == CombatEvent::Kind::Restart) {
                m_hitStep = m_hurtStep = 0;
                m_hurtAmount = 0.0f;
            } else if (e.kind == CombatEvent::Kind::Damage && playerId != 0 && e.amount > 0.0f) {
                if (e.targetId == playerId) {
                    m_hurtStep = e.step;
                    m_hurtAmount = e.amount;
                } else if (e.sourceId == playerId) {
                    m_hitStep = e.step;
                }
            }
        }
    }
}

HudState CombatHud::state(const World& world, uint32_t width, uint32_t height) const {
    HudState s;
    const entt::registry& reg = world.registry();
    const entt::entity p = playerOf(world);
    const auto* health = p != entt::null ? reg.try_get<Health>(p) : nullptr;
    if (!health) return s;
    s.player = true;
    s.health = health->current;
    s.maxHealth = health->max;
    if (const auto* w = reg.try_get<Weapon>(p)) {
        s.armed = true;
        s.magazine = w->magazine;
        const auto* ws = reg.try_get<WeaponState>(p);
        s.ammo = ws ? std::min(ws->ammo, w->magazine) : w->magazine;
        s.reloading = ws && ws->reloading > 0.0f;
        s.coneDeg = w->spreadDeg + (ws ? ws->bloomDeg : 0.0f);
    }
    // The cone's radius on screen at the camera's field of view, past a fixed gap.
    float fov = 75.0f;
    if (const auto view = resolveSceneView(world).camera) fov = view->fovDeg;
    const float half = 0.5f * static_cast<float>(height);
    s.crosshairGap = 6.0f * static_cast<float>(height) / 1080.0f +
                     half * std::tan(glm::radians(s.coneDeg)) / std::tan(glm::radians(std::clamp(fov, 1.0f, 179.0f) * 0.5f));
    (void)width;

    if (const CombatSystem* combat = combatSystem(world)) {
        const float dt = combat->stepSeconds();
        const uint64_t now = combat->steps();
        const auto since = [&](uint64_t step) { return static_cast<float>(now - std::min(now, step)) * dt; };
        s.hitMarker = m_hitStep != 0 && since(m_hitStep) < kHitMarkerTime - kTimeEps;
        if (m_hurtStep != 0) {
            const float t = since(m_hurtStep);
            if (t < kHurtTime - kTimeEps)
                s.hurt = kHurtMaxAlpha * std::clamp(m_hurtAmount / 25.0f, 0.4f, 1.0f) * (1.0f - t / kHurtTime);
        }
        const RoundInfo round = combat->round();
        s.round = round.started;
        s.phase = round.phase;
        s.restartIn = round.phase == RoundPhase::Dead ? static_cast<int>(std::ceil(round.restartIn - 1e-4f)) : 0;
    }
    return s;
}

render::HudList CombatHud::layout(const HudState& s, uint32_t width, uint32_t height) {
    using render::hudRect;
    using render::hudText;
    using render::hudTextWidth;
    render::HudList list;
    if (!s.player) return list;
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    const float k = h / 1080.0f;   // everything scales with the frame's height
    const float cx = std::floor(w * 0.5f), cy = std::floor(h * 0.5f);
    const glm::vec4 white(1.0f, 1.0f, 1.0f, 0.92f), shade(0.0f, 0.0f, 0.0f, 0.45f);

    // Damage taken: red at the edges.
    if (s.hurt > 0.0f) render::hudVignette(list, {0.0f, 0.0f, w, h}, {0.7f, 0.0f, 0.0f, s.hurt});

    const bool dead = s.phase == RoundPhase::Dead || !(s.health > 0.0f);
    if (dead) hudRect(list, {0.0f, 0.0f, w, h}, {0.35f, 0.0f, 0.0f, 0.45f});

    // The crosshair: four arms around the cone; the hit marker: four corners outside it.
    if (!dead && s.armed) {
        const float g = std::round(s.crosshairGap), len = std::round(10.0f * k), t = std::max(2.0f, std::round(2.0f * k));
        const float t0 = std::floor(t * 0.5f), t1 = t - t0;
        hudRect(list, {cx + g, cy - t0, cx + g + len, cy + t1}, white);
        hudRect(list, {cx - g - len, cy - t0, cx - g, cy + t1}, white);
        hudRect(list, {cx - t0, cy - g - len, cx + t1, cy - g}, white);
        hudRect(list, {cx - t0, cy + g, cx + t1, cy + g + len}, white);
        if (s.hitMarker) {
            const float d = g + len + std::round(4.0f * k), sq = std::max(3.0f, std::round(5.0f * k));
            const glm::vec4 hit(1.0f, 0.25f, 0.2f, 1.0f);
            for (const float sx : {-1.0f, 1.0f})
                for (const float sy : {-1.0f, 1.0f}) {
                    const float x = cx + sx * d * 0.7f, y = cy + sy * d * 0.7f;
                    hudRect(list, {x - sq * 0.5f, y - sq * 0.5f, x + sq * 0.5f, y + sq * 0.5f}, hit);
                }
        }
    }

    // Health: a bar bottom left with the number above it.
    const float margin = std::round(40.0f * k);
    const float barW = std::round(300.0f * k), barH = std::max(4.0f, std::round(16.0f * k));
    const float barY = h - margin - barH;
    hudRect(list, {margin, barY, margin + barW, barY + barH}, shade);
    const float frac = s.maxHealth > 0.0f ? std::clamp(s.health / s.maxHealth, 0.0f, 1.0f) : 0.0f;
    const glm::vec4 healthColor = frac > 0.3f ? glm::vec4(0.35f, 0.9f, 0.4f, 0.95f) : glm::vec4(0.95f, 0.25f, 0.2f, 0.95f);
    if (frac > 0.0f) hudRect(list, {margin, barY, margin + std::round(barW * frac), barY + barH}, healthColor);
    const float numPx = std::max(1.0f, std::round(5.0f * k));
    hudText(list, std::to_string(static_cast<int>(std::ceil(std::max(0.0f, s.health)))),
            {margin, barY - std::round(12.0f * k) - numPx * render::kHudGlyphHeight}, numPx, white);

    // The magazine: bottom right, RELOADING above it.
    if (s.armed) {
        const std::string ammo = std::to_string(s.ammo) + "/" + std::to_string(s.magazine);
        const float px = std::max(1.0f, std::round(6.0f * k));
        const float y = h - margin - px * render::kHudGlyphHeight;
        hudText(list, ammo, {w - margin - hudTextWidth(ammo, px), y}, px, s.ammo == 0 ? glm::vec4(0.95f, 0.3f, 0.2f, 0.95f) : white);
        if (s.reloading) {
            const float rpx = std::max(1.0f, std::round(4.0f * k));
            hudText(list, "RELOADING", {w - margin - hudTextWidth("RELOADING", rpx), y - std::round(14.0f * k) - rpx * render::kHudGlyphHeight},
                    rpx, glm::vec4(1.0f, 0.85f, 0.3f, 0.95f));
        }
    }

    // The round's screens.
    const auto centred = [&](std::string_view text, float y, float px, glm::vec4 color) {
        hudText(list, text, {std::round(cx - hudTextWidth(text, px) * 0.5f), y}, px, color);
    };
    if (dead) {
        const float big = std::max(2.0f, std::round(16.0f * k)), small = std::max(1.0f, std::round(6.0f * k));
        centred("DEAD", std::round(h * 0.36f), big, glm::vec4(1.0f, 0.9f, 0.9f, 1.0f));
        if (s.round)
            centred("RESTART " + std::to_string(std::max(0, s.restartIn)), std::round(h * 0.36f) + big * 9.0f, small, white);
    } else if (s.phase == RoundPhase::Won) {
        const float big = std::max(2.0f, std::round(14.0f * k)), small = std::max(1.0f, std::round(5.0f * k));
        centred("CLEARED", std::round(h * 0.36f), big, glm::vec4(0.75f, 1.0f, 0.75f, 1.0f));
        centred("ENTER TO RESTART", std::round(h * 0.36f) + big * 9.0f, small, white);
    }
    if (list.items.size() > render::kHudMaxItems) list.items.resize(render::kHudMaxItems);
    return list;
}

CombatHud& combatHudOf(World& world) {
    auto& ctx = world.registry().ctx();
    if (auto* h = ctx.find<CombatHudHolder>()) return *h->hud;
    return *ctx.emplace<CombatHudHolder>(CombatHudHolder{std::make_shared<CombatHud>()}).hud;
}

render::HudList combatHud(World& world, uint32_t width, uint32_t height) {
    CombatHud& hud = combatHudOf(world);
    hud.observe(world);
    return CombatHud::layout(hud.state(world, width, height), width, height);
}

} // namespace ohao::ecs
