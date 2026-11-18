// The physics module's API methods (physics/module.hpp): physics.raycast and physics.info (sub-project 5, step 5;
// docs/specs/2026-10-26-level-pipeline-design.md, "The agent-facing surface"), physics.contacts, player.info and
// player.teleport (sub-project 6, step 5). Rows of the session's catalogue where the module is installed, with their
// params in the module's own registry. Each first runs the sync systems a step starts from (levels expanded, players
// spawned, transforms, bodies; never instantiate or the render extract), so a query right after scene.load or
// world.apply sees the level's nodes and bodies; those systems do nothing when nothing changed.
#include "physics/module.hpp"

#include "ecs/components.hpp"
#include "ecs/world.hpp"
#include "physics/components.hpp"
#include "physics/physics_scene.hpp"
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

namespace ohao::ecs {

namespace {

using reflect::DecodeContext;
using reflect::hex16;
using reflect::JsonNode;
using reflect::JsonWriter;

constexpr float kMaxRayLength = 100'000.0f;   // physics.raycast's max: (0, 1e5] m
constexpr float kMaxTeleport = 100'000.0f;    // player.teleport's pos: each axis within ±1e5 m
constexpr std::size_t kMaxWarnings = 4;       // physics.info lists the first 8 warnings, then "moreWarnings"
constexpr std::size_t kMaxName = 40;          // names in replies, in bytes (a whole UTF-8 sequence)
constexpr std::size_t kMaxEventName = 24;     // physics.contacts' an and bn, in bytes

struct PhysicsRaycastParams {
    glm::vec3 from{0.0f};
    glm::vec3 dir{0.0f};
    float max{1000.0f};
};
struct PhysicsInfoParams {};
struct PhysicsContactsParams {
    std::optional<uint64_t> entity;   // only the pairs this entity is in
    int32_t max{32};                  // 1..64
};
struct PlayerInfoParams {
    std::optional<uint64_t> entity;   // absent: the world's only player
};
struct PlayerTeleportParams {
    std::optional<uint64_t> entity;
    glm::vec3 pos{0.0f};
    std::optional<float> yawDeg;      // absent: it keeps its facing
};

const reflect::TypeRegistry& paramsTypes() {
    static reflect::TypeRegistry r;
    static const bool once = [] {
        // max has no field limits: outside (0, 1e5] is E_PARAMS, as a zero dir is.
        r.type<PhysicsRaycastParams>("PhysicsRaycastParams").doc("physics.raycast params")
            .field("from", &PhysicsRaycastParams::from).doc("Ray origin [x, y, z]").unit("m").required()
            .field("dir", &PhysicsRaycastParams::dir).doc("Direction [x, y, z], any nonzero length").unitless().required()
            .field("max", &PhysicsRaycastParams::max).doc("Farthest hit, in (0, 100000]").unit("m");
        r.type<PhysicsInfoParams>("PhysicsInfoParams").doc("physics.info params");
        r.type<PhysicsContactsParams>("PhysicsContactsParams").doc("physics.contacts params")
            .field("entity", &PhysicsContactsParams::entity).doc("Only the pairs this entity (16 hex digits) is in")
            .field("max", &PhysicsContactsParams::max).doc("Pairs in one reply").unitless().min(1).max(64);
        r.type<PlayerInfoParams>("PlayerInfoParams").doc("player.info params")
            .field("entity", &PlayerInfoParams::entity).doc("The player's id, 16 hex digits; absent: the only player");
        r.type<PlayerTeleportParams>("PlayerTeleportParams").doc("player.teleport params")
            .field("entity", &PlayerTeleportParams::entity).doc("The player's id, 16 hex digits; absent: the only player")
            .field("pos", &PlayerTeleportParams::pos).doc("Where its feet go, world [x, y, z]").unit("m")
                .min(-kMaxTeleport).max(kMaxTeleport).required()
            .field("yawDeg", &PlayerTeleportParams::yawDeg)
                .doc("Facing about +Y from -Z, counter-clockwise seen from above; absent: as it faced").unit("deg")
                .min(-360.0).max(360.0);
        return true;
    }();
    (void)once;
    return r;
}

template <class P>
bool decode(Call& c, const JsonNode& params, DecodeContext& ctx, P& out) {
    return decodeParams(c, *paramsTypes().find<P>(), params, ctx, &out);
}

// Positions to 0.1 mm, directions to 1e-4: short numbers, and no -0 (tested by magnitude: under
// /fp:fast `r == 0 ? 0 : r` folds to r and keeps the sign).
float r4(float v) {
    if (std::fabs(v) < 0.5e-4f) return 0.0f;
    return std::round(v * 1e4f) / 1e4f;
}
float ms3(double v) { return static_cast<float>(std::round(v * 1000.0) / 1000.0); }
// Angles to 0.01 degree, without -0.
float deg2(float v) { return std::fabs(v) < 0.005f ? 0.0f : std::round(v * 100.0f) / 100.0f; }

void writeVec(JsonWriter& out, glm::vec3 v) {
    out.beginArray();
    out.float32(r4(v.x)); out.float32(r4(v.y)); out.float32(r4(v.z));
    out.endArray();
}

// Rounded to 1/scale (an event's point and normal to 1 mm and 1e-3).
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

std::string nameOrId(const World& world, entt::entity e) {
    const auto& reg = world.registry();
    if (e == entt::null || !reg.valid(e)) return std::string();
    const auto* n = reg.try_get<Name>(e);
    return n && !n->value.empty() ? utf8Prefix(n->value, kMaxEventName) : hex16(world.idOf(e));
}

// The derived state the queries read: the level's nodes, their world transforms, the bodies.
void syncPhysics(World& world) {
    world.runSync("level");
    world.runSync("transform");
    world.runSync("physics");
}

// The players' state as a step would start from it: levels expanded, players spawned, bodies built.
void syncPlayers(World& world) {
    world.runSync("level");
    world.runSync("player_spawn");
    world.runSync("transform");
    world.runSync("physics");
}

// The player the params name, or the world's only one. Reports and returns null.
entt::entity findPlayer(const World& world, const PhysicsScene& physics, const JsonNode& params,
                        const std::optional<uint64_t>& id, DecodeContext& ctx) {
    const std::vector<entt::entity> players = physics.players();
    if (id) {
        const entt::entity e = world.findById(*id);
        if (e == entt::null || std::find(players.begin(), players.end(), e) == players.end()) {
            ctx.push("entity");
            ctx.error("E_NOT_FOUND", *ctx.doc.member(params, "entity"),
                      "entity " + hex16(*id) + (e == entt::null ? " does not exist" : " is not a player (no PlayerController)"),
                      "the id of an entity with a PlayerController", "\"" + hex16(*id) + "\"");
            ctx.pop();
            return entt::null;
        }
        return e;
    }
    // Without an id: the character moved by input (an enemy, moved by its AI, is not the player).
    std::vector<entt::entity> driven;
    for (const entt::entity e : players)
        if (const auto* pc = world.registry().try_get<PlayerController>(e); pc && pc->input) driven.push_back(e);
    if (driven.empty()) {
        ctx.error("E_STATE", params, "the world has no player",
                  "a scene with a PlayerController, as assets/scenes/blockout_play.scene.json");
        return entt::null;
    }
    if (driven.size() > 1) {
        std::string ids;
        for (const entt::entity e : driven) ids += (ids.empty() ? "" : ", ") + hex16(world.idOf(e));
        ctx.error("E_COMMAND", params, std::to_string(driven.size()) + " players in the world; name one with entity",
                  "{\"entity\": one of " + ids + "}");
        return entt::null;
    }
    return driven.front();
}

void writePlayerInfo(const World& world, entt::entity e, const PlayerInfo& info, JsonWriter& out) {
    const auto* name = world.registry().try_get<Name>(e);
    out.beginObject();
    out.key("entity"); out.string(hex16(world.idOf(e)));
    out.key("name"); out.string(name ? utf8Prefix(name->value, kMaxName) : std::string());
    out.key("pos"); writeVec(out, info.pos);
    out.key("yawDeg"); out.float32(deg2(info.yawDeg));
    out.key("pitchDeg"); out.float32(deg2(info.pitchDeg));
    out.key("velocity"); writeVec3(out, info.velocity, 1e3f);
    out.key("ground"); out.boolean(info.onGround);
    if (info.ground != entt::null) { out.key("on"); out.string(nameOrId(world, info.ground)); }
    if (!info.surface.empty()) { out.key("surface"); out.string(utf8Prefix(info.surface, 32)); }
    out.key("footsteps"); out.uint64(info.footsteps);
    out.endObject();
}

bool raycast(Call& c, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    PhysicsRaycastParams p;
    if (!decode(c, params, ctx, p)) return false;
    // Rules no field limit states: E_PARAMS (the caller's mistake, as a limit would be).
    if (!(glm::dot(p.dir, p.dir) > 0.0f) || !std::isfinite(glm::dot(p.dir, p.dir))) {
        ctx.push("dir");
        ctx.error("E_PARAMS", *ctx.doc.member(params, "dir"), "the direction is zero (or not finite)",
                  "a nonzero [x, y, z]");
        ctx.pop();
        c.paramsFailed = true;
        return false;
    }
    if (!(p.max > 0.0f && p.max <= kMaxRayLength)) {
        ctx.push("max");
        ctx.error("E_PARAMS", *ctx.doc.member(params, "max"), "max is outside (0, 100000]", "a distance in (0, 100000] m");
        ctx.pop();
        c.paramsFailed = true;
        return false;
    }
    PhysicsScene& physics = *physicsScene(c.world);
    syncPhysics(c.world);
    const std::optional<RayHit> hit = physics.raycast(p.from, p.dir, p.max);
    out.beginObject();
    out.key("hit"); out.boolean(hit.has_value());
    if (hit) {
        const auto& reg = c.world.registry();
        const bool known = hit->entity != entt::null && reg.valid(hit->entity);
        out.key("entity"); out.string(known ? hex16(c.world.idOf(hit->entity)) : std::string());
        const auto* name = known ? reg.try_get<Name>(hit->entity) : nullptr;
        out.key("name"); out.string(name ? utf8Prefix(name->value, kMaxName) : std::string());
        out.key("point"); writeVec(out, hit->point);
        out.key("normal"); writeVec(out, hit->normal);
        out.key("dist"); out.float32(r4(hit->dist));
    }
    out.endObject();
    return true;
}

bool info(Call& c, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    PhysicsInfoParams p;
    if (!decode(c, params, ctx, p)) return false;
    PhysicsScene& physics = *physicsScene(c.world);
    syncPhysics(c.world);
    const PhysicsInfo info = physics.info();
    out.beginObject();
    out.key("bodies");
    out.beginObject();
    if (info.bodies.statics) { out.key("static"); out.uint64(info.bodies.statics); }
    if (info.bodies.dynamics) { out.key("dynamic"); out.uint64(info.bodies.dynamics); }
    if (info.bodies.kinematics) { out.key("kinematic"); out.uint64(info.bodies.kinematics); }
    out.endObject();
    out.key("shapes");
    out.beginObject();
    const std::pair<const char*, uint32_t> shapes[] = {{"box", info.shapes.box},       {"sphere", info.shapes.sphere},
                                                       {"capsule", info.shapes.capsule}, {"convex", info.shapes.convex},
                                                       {"mesh", info.shapes.mesh},       {"compound", info.shapes.compound}};
    for (const auto& [name, n] : shapes)
        if (n) { out.key(name); out.uint64(n); }
    out.endObject();
    out.key("steps"); out.uint64(info.steps);
    out.key("awake"); out.uint64(info.awake);
    out.key("contacts"); out.uint64(info.contacts);
    if (info.triggers) { out.key("triggers"); out.uint64(info.triggers); }
    if (info.players) { out.key("players"); out.uint64(info.players); }
    if (info.joints) { out.key("joints"); out.uint64(info.joints); }
    if (info.brokenJoints) { out.key("brokenJoints"); out.uint64(info.brokenJoints); }
    out.key("buildMs"); out.float32(ms3(info.buildMs));
    const auto& warnings = physics.warnings();
    if (!warnings.empty()) {
        const auto& reg = c.world.registry();
        out.key("warnings");
        out.beginArray();
        for (std::size_t i = 0; i < warnings.size() && i < kMaxWarnings * 2; ++i) {
            const PhysicsWarning& w = warnings[i];
            const bool alive = reg.valid(w.entity);
            const auto* name = alive ? reg.try_get<Name>(w.entity) : nullptr;
            out.beginObject();
            out.key("code"); out.string(w.code);
            out.key("entity"); out.string(alive ? hex16(c.world.idOf(w.entity)) : std::string());
            out.key("name"); out.string(name ? name->value : std::string());
            out.key("message"); out.string(w.message);
            out.endObject();
        }
        out.endArray();
        if (warnings.size() > kMaxWarnings * 2) { out.key("moreWarnings"); out.uint64(warnings.size() - kMaxWarnings * 2); }
    }
    out.endObject();
    return true;
}

bool contacts(Call& c, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    PhysicsContactsParams p;
    if (!decode(c, params, ctx, p)) return false;
    PhysicsScene& physics = *physicsScene(c.world);
    syncPhysics(c.world);
    entt::entity only = entt::null;
    if (p.entity) {
        only = c.world.findById(*p.entity);
        const std::vector<entt::entity> players = physics.players();
        const bool hasBody = only != entt::null &&
                             (physics.hasBody(only) || std::find(players.begin(), players.end(), only) != players.end());
        if (!hasBody) {
            ctx.push("entity");
            ctx.error("E_NOT_FOUND", *ctx.doc.member(params, "entity"),
                      "entity " + hex16(*p.entity) + (only == entt::null ? " does not exist" : " has no body (no Collider)"),
                      "the id of an entity with a Collider or a PlayerController", "\"" + hex16(*p.entity) + "\"");
            ctx.pop();
            return false;
        }
    }
    const std::vector<PhysicsContact> all = physics.contacts();
    std::size_t count = 0, listed = 0;
    out.beginObject();
    out.key("contacts");
    out.beginArray();
    for (const PhysicsContact& k : all) {
        if (only != entt::null && k.a != only && k.b != only) continue;
        ++count;
        if (listed == static_cast<std::size_t>(p.max)) continue;
        ++listed;
        out.beginObject();
        out.key("a"); out.string(hex16(k.aId));
        out.key("b"); out.string(hex16(k.bId));
        out.key("an"); out.string(nameOrId(c.world, k.a));
        out.key("bn"); out.string(nameOrId(c.world, k.b));
        if (k.trigger) { out.key("trigger"); out.boolean(true); }
        out.key("point"); writeVec3(out, k.point, 1e3f);
        out.key("normal"); writeVec3(out, k.normal, 1e3f);
        out.key("since"); out.uint64(k.since);
        out.endObject();
    }
    out.endArray();
    out.key("count"); out.uint64(count);
    if (count > listed) { out.key("more"); out.uint64(count - listed); }
    out.endObject();
    return true;
}

bool playerInfo(Call& c, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    PlayerInfoParams p;
    if (!decode(c, params, ctx, p)) return false;
    PhysicsScene& physics = *physicsScene(c.world);
    syncPlayers(c.world);
    const entt::entity e = findPlayer(c.world, physics, params, p.entity, ctx);
    if (e == entt::null) return false;
    writePlayerInfo(c.world, e, *physics.playerInfo(e), out);
    return true;
}

bool playerTeleport(Call& c, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    PlayerTeleportParams p;
    if (!decode(c, params, ctx, p)) return false;
    PhysicsScene& physics = *physicsScene(c.world);
    syncPlayers(c.world);
    const entt::entity e = findPlayer(c.world, physics, params, p.entity, ctx);
    if (e == entt::null) return false;
    physics.teleportPlayer(e, p.pos, p.yawDeg);
    writePlayerInfo(c.world, e, *physics.playerInfo(e), out);
    return true;
}

} // namespace

std::span<const Method> physicsMethods() {
    static const std::vector<Method> methods = {
        {{"physics.raycast", paramsTypes().find<PhysicsRaycastParams>(), "{hit, entity?, name?, point?, normal?, dist?}",
          "The closest collider along a ray. E_PARAMS for a zero dir or max outside (0, 100000]."},
         raycast},
        {{"physics.info", paramsTypes().find<PhysicsInfoParams>(),
          "{bodies, shapes, steps, awake, contacts, triggers?, players?, buildMs, warnings?: [{code, entity, name, message}]}",
          "The physics scene: bodies by motion, shapes by kind, steps run, awake dynamic bodies, pairs in contact."},
         info},
        {{"physics.contacts", paramsTypes().find<PhysicsContactsParams>(),
          "{contacts: [{a, b, an, bn, trigger?, point, normal, since}], count, more?}",
          "Body pairs touching now (resting included) and bodies in triggers, by id; since: the step they began."},
         contacts},
        {{"player.info", paramsTypes().find<PlayerInfoParams>(),
          "{entity, name, pos, yawDeg, pitchDeg, velocity, ground, on?, surface?, footsteps}",
          "The player: feet, facing, look, velocity, the body it stands on and its surface. Drive it with "
          "input.inject and world.step."},
         playerInfo},
        {{"player.teleport", paramsTypes().find<PlayerTeleportParams>(), "as player.info",
          "Puts the player's feet at pos, at rest, facing yawDeg (else as it faced); its ground is found there at once."},
         playerTeleport, true},
    };
    return methods;
}

} // namespace ohao::ecs
