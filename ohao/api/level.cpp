// level.info, level.reload, physics.raycast, physics.info (sub-project 5, step 5;
// docs/specs/2026-10-26-level-pipeline-design.md, "The agent-facing surface"), physics.events
// (sub-project 6, step 1).
//
// The session reaches level data and bodies through what the World's install hooks left on it:
// level::levelSummary / levelDiagnostics / reloadLevel (installLevels, ohao_level_world) and
// ecs::physicsScene (installPhysics, ohao_ecs). A World without them answers E_STATE. Each method
// first runs the level, transform and physics sync systems (never instantiate or the render
// extract), so a query right after scene.load or world.apply sees the level's nodes and bodies;
// those systems do nothing when nothing changed.
#include "api/internal.hpp"

#include "ecs/components.hpp"
#include "ecs/physics_scene.hpp"
#include "level/level_system.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace ohao::api {

namespace {

constexpr std::size_t kMaxSpawns = 8;     // level.info lists the first spawns (query --has SpawnPoint for all)
constexpr std::size_t kMaxWarnings = 4;   // and the first warnings, then "moreWarnings"
constexpr std::size_t kMaxName = 40;      // physics.raycast's name, in bytes (a whole UTF-8 sequence)
constexpr std::size_t kMaxEventName = 24; // physics.events' an and bn, in bytes

// Positions to 0.1 mm, directions to 1e-4: short numbers, and no -0 (tested by magnitude: under
// /fp:fast `r == 0 ? 0 : r` folds to r and keeps the sign).
float r4(float v) {
    if (std::fabs(v) < 0.5e-4f) return 0.0f;
    return std::round(v * 1e4f) / 1e4f;
}
float ms3(double v) { return static_cast<float>(std::round(v * 1000.0) / 1000.0); }

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

// The derived state the queries read: the level's nodes, their world transforms, the bodies.
void syncPhysics(Session::Impl& s) {
    s.world.runSync("level");
    s.world.runSync("transform");
    s.world.runSync("physics");
}

std::string utf8Prefix(const std::string& s, std::size_t bytes) {
    if (s.size() <= bytes) return s;
    std::size_t n = bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;   // not inside a sequence
    return s.substr(0, n);
}

// The Level entity the params name, or the world's only one. Reports and returns null.
entt::entity findLevel(Session::Impl& s, const JsonNode& params, const std::optional<uint64_t>& id,
                       DecodeContext& ctx) {
    const auto& reg = s.world.registry();
    if (id) {
        const entt::entity e = s.world.findById(*id);
        if (e == entt::null || !reg.all_of<ecs::Level>(e)) {
            ctx.push("entity");
            ctx.error("E_NOT_FOUND", *ctx.doc.member(params, "entity"),
                      "entity " + hex16(*id) + (e == entt::null ? " does not exist" : " is not a level (it has no Level)"),
                      "the id of an entity with a Level", "\"" + hex16(*id) + "\"");
            ctx.pop();
            return entt::null;
        }
        return e;
    }
    std::vector<entt::entity> levels;
    for (const entt::entity e : reg.view<const ecs::Level>()) levels.push_back(e);
    if (levels.empty()) {
        ctx.error("E_NOT_FOUND", params, "the world has no level", "scene.load a scene with a Level first");
        return entt::null;
    }
    if (levels.size() > 1) {
        std::sort(levels.begin(), levels.end(), [&](entt::entity a, entt::entity b) { return s.world.idOf(a) < s.world.idOf(b); });
        std::string ids;
        for (const entt::entity e : levels) ids += (ids.empty() ? "" : ", ") + hex16(s.world.idOf(e));
        ctx.error("E_COMMAND", params, std::to_string(levels.size()) + " levels in the world; name one with entity",
                  "{\"entity\": one of " + ids + "}");
        return entt::null;
    }
    return levels.front();
}

bool needLevels(Session::Impl& s, const JsonNode& params, DecodeContext& ctx) {
    if (level::levelsInstalled(s.world)) return true;
    ctx.error("E_STATE", params, "this world has no level system", "a host that installs levels (ohao serve, ohao run)");
    return false;
}

ecs::PhysicsScene* needPhysics(Session::Impl& s, const JsonNode& params, DecodeContext& ctx) {
    ecs::PhysicsScene* physics = ecs::physicsScene(s.world);
    if (!physics) ctx.error("E_STATE", params, "this world has no physics system", "a host that installs physics (ohao serve, ohao run)");
    return physics;
}

// Facing: the entity's local -Z, as a yaw about +Y from -Z (counter-clockwise seen from above).
float yawDegrees(const glm::mat4& world) {
    const glm::vec3 fwd = -glm::vec3(world[2]);
    if (std::fabs(fwd.x) < 1e-6f && std::fabs(fwd.z) < 1e-6f) return 0.0f;
    float deg = glm::degrees(std::atan2(-fwd.x, -fwd.z));
    deg = std::round(deg * 10.0f) / 10.0f;
    if (deg <= -180.0f) deg = 180.0f;
    return deg == 0.0f ? 0.0f : deg;
}

struct Warning {
    std::string code, message;
};

// level.info's result (level.reload's too). False after reporting (the file did not load).
bool writeLevelInfo(Session::Impl& s, entt::entity e, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    const auto& reg = s.world.registry();
    const std::optional<level::LevelSummary> sum = level::levelSummary(s.world, e);
    std::vector<Warning> warnings;
    std::vector<Warning> errors;
    for (const Diagnostic& d : level::levelDiagnostics(s.world, e))
        (d.code.starts_with("W_") ? warnings : errors).push_back({d.code, d.message});
    if (!sum || !sum->loaded) {
        const auto* err = reg.try_get<ecs::InstantiateError>(e);
        ctx.error(err ? err->code : "E_ASSET_INVALID", params,
                  err ? err->message : "the level has not been expanded");
        return false;
    }

    // Spawns in node order; colliders by shape and bodies by motion over the level's nodes.
    struct Spawn {
        uint32_t index;
        entt::entity e;
    };
    std::vector<Spawn> spawns;
    std::array<uint32_t, 5> colliders{};   // Box, Sphere, Capsule, Convex, Mesh
    std::array<uint32_t, 3> bodies{};      // static, dynamic, kinematic
    bool player = false;
    for (auto [n, ln] : reg.view<const ecs::LevelNode>().each()) {
        if (ln.level != e) continue;
        if (const auto* sp = reg.try_get<ecs::SpawnPoint>(n)) {
            spawns.push_back({ln.index, n});
            player = player || sp->tag == "player";
        }
        if (const auto* c = reg.try_get<ecs::Collider>(n); c && c->shape != ecs::ColliderShape::None)
            ++colliders[static_cast<std::size_t>(c->shape)];
        if (reg.all_of<ecs::PhysicsBody>(n)) {
            const auto* rb = reg.try_get<ecs::RigidBody>(n);
            ++bodies[static_cast<std::size_t>(rb ? rb->motion : ecs::Motion::Static)];
        }
    }
    std::sort(spawns.begin(), spawns.end(), [](const Spawn& a, const Spawn& b) { return a.index < b.index; });
    if (!player)
        warnings.push_back({"W_LEVEL_NO_PLAYER_SPAWN", "no node has the spawn hint \"player\" (a prop kit needs none)"});
    if (const ecs::PhysicsScene* physics = ecs::physicsScene(s.world))
        for (const ecs::PhysicsWarning& w : physics->warnings()) {
            const auto* ln = reg.valid(w.entity) ? reg.try_get<ecs::LevelNode>(w.entity) : nullptr;
            if (!ln || ln->level != e) continue;
            const auto* name = reg.try_get<ecs::Name>(w.entity);
            warnings.push_back({w.code, "'" + (name ? name->value : std::string("?")) + "': " + w.message});
        }

    out.beginObject();
    out.key("entity"); out.string(hex16(s.world.idOf(e)));
    out.key("path"); out.string(sum->path);
    out.key("nodes"); out.uint64(sum->nodes);
    out.key("meshes"); out.uint64(sum->meshes);
    out.key("uniqueMeshes"); out.uint64(sum->uniqueMeshes);
    out.key("triangles"); out.uint64(sum->triangles);
    static constexpr const char* kShapes[] = {"box", "sphere", "capsule", "convex", "mesh"};
    out.key("colliders");
    out.beginObject();
    for (std::size_t i = 0; i < colliders.size(); ++i)
        if (colliders[i]) { out.key(kShapes[i]); out.uint64(colliders[i]); }
    out.endObject();
    static constexpr const char* kMotions[] = {"static", "dynamic", "kinematic"};
    out.key("bodies");
    out.beginObject();
    for (std::size_t i = 0; i < bodies.size(); ++i)
        if (bodies[i]) { out.key(kMotions[i]); out.uint64(bodies[i]); }
    out.endObject();
    out.key("spawns");
    out.beginArray();
    for (std::size_t i = 0; i < spawns.size() && i < kMaxSpawns; ++i) {
        const entt::entity n = spawns[i].e;
        const auto* wt = reg.try_get<ecs::WorldTransform>(n);
        const glm::mat4 world = wt ? wt->world : reg.get<ecs::Transform>(n).matrix();
        out.beginObject();
        out.key("tag"); out.string(reg.get<ecs::SpawnPoint>(n).tag);
        out.key("entity"); out.string(hex16(s.world.idOf(n)));
        out.key("pos"); writeVec(out, glm::vec3(world[3]));
        out.key("yawDeg"); out.float32(yawDegrees(world));
        out.endObject();
    }
    out.endArray();
    if (spawns.size() > kMaxSpawns) { out.key("moreSpawns"); out.uint64(spawns.size() - kMaxSpawns); }
    out.key("lights"); out.uint64(sum->lights);
    out.key("cameras"); out.uint64(sum->cameras);
    const auto writeList = [&](const char* key, const char* moreKey, const std::vector<Warning>& list) {
        out.key(key);
        out.beginArray();
        for (std::size_t i = 0; i < list.size() && i < kMaxWarnings; ++i) {
            out.beginObject();
            out.key("code"); out.string(list[i].code);
            out.key("message"); out.string(list[i].message);
            out.endObject();
        }
        out.endArray();
        if (list.size() > kMaxWarnings) { out.key(moreKey); out.uint64(list.size() - kMaxWarnings); }
    };
    writeList("warnings", "moreWarnings", warnings);
    if (!errors.empty()) writeList("errors", "moreErrors", errors);
    out.key("loadMs"); out.float32(ms3(sum->loadMs));
    out.endObject();
    return true;
}

} // namespace

bool methodLevelInfo(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    LevelInfoParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    if (!needLevels(s, params, ctx)) return false;
    syncPhysics(s);
    const entt::entity e = findLevel(s, params, p.entity, ctx);
    return e != entt::null && writeLevelInfo(s, e, params, ctx, out);
}

bool methodLevelReload(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx,
                       JsonWriter& out) {
    LevelReloadParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    if (!needLevels(s, params, ctx)) return false;
    syncPhysics(s);   // an edit not yet expanded is expanded first, from the file as it was
    const entt::entity e = findLevel(s, params, p.entity, ctx);
    if (e == entt::null) return false;
    (void)level::reloadLevel(s.world, e);
    syncPhysics(s);
    return writeLevelInfo(s, e, params, ctx, out);
}

bool methodPhysicsRaycast(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx,
                          JsonWriter& out) {
    PhysicsRaycastParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    // Rules no field limit states: E_PARAMS (BadParams, as a limit would be).
    if (!(glm::dot(p.dir, p.dir) > 0.0f) || !std::isfinite(glm::dot(p.dir, p.dir))) {
        ctx.push("dir");
        ctx.error("E_PARAMS", *ctx.doc.member(params, "dir"), "the direction is zero (or not finite)",
                  "a nonzero [x, y, z]");
        ctx.pop();
        return badParams(s);
    }
    if (!(p.max > 0.0f && p.max <= kMaxRayLength)) {
        ctx.push("max");
        ctx.error("E_PARAMS", *ctx.doc.member(params, "max"), "max is outside (0, 100000]", "a distance in (0, 100000] m");
        ctx.pop();
        return badParams(s);
    }
    ecs::PhysicsScene* physics = needPhysics(s, params, ctx);
    if (!physics) return false;
    syncPhysics(s);
    const std::optional<ecs::RayHit> hit = physics->raycast(p.from, p.dir, p.max);
    out.beginObject();
    out.key("hit"); out.boolean(hit.has_value());
    if (hit) {
        const auto& reg = s.world.registry();
        const bool known = hit->entity != entt::null && reg.valid(hit->entity);
        out.key("entity"); out.string(known ? hex16(s.world.idOf(hit->entity)) : std::string());
        const auto* name = known ? reg.try_get<ecs::Name>(hit->entity) : nullptr;
        out.key("name"); out.string(name ? utf8Prefix(name->value, kMaxName) : std::string());
        out.key("point"); writeVec(out, hit->point);
        out.key("normal"); writeVec(out, hit->normal);
        out.key("dist"); out.float32(r4(hit->dist));
    }
    out.endObject();
    return true;
}

bool methodPhysicsInfo(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx,
                       JsonWriter& out) {
    PhysicsInfoParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    ecs::PhysicsScene* physics = needPhysics(s, params, ctx);
    if (!physics) return false;
    syncPhysics(s);
    const ecs::PhysicsInfo info = physics->info();
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
    out.key("buildMs"); out.float32(ms3(info.buildMs));
    const auto& warnings = physics->warnings();
    if (!warnings.empty()) {
        const auto& reg = s.world.registry();
        out.key("warnings");
        out.beginArray();
        for (std::size_t i = 0; i < warnings.size() && i < kMaxWarnings * 2; ++i) {
            const ecs::PhysicsWarning& w = warnings[i];
            const bool alive = reg.valid(w.entity);
            const auto* name = alive ? reg.try_get<ecs::Name>(w.entity) : nullptr;
            out.beginObject();
            out.key("code"); out.string(w.code);
            out.key("entity"); out.string(alive ? hex16(s.world.idOf(w.entity)) : std::string());
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

// physics.events (sub-project 6, step 1; docs/specs/2026-10-26-player-physics-design.md,
// "Events"): the log after a cursor. Reading changes nothing, so it does not sync.
bool methodPhysicsEvents(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx,
                         JsonWriter& out) {
    PhysicsEventsParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    ecs::PhysicsScene* physics = needPhysics(s, params, ctx);
    if (!physics) return false;
    uint64_t dropped = 0;
    const auto since = static_cast<uint64_t>(p.since);
    const std::vector<ecs::PhysicsEvent> events = physics->eventsSince(since, static_cast<std::size_t>(p.max), dropped);
    const auto& reg = s.world.registry();
    const auto nameOf = [&](entt::entity e) {
        const auto* n = e != entt::null && reg.valid(e) ? reg.try_get<ecs::Name>(e) : nullptr;
        return n ? utf8Prefix(n->value, kMaxEventName) : std::string();
    };
    out.beginObject();
    out.key("events");
    out.beginArray();
    for (const ecs::PhysicsEvent& e : events) {
        out.beginObject();
        out.key("seq"); out.uint64(e.seq);
        out.key("step"); out.uint64(e.step);
        out.key("kind"); out.string(ecs::physicsEventKindName(e.kind));
        out.key("a"); out.string(hex16(e.aId));
        out.key("b"); out.string(hex16(e.bId));
        out.key("an"); out.string(nameOf(e.a));
        out.key("bn"); out.string(nameOf(e.b));
        out.key("point"); writeVec3(out, e.point, 1e3f);
        out.key("normal"); writeVec3(out, e.normal, 1e3f);
        out.key("speed"); out.float32(std::round(e.speed * 100.0f) / 100.0f);
        out.endObject();
    }
    out.endArray();
    // The cursor for the next call: the last event returned, else where the caller is (never past the log).
    out.key("next"); out.uint64(events.empty() ? std::min(since, physics->nextEventSeq() - 1) : events.back().seq);
    if (dropped) { out.key("dropped"); out.uint64(dropped); }
    out.endObject();
    return true;
}

} // namespace ohao::api
