// vfx.info (sub-project 12; docs/specs/2026-10-28-vfx-design.md, "The agent-facing surface").
//
// The session reaches the effects through what installVfx left on the World (ecs::vfxSystem); a World
// without it answers E_STATE. Task 1 answers the step half: the set, the caps, the live records by
// kind and the records after a cursor. The frame half (particles live) comes with the renderer's
// effects; until then `particles` is null.
#include "api/internal.hpp"

#include "ecs/vfx.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace ohao::api {

namespace {

// To 1/100, without -0 (as combat.cpp and audio.cpp).
float r2(float v) { return std::fabs(v) < 0.005f ? 0.0f : std::round(v * 100.0f) / 100.0f; }

void writeVec3(JsonWriter& out, glm::vec3 v) {
    out.beginArray();
    for (int i = 0; i < 3; ++i) out.float32(r2(v[i]));
    out.endArray();
}

// A record's name in the set (the effect, tracer or decal it draws).
std::string nameOf(const vfx::VfxSet& set, const ecs::VfxRecord& r) {
    using K = ecs::VfxRecord::Kind;
    const std::size_t i = r.effect;
    switch (r.kind) {
    case K::Effect:
    case K::Light: return i < set.effects.size() ? set.effects[i].name : std::string();
    case K::Tracer: return i < set.tracers.size() ? set.tracers[i].name : std::string();
    case K::Decal: return i < set.decals.size() ? set.decals[i].name : std::string();
    }
    return {};
}

} // namespace

bool methodVfxInfo(Session::Impl& s, const JsonDocument&, const JsonNode& params, DecodeContext& ctx, JsonWriter& out) {
    VfxInfoParams p;
    if (!decodeParams(s, params, ctx, p)) return false;
    const ecs::VfxSystem* sys = ecs::vfxSystem(s.world);
    if (!sys) {
        ctx.error("E_STATE", params, "this world has no vfx system", "a host that installs it (ohao serve, ohao run)");
        return false;
    }
    const vfx::VfxSet& set = sys->set();
    const ecs::VfxStats st = sys->stats();
    out.beginObject();
    out.key("set"); out.string(sys->setPath().empty() ? "default" : sys->setPath());
    out.key("steps"); out.uint64(st.steps);
    out.key("caps");
    out.beginObject();
    out.key("records"); out.uint64(set.caps.records);
    out.key("particles"); out.uint64(set.caps.particles);
    out.key("decals"); out.uint64(set.caps.decals);
    out.key("tracers"); out.uint64(set.caps.tracers);
    out.key("proxies"); out.uint64(set.caps.proxies);
    out.endObject();
    const ecs::VfxRecord::Kind kinds[] = {ecs::VfxRecord::Kind::Effect, ecs::VfxRecord::Kind::Tracer, ecs::VfxRecord::Kind::Decal,
                                          ecs::VfxRecord::Kind::Light};
    out.key("live");
    out.beginObject();
    for (const auto k : kinds) {
        out.key(ecs::vfxKindName(k));
        out.uint64(st.live[static_cast<std::size_t>(k)]);
    }
    out.endObject();
    bool anyDropped = false;
    for (const uint64_t d : st.dropped) anyDropped |= d != 0;
    if (anyDropped) {   // over a cap, by kind, ever (left out while none)
        out.key("dropped");
        out.beginObject();
        for (const auto k : kinds) {
            out.key(ecs::vfxKindName(k));
            out.uint64(st.dropped[static_cast<std::size_t>(k)]);
        }
        out.endObject();
    }
    out.key("particles"); out.null();   // the frame half (Task 2): live particles from the last frame
    uint64_t next = 0;
    const std::vector<ecs::VfxRecord> records =
        p.max ? sys->recordsSince(static_cast<uint64_t>(p.since), static_cast<std::size_t>(p.max), next) : std::vector<ecs::VfxRecord>{};
    if (!p.max) (void)sys->recordsSince(static_cast<uint64_t>(p.since), 0, next);
    out.key("records");
    out.beginArray();
    for (const ecs::VfxRecord& r : records) {
        out.beginObject();
        out.key("seq"); out.uint64(r.seq);
        out.key("step"); out.uint64(r.spawnStep);
        out.key(ecs::vfxKindName(r.kind)); out.string(nameOf(set, r));
        out.key("at");
        // A decal's pose is local to what it is attached to; its place in the world is that body's
        // pose times it at the step (what an agent wants), else the pose as written.
        glm::vec3 at(r.pose[3]);
        if (r.on != entt::null && s.world.registry().valid(r.on))
            if (const auto* wt = s.world.registry().try_get<ecs::WorldTransform>(r.on)) at = glm::vec3(wt->world * r.pose[3]);
        writeVec3(out, at);
        if (r.on != entt::null && s.world.registry().valid(r.on)) {
            out.key("on");
            out.string(hex16(s.world.idOf(r.on)));
        }
        out.endObject();
    }
    out.endArray();
    out.key("next"); out.uint64(next);
    out.endObject();
    return true;
}

} // namespace ohao::api
