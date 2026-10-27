// combat.info, combat.events and combat.damage (sub-project 7, step 5;
// docs/specs/2026-10-27-combat-design.md, "The agent-facing surface").
//
// The session reaches the fight through what installCombat left on the World (ecs::combatSystem); a
// World without it answers E_STATE. Firing is input.inject (fire down/up), the path the window takes,
// so there is no weapon.fire. combat.info first runs the sync systems a step starts from (levels
// expanded, players spawned, bodies built), so it answers right after scene.load.
#include "api/internal.hpp"

#include "ecs/combat.hpp"
#include "ecs/components.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace ohao::api {

namespace {

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

std::string nameOf(const Session::Impl& s, entt::entity e) {
    const auto& reg = s.world.registry();
    const auto* n = e != entt::null && reg.valid(e) ? reg.try_get<ecs::Name>(e) : nullptr;
    return n ? utf8Prefix(n->value, kMaxName) : std::string();
}

ecs::CombatSystem* needCombat(Session::Impl& s, const JsonNode& params, DecodeContext& ctx) {
    ecs::CombatSystem* combat = ecs::combatSystem(s.world);
    if (!combat) ctx.error("E_STATE", params, "this world has no combat system", "a host that installs combat (ohao serve, ohao run)");
    return combat;
}

// Where an entity stands: its world position (a character's feet).
glm::vec3 positionOf(const entt::registry& reg, entt::entity e) {
    if (const auto* wt = reg.try_get<ecs::WorldTransform>(e)) return glm::vec3(wt->world[3]);
    if (const auto* t = reg.try_get<ecs::Transform>(e)) return t->position;
    return glm::vec3(0.0f);
}

// The entity whose Health a hit on `e` takes: itself or its nearest ancestor with one.
entt::entity healthOwner(const entt::registry& reg, entt::entity e) {
    for (int depth = 0; e != entt::null && reg.valid(e) && depth < 64; ++depth) {
        if (reg.all_of<ecs::Health>(e)) return e;
        const auto* p = reg.try_get<ecs::Parent>(e);
        e = p ? p->entity : entt::entity{entt::null};
    }
    return entt::null;
}

const char* phaseName(ecs::RoundPhase p) {
    switch (p) {
        case ecs::RoundPhase::Playing: return "playing";
        case ecs::RoundPhase::Dead: return "dead";
        case ecs::RoundPhase::Won: return "won";
    }
    return "playing";
}

const char* modeName(ecs::EnemyMode m) {
    switch (m) {
        case ecs::EnemyMode::Idle: return "idle";
        case ecs::EnemyMode::Chase: return "chase";
        case ecs::EnemyMode::Attack: return "attack";
        case ecs::EnemyMode::Dead: return "dead";
    }
    return "idle";
}

void writeCombatant(Session::Impl& s, entt::entity e, JsonWriter& out) {
    const auto& reg = s.world.registry();
    out.beginObject();
    out.key("entity"); out.string(hex16(s.world.idOf(e)));
    out.key("name"); out.string(nameOf(s, e));
    if (const auto* pc = reg.try_get<ecs::PlayerController>(e); pc && pc->input) { out.key("player"); out.boolean(true); }
    if (const auto* h = reg.try_get<ecs::Health>(e)) {
        out.key("health"); out.float32(r2(h->current));
        out.key("max"); out.float32(r2(h->max));
        out.key("dead"); out.boolean(!(h->current > 0.0f));
    }
    out.key("pos"); writeVec3(out, positionOf(reg, e), 1e3f);
    if (const auto* w = reg.try_get<ecs::Weapon>(e)) {
        // Before its first step a weapon has no WeaponState: the step gives it a full magazine.
        const auto* ws = reg.try_get<ecs::WeaponState>(e);
        const ecs::WeaponState st = ws ? *ws : ecs::WeaponState{w->magazine};
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
    if (reg.all_of<ecs::Enemy>(e)) {
        const auto* es = reg.try_get<ecs::EnemyState>(e);
        const ecs::EnemyMode mode = es ? es->mode : ecs::EnemyMode::Idle;
        out.key("enemy");
        out.beginObject();
        out.key("state"); out.string(modeName(mode));
        if (es && (mode == ecs::EnemyMode::Chase || mode == ecs::EnemyMode::Attack)) {
            out.key("lastSeen"); writeVec3(out, es->lastSeen, 1e3f);
        }
        out.endObject();
    }
    out.endObject();
}

void writeEvent(Session::Impl& s, const ecs::CombatEvent& e, JsonWriter& out) {
    using Kind = ecs::CombatEvent::Kind;
    out.beginObject();
    out.key("seq"); out.uint64(e.seq + 1);   // from 1, as physics.events
    out.key("step"); out.uint64(e.step);
    out.key("kind"); out.string(ecs::combatEventKindName(e.kind));
    const bool round = e.kind == Kind::RoundStart || e.kind == Kind::Restart || e.kind == Kind::Win;
    if (!round && e.sourceId) {
        out.key("source"); out.string(hex16(e.sourceId));
        out.key("sn"); out.string(nameOf(s, e.source));
    }
    if ((e.kind == Kind::Hit || e.kind == Kind::Damage || e.kind == Kind::Death) && e.targetId) {
        out.key("target"); out.string(hex16(e.targetId));
        out.key("tn"); out.string(nameOf(s, e.target));
    }
    switch (e.kind) {
        case Kind::Shot:
            out.key("point"); writeVec3(out, e.point, 1e3f);
            out.key("dir"); writeVec3(out, e.normal, 1e3f);
            out.key("coneDeg"); out.float32(r2(e.amount));
            break;
        case Kind::Hit:
            out.key("point"); writeVec3(out, e.point, 1e3f);
            out.key("normal"); writeVec3(out, e.normal, 1e3f);
            out.key("dist"); out.float32(r2(e.amount));
            out.key("surface"); out.string(utf8Prefix(e.surface, 32));
            break;
        case Kind::Damage:
            out.key("amount"); out.float32(r2(e.amount));
            out.key("health"); out.float32(r2(e.health));
            break;
        case Kind::Reload:
        case Kind::Reloaded:
            out.key("ammo"); out.uint64(static_cast<uint64_t>(std::max(0.0f, e.amount)));
            break;
        default:
            break;
    }
    out.endObject();
}

} // namespace

bool methodCombatInfo(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    CombatInfoParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    ecs::CombatSystem* combat = needCombat(s, params, ctx);
    if (!combat) return false;
    // The state a step starts from: levels expanded, players spawned, transforms and bodies current.
    s.world.runSync("level");
    s.world.runSync("player_spawn");
    s.world.runSync("transform");
    s.world.runSync("physics");
    const auto& reg = s.world.registry();
    std::vector<std::pair<uint64_t, entt::entity>> combatants;
    for (const entt::entity e : reg.view<ecs::Health>()) combatants.emplace_back(s.world.idOf(e), e);
    for (const entt::entity e : reg.view<ecs::Weapon>())
        if (!reg.all_of<ecs::Health>(e)) combatants.emplace_back(s.world.idOf(e), e);
    std::sort(combatants.begin(), combatants.end());

    out.beginObject();
    out.key("steps"); out.uint64(combat->steps());
    if (!reg.view<const ecs::Round>().empty()) {
        const ecs::RoundInfo round = combat->round();
        out.key("round");
        out.beginObject();
        out.key("started"); out.boolean(round.started);
        out.key("phase"); out.string(phaseName(round.phase));
        if (round.phase == ecs::RoundPhase::Dead) { out.key("restartIn"); out.float32(r2(round.restartIn)); }
        out.key("restarts"); out.uint64(round.restarts);
        out.endObject();
    }
    out.key("combatants");
    out.beginArray();
    for (std::size_t i = 0; i < combatants.size() && i < kMaxCombatants; ++i) writeCombatant(s, combatants[i].second, out);
    out.endArray();
    if (combatants.size() > kMaxCombatants) { out.key("more"); out.uint64(combatants.size() - kMaxCombatants); }
    out.endObject();
    return true;
}

// The log after a cursor, as physics.events. Reading changes nothing, so it does not sync.
bool methodCombatEvents(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx,
                        JsonWriter& out) {
    CombatEventsParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    ecs::CombatSystem* combat = needCombat(s, params, ctx);
    if (!combat) return false;
    // The reply numbers events from 1 (the log's seq + 1), so "after since" is the log's seq >= since.
    const auto since = static_cast<uint64_t>(p.since);
    uint64_t dropped = 0;
    const std::vector<ecs::CombatEvent> events = combat->eventsSince(since, static_cast<std::size_t>(p.max), dropped);
    out.beginObject();
    out.key("events");
    out.beginArray();
    for (const ecs::CombatEvent& e : events) writeEvent(s, e, out);
    out.endArray();
    // The cursor for the next call: the last event returned, else where the caller is (never past the log).
    out.key("next"); out.uint64(events.empty() ? std::min(since, combat->nextEventSeq()) : events.back().seq + 1);
    if (dropped) { out.key("dropped"); out.uint64(dropped); }
    out.endObject();
    return true;
}

bool methodCombatDamage(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx,
                        JsonWriter& out) {
    CombatDamageParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    ecs::CombatSystem* combat = needCombat(s, params, ctx);
    if (!combat) return false;
    const auto& reg = s.world.registry();
    const entt::entity target = s.world.findById(*p.entity);
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
        source = s.world.findById(*p.source);
        if (source == entt::null) {
            ctx.push("source");
            ctx.error("E_NOT_FOUND", *ctx.doc.member(params, "source"), "entity " + hex16(*p.source) + " does not exist",
                      "the id of an entity", "\"" + hex16(*p.source) + "\"");
            ctx.pop();
            return false;
        }
    }
    const float before = reg.get<ecs::Health>(owner).current;
    const float after = combat->damage(target, p.amount, source, positionOf(reg, owner));
    const auto& h = reg.get<ecs::Health>(owner);
    out.beginObject();
    out.key("entity"); out.string(hex16(s.world.idOf(owner)));
    out.key("name"); out.string(nameOf(s, owner));
    out.key("health"); out.float32(r2(after));
    out.key("max"); out.float32(r2(h.max));
    out.key("dead"); out.boolean(!(after > 0.0f));
    out.key("taken"); out.float32(r2(before - after));
    out.endObject();
    return true;
}

} // namespace ohao::api
