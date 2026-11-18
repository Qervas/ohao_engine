// level.info and level.reload (sub-project 5, step 5; docs/specs/2026-10-26-level-pipeline-design.md, "The
// agent-facing surface"), asset.info and asset.reload (system design step 5h).
//
// The session reaches level data and bodies through what the World's modules left on it:
// level::levelSummary / levelDiagnostics / reloadLevel (installLevels, ohao_level_world) and
// ecs::physicsScene (the physics module). A World without levels answers E_STATE. Each method
// first runs the level, transform and physics sync systems (never instantiate or the render
// extract), so a query right after scene.load or world.apply sees the level's nodes and bodies;
// those systems do nothing when nothing changed.
#include "api/internal.hpp"

#include "ecs/components.hpp"
#include "visual/components.hpp"
#include "physics/components.hpp"
#include "physics/physics_scene.hpp"
#include "level/level_system.hpp"

#include <map>
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
void syncPhysics(Session::Impl& s) {
    s.world.runSync("level");
    s.world.runSync("transform");
    s.world.runSync("physics");
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

// The entities whose MeshSource or CollisionMesh is the model file `path` (every model file when empty).
template <class F> void forModelUsers(Session::Impl& s, std::string_view path, F&& fn) {
    auto& reg = s.world.registry();
    for (const entt::entity e : reg.view<ecs::EntityId>()) {
        const auto* ms = reg.try_get<ecs::MeshSource>(e);
        const auto* model = ms ? std::get_if<ecs::ModelAsset>(&ms->source) : nullptr;
        const auto* cm = reg.try_get<ecs::CollisionMesh>(e);
        const bool mesh = model && (path.empty() || model->asset.path == path);
        const bool collision = cm && (path.empty() || cm->source.asset.path == path);
        if (mesh || collision) fn(e, mesh ? model->asset : cm->source.asset);
    }
}

bool methodAssetInfo(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    AssetInfoParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    constexpr std::size_t kMaxModels = 32;
    std::map<std::string, uint32_t> users;
    forModelUsers(s, {}, [&](entt::entity, const ecs::AssetRef& ref) { ++users[ref.path]; });
    std::vector<ecs::AssetResolver::Loaded> loaded;
    if (s.assets) loaded = s.assets->loaded();
    out.beginObject();
    out.key("models");
    out.beginArray();
    for (std::size_t i = 0; i < loaded.size() && i < kMaxModels; ++i) {
        const auto& l = loaded[i];
        const std::string file = l.path.substr(0, l.path.find(" (meshes)"));
        const auto it = users.find(file);
        out.beginObject();
        out.key("path"); out.string(l.path);
        out.key("entities"); out.uint64(it == users.end() ? 0 : it->second);
        out.key("meshes"); out.uint64(l.meshes);
        out.key("cookedBytes"); out.uint64(l.cookedBytes);
        out.endObject();
    }
    out.endArray();
    if (loaded.size() > kMaxModels) { out.key("more"); out.uint64(loaded.size() - kMaxModels); }
    out.key("cache"); out.string(".ohao/cooked");
    out.endObject();
    return true;
}

bool methodAssetReload(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    AssetReloadParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    if (!s.assets) {
        ctx.error("E_COMMAND", params, "this session has no asset resolver", "a host that loads assets (ohao serve, ohao run)");
        return false;
    }
    if (p.path) s.assets->forget(ecs::AssetRef{*p.path, {}});
    else s.assets->forgetAll();
    // The users' meshes and bodies are rebuilt from the file as it is now.
    auto& reg = s.world.registry();
    std::vector<entt::entity> touched;
    forModelUsers(s, p.path ? std::string_view(*p.path) : std::string_view{}, [&](entt::entity e, const ecs::AssetRef&) { touched.push_back(e); });
    for (const entt::entity e : touched) {
        if (reg.all_of<ecs::MeshSource>(e)) reg.patch<ecs::MeshSource>(e);
        if (reg.all_of<ecs::Collider>(e)) reg.patch<ecs::Collider>(e);
    }
    syncPhysics(s);
    out.beginObject();
    out.key("entities"); out.uint64(touched.size());
    out.endObject();
    return true;
}

} // namespace ohao::api
