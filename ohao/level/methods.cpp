// The level module's API methods (level/module.hpp): level.info and level.reload (sub-project 5, step 5;
// docs/specs/2026-10-26-level-pipeline-design.md, "The agent-facing surface"), rows of the session's catalogue where
// the module is installed, with their params in the module's own registry. Each first runs the level, transform and
// physics sync systems (never instantiate or the render extract), so a query right after scene.load or world.apply
// sees the level's nodes and bodies; those systems do nothing when nothing changed.
#include "level/module.hpp"

#include "ecs/components.hpp"
#include "ecs/world.hpp"
#include "level/components.hpp"
#include "level/level_system.hpp"
#include "physics/components.hpp"
#include "physics/physics_scene.hpp"
#include "reflect/decode.hpp"
#include "reflect/json_value.hpp"
#include "reflect/json_writer.hpp"
#include "reflect/registry.hpp"
#include "visual/components.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace ohao::ecs {

namespace {

using reflect::DecodeContext;
using reflect::Diagnostic;
using reflect::hex16;
using reflect::JsonNode;
using reflect::JsonWriter;

constexpr std::size_t kMaxSpawns = 8;     // level.info lists the first spawns (query --has SpawnPoint for all)
constexpr std::size_t kMaxWarnings = 4;   // and the first warnings, then "moreWarnings"

struct LevelInfoParams {
    std::optional<uint64_t> entity;   // absent: the world's only level
};
struct LevelReloadParams {
    std::optional<uint64_t> entity;
};

const reflect::TypeRegistry& paramsTypes() {
    static reflect::TypeRegistry r;
    static const bool once = [] {
        r.type<LevelInfoParams>("LevelInfoParams").doc("level.info params")
            .field("entity", &LevelInfoParams::entity).doc("The Level entity's id, 16 hex digits; absent: the only level");
        r.type<LevelReloadParams>("LevelReloadParams").doc("level.reload params")
            .field("entity", &LevelReloadParams::entity).doc("The Level entity's id, 16 hex digits; absent: the only level");
        return true;
    }();
    (void)once;
    return r;
}

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

// The derived state the queries read: the level's nodes, their world transforms, the bodies.
void syncPhysics(World& world) {
    world.runSync("level");
    world.runSync("transform");
    world.runSync("physics");
}

// The Level entity the params name, or the world's only one. Reports and returns null.
entt::entity findLevel(World& world, const JsonNode& params, const std::optional<uint64_t>& id,
                       DecodeContext& ctx) {
    const auto& reg = world.registry();
    if (id) {
        const entt::entity e = world.findById(*id);
        if (e == entt::null || !reg.all_of<Level>(e)) {
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
    for (const entt::entity e : reg.view<const Level>()) levels.push_back(e);
    if (levels.empty()) {
        ctx.error("E_NOT_FOUND", params, "the world has no level", "scene.load a scene with a Level first");
        return entt::null;
    }
    if (levels.size() > 1) {
        std::sort(levels.begin(), levels.end(), [&](entt::entity a, entt::entity b) { return world.idOf(a) < world.idOf(b); });
        std::string ids;
        for (const entt::entity e : levels) ids += (ids.empty() ? "" : ", ") + hex16(world.idOf(e));
        ctx.error("E_COMMAND", params, std::to_string(levels.size()) + " levels in the world; name one with entity",
                  "{\"entity\": one of " + ids + "}");
        return entt::null;
    }
    return levels.front();
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
bool writeLevelInfo(World& world, entt::entity e, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    const auto& reg = world.registry();
    const std::optional<level::LevelSummary> sum = level::levelSummary(world, e);
    std::vector<Warning> warnings;
    std::vector<Warning> errors;
    for (const Diagnostic& d : level::levelDiagnostics(world, e))
        (d.code.starts_with("W_") ? warnings : errors).push_back({d.code, d.message});
    if (!sum || !sum->loaded) {
        const auto* err = reg.try_get<InstantiateError>(e);
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
    for (auto [n, ln] : reg.view<const LevelNode>().each()) {
        if (ln.level != e) continue;
        if (const auto* sp = reg.try_get<SpawnPoint>(n)) {
            spawns.push_back({ln.index, n});
            player = player || sp->tag == "player";
        }
        if (const auto* c = reg.try_get<Collider>(n); c && c->shape != ColliderShape::None)
            ++colliders[static_cast<std::size_t>(c->shape)];
        if (reg.all_of<PhysicsBody>(n)) {
            const auto* rb = reg.try_get<RigidBody>(n);
            ++bodies[static_cast<std::size_t>(rb ? rb->motion : Motion::Static)];
        }
    }
    std::sort(spawns.begin(), spawns.end(), [](const Spawn& a, const Spawn& b) { return a.index < b.index; });
    if (!player)
        warnings.push_back({"W_LEVEL_NO_PLAYER_SPAWN", "no node has the spawn hint \"player\" (a prop kit needs none)"});
    if (const PhysicsScene* physics = physicsScene(world))
        for (const PhysicsWarning& w : physics->warnings()) {
            const auto* ln = reg.valid(w.entity) ? reg.try_get<LevelNode>(w.entity) : nullptr;
            if (!ln || ln->level != e) continue;
            const auto* name = reg.try_get<Name>(w.entity);
            warnings.push_back({w.code, "'" + (name ? name->value : std::string("?")) + "': " + w.message});
        }

    out.beginObject();
    out.key("entity"); out.string(hex16(world.idOf(e)));
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
        const auto* wt = reg.try_get<WorldTransform>(n);
        const glm::mat4 m = wt ? wt->world : reg.get<Transform>(n).matrix();
        out.beginObject();
        out.key("tag"); out.string(reg.get<SpawnPoint>(n).tag);
        out.key("entity"); out.string(hex16(world.idOf(n)));
        out.key("pos"); writeVec(out, glm::vec3(m[3]));
        out.key("yawDeg"); out.float32(yawDegrees(m));
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

template <class P>
bool decode(Call& c, const JsonNode& params, DecodeContext& ctx, P& out) {
    return decodeParams(c, *paramsTypes().find<P>(), params, ctx, &out);
}

bool info(Call& c, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    LevelInfoParams p;
    if (!decode(c, params, ctx, p)) return false;
    syncPhysics(c.world);
    const entt::entity e = findLevel(c.world, params, p.entity, ctx);
    return e != entt::null && writeLevelInfo(c.world, e, params, ctx, out);
}

bool reload(Call& c, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    LevelReloadParams p;
    if (!decode(c, params, ctx, p)) return false;
    syncPhysics(c.world);   // an edit not yet expanded is expanded first, from the file as it was
    const entt::entity e = findLevel(c.world, params, p.entity, ctx);
    if (e == entt::null) return false;
    (void)level::reloadLevel(c.world, e);
    syncPhysics(c.world);
    return writeLevelInfo(c.world, e, params, ctx, out);
}

} // namespace

std::span<const Method> levelMethods() {
    static const std::vector<Method> methods = {
        {{"level.info", paramsTypes().find<LevelInfoParams>(),
          "{entity, path, nodes, meshes, uniqueMeshes, triangles, colliders, bodies, spawns: [{tag, entity, pos, yawDeg}], "
          "lights, cameras, warnings, errors?, loadMs}",
          "A Level's summary: counts, colliders by shape, bodies by motion, spawns in node order (first 8). "
          "E_NOT_FOUND for no level or a non-level entity."},
         info},
        {{"level.reload", paramsTypes().find<LevelReloadParams>(), "as level.info",
          "Re-reads the level's file and rebuilds its entities and bodies in place; an unchanged file keeps world.hash."},
         reload, true},
    };
    return methods;
}

} // namespace ohao::ecs
