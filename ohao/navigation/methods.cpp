// The navigation module's API methods (navigation/module.hpp): nav.info, nav.path, nav.nearest and nav.reach. Each
// first runs the level, transform and physics sync systems, so a query right after scene.load or world.apply sees the
// level's colliders; the navmesh is then built if it is out of date and its obstacles applied.
#include "navigation/module.hpp"
#include "navigation/navigation.hpp"

#include "ecs/world.hpp"
#include "reflect/decode.hpp"
#include "reflect/json_value.hpp"
#include "reflect/json_writer.hpp"
#include "reflect/registry.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace ohao::ecs {

namespace {

using reflect::DecodeContext;
using reflect::JsonNode;
using reflect::JsonWriter;

constexpr uint32_t kMaxReach = 256;

struct NavInfoParams {};
struct NavPathParams {
    glm::vec3 from{0.0f};
    glm::vec3 to{0.0f};
    uint32_t maxCorners{32};
};
struct NavNearestParams {
    glm::vec3 point{0.0f};
    glm::vec3 extents{2.0f, 4.0f, 2.0f};
};
struct NavReachParams {
    glm::vec3 from{0.0f};
    std::vector<glm::vec3> points;
};

const reflect::TypeRegistry& paramsTypes() {
    static reflect::TypeRegistry r;
    static const bool once = [] {
        r.type<NavInfoParams>("NavInfoParams").doc("nav.info params");
        r.type<NavPathParams>("NavPathParams").doc("nav.path params")
            .field("from", &NavPathParams::from).doc("Where the path starts, world").unit("m").required()
            .field("to", &NavPathParams::to).doc("Where it goes, world").unit("m").required()
            .field("maxCorners", &NavPathParams::maxCorners).doc("At most this many corners").unitless().min(2).max(256);
        r.type<NavNearestParams>("NavNearestParams").doc("nav.nearest params")
            .field("point", &NavNearestParams::point).doc("The point, world").unit("m").required()
            .field("extents", &NavNearestParams::extents).doc("Half sizes of the box searched about it").unit("m").min(0.01);
        r.type<NavReachParams>("NavReachParams").doc("nav.reach params")
            .field("from", &NavReachParams::from).doc("Where the paths start, world").unit("m").required()
            .field("points", &NavReachParams::points).doc("The points to reach, world").unit("m").maxItems(kMaxReach).required();
        return true;
    }();
    (void)once;
    return r;
}

template <class P>
bool decode(Call& c, const JsonNode& params, DecodeContext& ctx, P& out) {
    return decodeParams(c, *paramsTypes().find<P>(), params, ctx, &out);
}

float r3(float v) {
    if (std::fabs(v) < 0.5e-3f) return 0.0f;
    return std::round(v * 1e3f) / 1e3f;
}
float ms3(double v) { return static_cast<float>(std::round(v * 1000.0) / 1000.0); }

void writeVec(JsonWriter& out, glm::vec3 v) {
    out.beginArray();
    out.float32(r3(v.x)); out.float32(r3(v.y)); out.float32(r3(v.z));
    out.endArray();
}

void syncAll(World& world) {
    world.runSync("level");
    world.runSync("transform");
    world.runSync("physics");
}

const char* statusName(NavRoute::Status s) {
    switch (s) {
        case NavRoute::Status::Found: return "found";
        case NavRoute::Status::Partial: return "partial";
        case NavRoute::Status::NoPath: return "none";
    }
    return "none";
}

bool info(Call& c, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    NavInfoParams p;
    if (!decode(c, params, ctx, p)) return false;
    syncAll(c.world);
    const auto n = navInfo(c.world);
    if (!n) {
        ctx.error("E_STATE", params, "the world has no navigation", "a host that installs the navigation module");
        return false;
    }
    out.beginObject();
    out.key("built"); out.boolean(n->built);
    out.key("version"); out.string(reflect::hex16(n->version));
    out.key("tiles"); out.uint64(n->tiles);
    out.key("cooked"); out.uint64(n->cooked);
    out.key("cached"); out.uint64(n->cached);
    out.key("layers"); out.uint64(n->layers);
    out.key("polys"); out.uint64(n->polys);
    out.key("triangles"); out.uint64(n->triangles);
    out.key("links"); out.uint64(n->links);
    out.key("obstacles"); out.uint64(n->obstacles);
    out.key("agents"); out.uint64(n->agents);
    out.key("settings");
    out.beginObject();
    out.key("cellSize"); out.float32(r3(n->cellSize));
    out.key("cellHeight"); out.float32(r3(n->cellHeight));
    out.key("tileSize"); out.float32(r3(n->tileSize));
    out.key("agentRadius"); out.float32(r3(n->agentRadius));
    out.key("agentHeight"); out.float32(r3(n->agentHeight));
    out.key("agentClimb"); out.float32(r3(n->agentClimb));
    out.key("slopeDeg"); out.float32(r3(n->slopeDeg));
    out.endObject();
    out.key("cookMs"); out.float32(ms3(n->cookMs));
    out.key("buildMs"); out.float32(ms3(n->buildMs));
    out.key("obstaclesMs"); out.float32(ms3(n->obstaclesMs));
    out.key("warnings");
    out.beginArray();
    for (const std::string& w : n->warnings) out.string(w);
    out.endArray();
    out.endObject();
    return true;
}

void writeRoute(JsonWriter& out, const NavRoute& r) {
    out.beginObject();
    out.key("status"); out.string(statusName(r.status));
    if (r.status == NavRoute::Status::NoPath) {
        out.key("why"); out.string(r.why);
    } else {
        out.key("length"); out.float32(r3(r.length));
        out.key("polys"); out.uint64(r.polys);
        out.key("corners");
        out.beginArray();
        for (const glm::vec3& v : r.corners) writeVec(out, v);
        out.endArray();
    }
    out.endObject();
}

bool path(Call& c, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    NavPathParams p;
    if (!decode(c, params, ctx, p)) return false;
    syncAll(c.world);
    const auto r = navPath(c.world, p.from, p.to, p.maxCorners);
    if (!r) {
        ctx.error("E_STATE", params, "the world has no navigation", "a host that installs the navigation module");
        return false;
    }
    writeRoute(out, *r);
    return true;
}

bool nearest(Call& c, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    NavNearestParams p;
    if (!decode(c, params, ctx, p)) return false;
    syncAll(c.world);
    if (!navInfo(c.world)) {
        ctx.error("E_STATE", params, "the world has no navigation", "a host that installs the navigation module");
        return false;
    }
    const auto at = navNearest(c.world, p.point, p.extents);
    out.beginObject();
    out.key("found"); out.boolean(at.has_value());
    if (at) {
        out.key("point"); writeVec(out, *at);
        out.key("distance"); out.float32(r3(glm::length(*at - p.point)));
    }
    out.endObject();
    return true;
}

bool reach(Call& c, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    NavReachParams p;
    if (!decode(c, params, ctx, p)) return false;
    syncAll(c.world);
    if (!navInfo(c.world)) {
        ctx.error("E_STATE", params, "the world has no navigation", "a host that installs the navigation module");
        return false;
    }
    uint32_t reachable = 0;
    out.beginObject();
    out.key("points");
    out.beginArray();
    for (const glm::vec3& q : p.points) {
        const auto r = navPath(c.world, p.from, q, 256);
        const bool ok = r && r->status == NavRoute::Status::Found;
        reachable += ok ? 1u : 0u;
        out.beginObject();
        out.key("reachable"); out.boolean(ok);
        if (ok) {
            out.key("length"); out.float32(r3(r->length));
        } else if (r && r->status == NavRoute::Status::Partial && !r->corners.empty()) {
            out.key("nearest"); writeVec(out, r->corners.back());
        }
        out.endObject();
    }
    out.endArray();
    out.key("reachable"); out.uint64(reachable);
    out.key("unreachable"); out.uint64(p.points.size() - reachable);
    out.endObject();
    return true;
}

} // namespace

std::span<const Method> navigationMethods() {
    static const std::vector<Method> methods = {
        {{"nav.info", paramsTypes().find<NavInfoParams>(),
          "{built, version, tiles, cooked, cached, layers, polys, triangles, links, obstacles, agents, settings: "
          "{cellSize, cellHeight, tileSize, agentRadius, agentHeight, agentClimb, slopeDeg}, cookMs, buildMs, obstaclesMs, "
          "warnings}",
          "The navmesh: its tiles (cooked by the last build, or read from the cooked cache), polygons, the static "
          "triangles it was built from, links, active obstacles, agents, settings and the last build's times. Builds "
          "it first when it is out of date."},
         info},
        {{"nav.path", paramsTypes().find<NavPathParams>(),
          "{status: found|partial|none, length?, polys?, corners?: [[x, y, z]], why?}",
          "The shortest walkable path between two points (each snapped to the navmesh within 2 m across, 4 m up and "
          "down), string-pulled to its corners. partial: the goal is not reachable; the corners end at the reachable "
          "point nearest it."},
         path},
        {{"nav.nearest", paramsTypes().find<NavNearestParams>(), "{found, point?, distance?}",
          "The nearest navmesh point to a point, within a box of half sizes `extents` about it."},
         nearest},
        {{"nav.reach", paramsTypes().find<NavReachParams>(),
          "{points: [{reachable, length?, nearest?}], reachable, unreachable}",
          "Which of up to 256 points a path from `from` reaches, and how far each is along it: every spawn, post and "
          "objective of a level checked at once. An unreachable point gives the nearest point a path reaches."},
         reach},
    };
    return methods;
}

} // namespace ohao::ecs
