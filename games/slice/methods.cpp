// combat.info and combat.damage, the slice's methods (sub-project 7, step 5; the slice's events are events.read's
// slice.*, step 3c; docs/specs/2026-10-27-combat-design.md, "The agent-facing surface"). Rows of the game API
// (game::Method), with their params in the slice's own registry.
//
// Firing is input.inject (fire down/up), the path the window takes, so there is no weapon.fire. combat.info first runs
// the sync systems a step starts from (levels expanded, players spawned, bodies built), so it answers right after
// scene.load.
#include "slice/combat.hpp"
#include "slice/components.hpp"
#include "slice/module.hpp"

#include "ecs/world.hpp"
#include "reflect/decode.hpp"
#include "reflect/json_value.hpp"
#include "reflect/json_writer.hpp"
#include "reflect/registry.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace ohao::slice {

namespace {

using reflect::DecodeContext;
using reflect::hex16;
using reflect::JsonNode;
using reflect::JsonWriter;

constexpr float kMaxDamage = 10'000.0f;   // combat.damage's amount: [0, 1e4] hp
struct CombatInfoParams {};
struct CombatDamageParams {
    std::optional<uint64_t> entity;   // required: the entity hit (its Health, or its nearest ancestor's)
    float amount{0.0f};               // required
    std::optional<uint64_t> source;   // who dealt it (the events' source)
};

const reflect::TypeRegistry& paramsTypes() {
    static reflect::TypeRegistry r;
    static const bool once = [] {
        r.type<CombatInfoParams>("CombatInfoParams").doc("combat.info params");
        r.type<CombatDamageParams>("CombatDamageParams").doc("combat.damage params")
            .field("entity", &CombatDamageParams::entity).doc("Who takes it, 16 hex digits (its Health or an ancestor's)")
                .required()
            .field("amount", &CombatDamageParams::amount).doc("Hit points to take").unitless().min(0.0).max(kMaxDamage)
                .required()
            .field("source", &CombatDamageParams::source).doc("Who dealt it, 16 hex digits");
        return true;
    }();
    (void)once;
    return r;
}

template <class P>
bool decode(game::Call& c, const JsonNode& params, DecodeContext& ctx, P& out) {
    return game::decodeParams(c, *paramsTypes().find<P>(), params, ctx, &out);
}

constexpr std::size_t kMaxCombatants = 16;   // combat.info lists the first, then "more"
constexpr std::size_t kMaxName = 24;         // names in replies, in bytes (a whole UTF-8 sequence)

// To 1/100, without -0 (tested by magnitude: under /fp:fast `r == 0 ? 0 : r` keeps the sign).
float r2(float v) { return std::fabs(v) < 0.005f ? 0.0f : std::round(v * 100.0f) / 100.0f; }

// Rounded to 1/scale (positions to 1 mm, directions to 1e-3).
void writeVec3(JsonWriter& out, glm::vec3 v, float scale) {
    out.beginArray();
    for (int i = 0; i < 3; ++i) out.float32(std::fabs(v[i]) < 0.5f / scale ? 0.0f : std::round(v[i] * scale) / scale);
    out.endArray();
}

std::string utf8Prefix(const std::string& s, std::size_t bytes) {
    if (s.size() <= bytes) return s;
    std::size_t n = bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;   // not inside a sequence
    return s.substr(0, n);
}

std::string nameOf(const World& world, entt::entity e) {
    const auto& reg = world.registry();
    const auto* n = e != entt::null && reg.valid(e) ? reg.try_get<Name>(e) : nullptr;
    return n ? utf8Prefix(n->value, kMaxName) : std::string();
}

CombatSystem* needCombat(game::Call& c, const JsonNode& params, DecodeContext& ctx) {
    CombatSystem* combat = combatSystem(c.world);
    if (!combat) ctx.error("E_STATE", params, "this world has no combat system", "a host that installs combat (ohao serve, ohao run)");
    return combat;
}

// Where an entity stands: its world position (a character's feet).
glm::vec3 positionOf(const entt::registry& reg, entt::entity e) {
    if (const auto* wt = reg.try_get<WorldTransform>(e)) return glm::vec3(wt->world[3]);
    if (const auto* t = reg.try_get<Transform>(e)) return t->position;
    return glm::vec3(0.0f);
}

// The entity whose Health a hit on `e` takes: itself or its nearest ancestor with one.
entt::entity healthOwner(const entt::registry& reg, entt::entity e) {
    for (int depth = 0; e != entt::null && reg.valid(e) && depth < 64; ++depth) {
        if (reg.all_of<Health>(e)) return e;
        const auto* p = reg.try_get<Parent>(e);
        e = p ? p->entity : entt::entity{entt::null};
    }
    return entt::null;
}

const char* phaseName(RoundPhase p) {
    switch (p) {
        case RoundPhase::Playing: return "playing";
        case RoundPhase::Dead: return "dead";
        case RoundPhase::Won: return "won";
    }
    return "playing";
}

const char* modeName(EnemyMode m) {
    switch (m) {
        case EnemyMode::Idle: return "idle";
        case EnemyMode::Chase: return "chase";
        case EnemyMode::Attack: return "attack";
        case EnemyMode::Dead: return "dead";
    }
    return "idle";
}

void writeCombatant(const World& world, entt::entity e, JsonWriter& out) {
    const auto& reg = world.registry();
    out.beginObject();
    out.key("entity"); out.string(hex16(world.idOf(e)));
    out.key("name"); out.string(nameOf(world, e));
    if (const auto* pc = reg.try_get<PlayerController>(e); pc && pc->input) { out.key("player"); out.boolean(true); }
    if (const auto* h = reg.try_get<Health>(e)) {
        out.key("health"); out.float32(r2(h->current));
        out.key("max"); out.float32(r2(h->max));
        out.key("dead"); out.boolean(!(h->current > 0.0f));
    }
    out.key("pos"); writeVec3(out, positionOf(reg, e), 1e3f);
    if (const auto* w = reg.try_get<Weapon>(e)) {
        // Before its first step a weapon has no WeaponState: the step gives it a full magazine.
        const auto* ws = reg.try_get<WeaponState>(e);
        const WeaponState st = ws ? *ws : WeaponState{w->magazine};
        out.key("weapon");
        out.beginObject();
        out.key("ammo"); out.uint64(std::min(st.ammo, w->magazine));
        out.key("magazine"); out.uint64(w->magazine);
        out.key("reloading"); out.float32(r2(st.reloading));
        out.key("bloomDeg"); out.float32(r2(st.bloomDeg));
        out.key("kickDeg"); out.float32(r2(st.kickDeg));
        out.key("shots"); out.uint64(st.shots);
        out.endObject();
    }
    if (reg.all_of<Enemy>(e)) {
        const auto* es = reg.try_get<EnemyState>(e);
        const EnemyMode mode = es ? es->mode : EnemyMode::Idle;
        out.key("enemy");
        out.beginObject();
        out.key("state"); out.string(modeName(mode));
        if (es && (mode == EnemyMode::Chase || mode == EnemyMode::Attack)) {
            out.key("lastSeen"); writeVec3(out, es->lastSeen, 1e3f);
        }
        out.endObject();
    }
    out.endObject();
}

bool combatInfo(game::Call& c, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    CombatInfoParams p;
    if (!decode(c, params, ctx, p)) return false;
    CombatSystem* combat = needCombat(c, params, ctx);
    if (!combat) return false;
    // The state a step starts from: levels expanded, players spawned, transforms and bodies current.
    c.world.runSync("level");
    c.world.runSync("player_spawn");
    c.world.runSync("transform");
    c.world.runSync("physics");
    const auto& reg = c.world.registry();
    std::vector<std::pair<uint64_t, entt::entity>> combatants;
    for (const entt::entity e : reg.view<Health>()) combatants.emplace_back(c.world.idOf(e), e);
    for (const entt::entity e : reg.view<Weapon>())
        if (!reg.all_of<Health>(e)) combatants.emplace_back(c.world.idOf(e), e);
    std::sort(combatants.begin(), combatants.end());

    out.beginObject();
    out.key("steps"); out.uint64(combat->steps());
    if (!reg.view<const Round>().empty()) {
        const RoundInfo round = combat->round();
        out.key("round");
        out.beginObject();
        out.key("started"); out.boolean(round.started);
        out.key("phase"); out.string(phaseName(round.phase));
        if (round.phase == RoundPhase::Dead) { out.key("restartIn"); out.float32(r2(round.restartIn)); }
        out.key("restarts"); out.uint64(round.restarts);
        out.endObject();
    }
    out.key("combatants");
    out.beginArray();
    for (std::size_t i = 0; i < combatants.size() && i < kMaxCombatants; ++i) writeCombatant(c.world, combatants[i].second, out);
    out.endArray();
    if (combatants.size() > kMaxCombatants) { out.key("more"); out.uint64(combatants.size() - kMaxCombatants); }
    out.endObject();
    return true;
}

bool combatDamage(game::Call& c, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    CombatDamageParams p;
    if (!decode(c, params, ctx, p)) return false;
    CombatSystem* combat = needCombat(c, params, ctx);
    if (!combat) return false;
    const auto& reg = c.world.registry();
    const entt::entity target = c.world.findById(*p.entity);
    const entt::entity owner = target == entt::null ? entt::entity{entt::null} : healthOwner(reg, target);
    if (owner == entt::null) {
        ctx.push("entity");
        ctx.error("E_NOT_FOUND", *ctx.doc.member(params, "entity"),
                  "entity " + hex16(*p.entity) +
                      (target == entt::null ? " does not exist" : " has no Health, nor has any ancestor"),
                  "the id of an entity with Health (or below one)", "\"" + hex16(*p.entity) + "\"");
        ctx.pop();
        return false;
    }
    entt::entity source = entt::null;
    if (p.source) {
        source = c.world.findById(*p.source);
        if (source == entt::null) {
            ctx.push("source");
            ctx.error("E_NOT_FOUND", *ctx.doc.member(params, "source"), "entity " + hex16(*p.source) + " does not exist",
                      "the id of an entity", "\"" + hex16(*p.source) + "\"");
            ctx.pop();
            return false;
        }
    }
    const float before = reg.get<Health>(owner).current;
    const float after = combat->damage(target, p.amount, source, positionOf(reg, owner));
    const auto& h = reg.get<Health>(owner);
    out.beginObject();
    out.key("entity"); out.string(hex16(c.world.idOf(owner)));
    out.key("name"); out.string(nameOf(c.world, owner));
    out.key("health"); out.float32(r2(after));
    out.key("max"); out.float32(r2(h.max));
    out.key("dead"); out.boolean(!(after > 0.0f));
    out.key("taken"); out.float32(r2(before - after));
    out.endObject();
    return true;
}

} // namespace

std::span<const game::Method> sliceMethods() {
    static const std::vector<game::Method> methods = {
        {{"combat.info", paramsTypes().find<CombatInfoParams>(),
          "{steps, round?: {started, phase, restartIn?, restarts}, combatants: [{entity, name, player?, health?, max?, "
          "dead?, pos, weapon?: {ammo, magazine, reloading, bloomDeg, kickDeg, shots}, enemy?: {state, lastSeen?}}], more?}",
          "The fight: the round (playing, dead, won) and everything with Health or a Weapon, in id order. Fire with "
          "input.inject (fire, reload) and world.step."},
         combatInfo},
        {{"combat.damage", paramsTypes().find<CombatDamageParams>(), "{entity, name, health, max, dead, taken}",
          "Takes amount from the entity's Health (or its nearest ancestor's), as a hit would: damage and death "
          "events; a dead enemy lies down at the next step."},
         combatDamage, true},
    };
    return methods;
}

} // namespace ohao::slice
