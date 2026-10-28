// VFX (sub-project 12): test 31, the effect set, the closed form and the spawn records the step
// writes, CPU only. docs/specs/2026-10-28-vfx-design.md, "Data: the VFX set", "The step: spawn
// records", "Determinism", "Test plan" (31); the plan's Task 1.

#include "ecs/animation.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/audio_schedule.hpp"
#include "ecs/combat.hpp"
#include "ecs/components.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/snapshot.hpp"
#include "ecs/vfx.hpp"
#include "ecs/world.hpp"
#include "ecs/world_hash.hpp"
#include "level/level_system.hpp"
#include "reflect/registry.hpp"
#include "vfx/particle_math.hpp"
#include "vfx/tracer_math.hpp"
#include "vfx/vfx_set.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "common/harness.hpp"   // CHECK, RUN, the JSON summary line

using namespace ohao::ecs;
using ohao::InputEvent;
namespace vfx = ohao::vfx;

namespace {

const ohao::reflect::TypeRegistry& engineTypes() {
    static ohao::reflect::TypeRegistry r;
    static const bool once = (registerSceneTypes(r), true);
    (void)once;
    return r;
}

constexpr float kDt = 1.0f / 60.0f;
constexpr const char* kSlice = "assets/vfx/slice.vfx.json";

std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Whether parsing `json` fails with an error at `path`.
bool errorAt(const char* json, std::string_view path) {
    auto r = vfx::parseVfxSet(json);
    if (r) {
        std::printf("\n      parsed, expected an error at %.*s", static_cast<int>(path.size()), path.data());
        return false;
    }
    for (const auto& e : r.error())
        if (e.path == path) return true;
    std::printf("\n      no error at %.*s; got:", static_cast<int>(path.size()), path.data());
    for (const auto& e : r.error()) std::printf(" [%s: %s]", e.path.c_str(), e.message.c_str());
    return false;
}

InputEvent actionEvent(const char* name, bool down) {
    InputEvent e;
    e.type = down ? InputEvent::Type::ActionDown : InputEvent::Type::ActionUp;
    e.name = name;
    return e;
}

constexpr uint64_t kEnemyA = 0x5c0b1a7e00000010ull;

// The play scene as hosts load it (levels, physics, combat, the round hook, animation, the audio
// schedule, and the vfx system unless `withVfx` is false).
struct Fight {
    World w;
    FileAssetResolver files{OHAO_SOURCE_DIR};
    PhysicsScene* physics{nullptr};
    CombatSystem* combat{nullptr};
    VfxSystem* vfx{nullptr};
    entt::entity player{entt::null};
    bool loaded{false};

    explicit Fight(bool withVfx = true) {
        w.seedIds(7);
        ohao::level::installLevels(w, files, engineTypes());
        physics = &installPhysics(w, files);
        combat = &installCombat(w);
        installRoundHook(w, engineTypes());
        installAnimation(w, files);
        installAudio(w, files);
        if (withVfx) vfx = &installVfx(w, files);
        loaded = static_cast<bool>(
            loadSceneFile(w, std::filesystem::path(OHAO_SOURCE_DIR) / "assets/scenes/blockout_play.scene.json", engineTypes()));
        if (loaded)
            for (const char* sync : {"level", "player_spawn", "transform", "physics"}) w.runSync(sync);
        for (auto [e, pc] : w.registry().view<const PlayerController>().each())
            if (pc.input) player = e;
    }
    // n steps with `inputs(i)` at step i (counting from `from`); every record written in them, in order.
    std::vector<VfxRecord> run(int n, const std::function<std::vector<InputEvent>(int)>& inputs = {}, int from = 0,
                               std::vector<CombatEvent>* events = nullptr) {
        std::vector<VfxRecord> written;
        for (int i = 0; i < n; ++i) {
            w.stepInput(inputs ? inputs(from + i) : std::vector<InputEvent>{});
            w.update(kDt);
            if (events) {
                const auto ev = combatEvents(w);
                events->insert(events->end(), ev.begin(), ev.end());
            }
            if (!vfx) continue;
            for (const VfxRecord& r : vfx->records())
                if (r.spawnStep == vfx->steps()) written.push_back(r);
        }
        return written;
    }
    void face(uint64_t id) {
        const glm::vec3 me = physics->playerInfo(player)->pos, it = w.registry().get<Transform>(w.findById(id)).position;
        physics->teleportPlayer(player, me, glm::degrees(std::atan2(-(it.x - me.x), -(it.z - me.z))));
    }
};

// Bursts of fire every 40 steps (held 13); a grenade thrown at step 20.
std::vector<InputEvent> script(int i) {
    std::vector<InputEvent> in;
    if (i % 40 == 0) in.push_back(actionEvent("fire", true));
    if (i % 40 == 13) in.push_back(actionEvent("fire", false));
    if (i == 20) in.push_back(actionEvent("grenade", true));
    if (i == 21) in.push_back(actionEvent("grenade", false));
    return in;
}

// A record as text: every field that the same fight must repeat (not sourceSeq: the combat log's
// sequence is not part of a snapshot).
std::string describe(const VfxRecord& r) {
    char buf[320];
    const glm::vec3 p(r.pose[3]), z(r.pose[2]);
    std::snprintf(buf, sizeof buf, "%s %u seed %08x seq %llu step %llu at %.5f %.5f %.5f z %.4f %.4f %.4f a %.4f %.4f %.4f b %.4f %.4f %.4f life %.4f",
                  vfxKindName(r.kind), r.effect, r.seed, static_cast<unsigned long long>(r.seq),
                  static_cast<unsigned long long>(r.spawnStep), p.x, p.y, p.z, z.x, z.y, z.z, r.a.x, r.a.y, r.a.z, r.b.x,
                  r.b.y, r.b.z, r.life);
    return buf;
}

std::vector<std::string> lines(const std::vector<VfxRecord>& rs) {
    std::vector<std::string> out;
    for (const VfxRecord& r : rs) out.push_back(describe(r));
    return out;
}

void firstDiff(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const std::string x = i < a.size() ? a[i] : "(none)", y = i < b.size() ? b[i] : "(none)";
        if (x == y) continue;
        std::printf("\n      first difference at %zu of %zu/%zu:\n        %s\n        %s", i, a.size(), b.size(), x.c_str(),
                    y.c_str());
        return;
    }
}

// A set with one effect "puff" (one emitter, `life` s) and the given caps, for placed emitters.
vfx::VfxSet puffSet(float life, uint32_t recordsCap = 256) {
    vfx::VfxSet s;
    vfx::EffectDesc e;
    e.name = "puff";
    vfx::EmitterDesc em;
    em.count = 4;
    em.life = {life, life};
    e.emitters.push_back(em);
    s.effects.push_back(e);
    s.caps.records = recordsCap;
    return s;
}

// ---- the set ------------------------------------------------------------------------------------

void t31_set_parse() {
    const char* ok = R"({"effects": {"e": {"emitters": [{"count": 3, "life": [0.1, 0.2], "blend": "alpha"}]}},
                         "events": {"shot": {"effect": "e"}}})";
    CHECK(static_cast<bool>(vfx::parseVfxSet(ok)), "a small set parses");
    CHECK(errorAt(R"({"effects": {"e": {"emitters": [{"texture": "nope"}]}}})", "/effects/e/emitters/0/texture"),
          "an unknown texture, at its pointer");
    CHECK(errorAt(R"({"effects": {"e": {"emitters": [{}]}}, "events": {"shot": {"effect": "nope"}}})", "/events/shot/effect"),
          "an unknown effect");
    CHECK(errorAt(R"({"effects": {"e": {"emitters": [{}]}}, "events": {"hit": {"effect": "e", "decal": "nope"}}})",
                  "/events/hit/decal"),
          "an unknown decal");
    CHECK(errorAt(R"({"effects": {"e": {"emitters": [{"life": [2, 1]}]}}})", "/effects/e/emitters/0/life"),
          "a range with min > max");
    CHECK(errorAt(R"({"effects": {"impact_x": {"emitters": [{}]}}, "events": {"hit": {"effect": "impact_{surface}"}}})",
                  "/events/hit/fallback"),
          "a {surface} binding without a fallback");
    CHECK(errorAt(R"({"caps": {"records": 0}})", "/caps/records"), "a cap of 0");
    CHECK(errorAt(R"({"effects": {"e": {"emitters": [{"blend": "glow"}]}}})", "/effects/e/emitters/0/blend"),
          "an unknown blend");
    CHECK(errorAt(R"({"effects": {"e": {"emitters": [{}]}}, "speed": 1})", "/speed"), "an unknown key");
    const auto slice = vfx::parseVfxSet(readFile(std::filesystem::path(OHAO_SOURCE_DIR) / kSlice));
    if (!slice)
        for (const auto& e : slice.error()) std::printf("\n      slice: %s: %s", e.path.c_str(), e.message.c_str());
    CHECK(static_cast<bool>(slice), "the slice's set parses");
    if (slice) {
        const vfx::VfxSet& s = slice.value();
        bool all = true;
        for (const char* name : {"muzzle", "impact_concrete", "impact_metal", "impact_wood", "impact_default", "grenade_boom",
                                 "smoke_cloud"})
            if (s.effect(name) < 0) all = false, std::printf("\n      the slice lacks %s", name);
        CHECK(all, "the slice has the muzzle, the four impacts, the boom and the smoke");
        CHECK(s.tracer("rifle") >= 0 && s.decal("scorch") >= 0 && s.decal("bullet_default") >= 0,
              "a tracer, the scorch and the default bullet decal");
    }
}

// The default set is the old particle presets as data (the particle system's presets and combat_fx's
// dust, both deleted in step 2).
void t31_default_set() {
    const vfx::VfxSet s = vfx::defaultVfxSet();
    const int m = s.effect("muzzle");
    CHECK(m >= 0 && s.effects[m].emitters.size() == 1, "muzzle: one emitter");
    if (m >= 0) {
        const vfx::EmitterDesc& e = s.effects[m].emitters[0];
        CHECK(e.count == 16 && e.life.min == 0.05f && e.life.max == 0.15f && e.speed.min == 8.0f && e.speed.max == 15.0f,
              "muzzle: presetMuzzleFlash's count, life and speed");
        CHECK(std::fabs(e.spreadDeg - glm::degrees(0.3f)) < 1e-3f && e.sizeStart == 0.05f && e.sizeEnd == 0.01f &&
                  e.gravity == 0.0f && e.drag == 5.0f,
              "its spread (0.3 rad), sizes, gravity and drag");
        CHECK(e.color.keys.size() == 2 && e.color.keys[0].second == glm::vec3(1.0f, 0.8f, 0.3f) &&
                  e.color.keys[1].second == glm::vec3(1.0f, 0.4f, 0.1f) && e.alpha.keys.front().second == 1.0f &&
                  e.alpha.keys.back().second == 0.0f,
              "its colour and alpha over life");
    }
    bool impacts = true;
    for (const char* surface : {"concrete", "metal", "wood", "default"}) {
        const int i = s.effect(std::string("impact_") + surface);
        impacts &= i >= 0 && s.effects[i].emitters.size() == 2;
    }
    CHECK(impacts, "impact_concrete, _metal, _wood and _default: sparks and the surface's dust");
    if (const int i = s.effect("impact_metal"); i >= 0)
        CHECK(s.effects[i].emitters[1].count == 14 && s.effects[i].emitters[1].speed.min == 4.0f &&
                  s.effects[i].emitters[1].speed.max == 11.0f,
              "metal: dustFor's spark spray (14, 4-11 m/s)");
    if (const int i = s.effect("impact_wood"); i >= 0)
        CHECK(s.effects[i].emitters[1].gravity == 3.0f && s.effects[i].emitters[1].sizeEnd == 0.08f, "wood: heavier chips");
    CHECK(s.explode.effects == (std::vector<std::string>{"grenade_boom", "smoke_cloud"}), "explode: boom and smoke");
    CHECK(s.shot.tracer.empty() && s.tracers.empty(), "no tracers (today has none)");
    const int d = s.decal("mark");
    CHECK(d >= 0 && s.decals[d].size == 0.08f && s.decals[d].albedo == glm::vec3(0.035f, 0.032f, 0.03f) &&
              s.decals[d].roughness == 0.95f,
          "the impact marks' look as a decal");
    // A world without a VfxSet steps with it.
    World w;
    FileAssetResolver files{OHAO_SOURCE_DIR};
    VfxSystem& sys = installVfx(w, files);
    w.update(kDt);
    CHECK(sys.setPath().empty() && sys.set().effect("muzzle") == m, "without a VfxSet: the default set");
}

// The CPU closed form against an RK4 integration at 1 ms over 2 s, with and without drag.
void t31_closed_form() {
    for (const float drag : {0.0f, 4.0f}) {
        vfx::EmitterDesc em;
        em.life = {3.0f, 3.0f};
        em.speed = {6.0f, 6.0f};
        em.spreadDeg = 40.0f;
        em.gravity = 9.81f;
        em.drag = drag;
        glm::mat4 pose(1.0f);
        pose[3] = glm::vec4(1.0f, 2.0f, 3.0f, 1.0f);
        const vfx::ParticleState start = vfx::evaluate(em, 1234u, 5u, 0.0f, pose);
        glm::vec3 x = start.position, v = start.velocity0;
        const glm::vec3 G(0.0f, -em.gravity, 0.0f);
        const auto accel = [&](glm::vec3 vel) { return G - drag * vel; };
        const float h = 0.001f;
        float worst = 0.0f;
        for (int i = 1; i <= 2000; ++i) {
            const glm::vec3 k1v = accel(v), k1x = v;
            const glm::vec3 k2v = accel(v + 0.5f * h * k1v), k2x = v + 0.5f * h * k1v;
            const glm::vec3 k3v = accel(v + 0.5f * h * k2v), k3x = v + 0.5f * h * k2v;
            const glm::vec3 k4v = accel(v + h * k3v), k4x = v + h * k3v;
            x += h / 6.0f * (k1x + 2.0f * k2x + 2.0f * k3x + k4x);
            v += h / 6.0f * (k1v + 2.0f * k2v + 2.0f * k3v + k4v);
            if (i % 250 == 0) {
                const vfx::ParticleState p = vfx::evaluate(em, 1234u, 5u, static_cast<float>(i) * h, pose);
                worst = std::max(worst, glm::length(p.position - x));
            }
        }
        std::printf("\n      drag %.0f: worst %.6f m over 2 s", drag, worst);
        CHECK(worst < 0.001f, "within 1 mm of the integration");
    }
    // Counter-based draws: the same (seed, index) the same particle, another index another.
    vfx::EmitterDesc em;
    em.life = {0.5f, 1.5f};
    em.speed = {1.0f, 3.0f};
    em.spreadDeg = 30.0f;
    const glm::mat4 pose(1.0f);
    const auto a = vfx::evaluate(em, 7u, 3u, 0.25f, pose), b = vfx::evaluate(em, 7u, 3u, 0.25f, pose),
               c = vfx::evaluate(em, 7u, 4u, 0.25f, pose);
    CHECK(a.position == b.position && a.life == b.life, "the same draws");
    CHECK(a.position != c.position, "another index, another particle");
    CHECK(glm::dot(glm::normalize(a.velocity0), glm::vec3(0, 0, -1)) >= std::cos(glm::radians(30.0f)) - 1e-5f,
          "thrown within the cone about -Z");
}

// ---- the step -----------------------------------------------------------------------------------

// Records from a scripted fight: a muzzle per shot at the logical muzzle, tracers at the set's cadence,
// an impact effect and decal per hit by surface, the grenade's boom, smoke and scorch.
void t31_records_from_events() {
    Fight f;
    CHECK(f.loaded && f.vfx, "the play scene with the vfx system");
    if (!f.loaded || !f.vfx) return;
    f.w.registry().replace<Health>(f.player, Health{100000.0f, 100000.0f});
    f.face(kEnemyA);
    const auto& reg = f.w.registry();
    const vfx::VfxSet& set = f.vfx->set();
    const uint64_t playerId = f.w.idOf(f.player);
    int shots = 0, hits = 0, explodes = 0, tracers = 0, checked = 0;
    bool muzzlesOk = true, tracersOk = true, impactsOk = true, decalsOk = true, explodeOk = true;
    for (int i = 0; i < 240; ++i) {
        std::vector<CombatEvent> events;
        const auto written = f.run(1, script, i, &events);
        // The step's events after its last restart (none here).
        for (std::size_t k = 0; k < events.size(); ++k) {
            const CombatEvent& e = events[k];
            const auto find = [&](VfxRecord::Kind kind, int effect) {
                return std::count_if(written.begin(), written.end(), [&](const VfxRecord& r) {
                    return r.kind == kind && r.sourceSeq == e.seq && (effect < 0 || r.effect == effect);
                });
            };
            if (e.kind == CombatEvent::Kind::Shot) {
                ++shots;
                const glm::vec3 muzzle = e.point + glm::normalize(e.normal) * kMuzzleAhead;
                const auto it = std::find_if(written.begin(), written.end(), [&](const VfxRecord& r) {
                    return r.kind == VfxRecord::Kind::Effect && r.sourceSeq == e.seq;
                });
                muzzlesOk &= it != written.end() && it->effect == set.effect("muzzle") &&
                             glm::length(glm::vec3(it->pose[3]) - muzzle) < 1e-4f;
                const auto* ws = reg.try_get<WeaponState>(e.source);
                const bool own = e.sourceId == playerId;
                const vfx::TracerDesc& td = set.tracers[set.tracer("rifle")];
                const uint32_t every = own ? td.everyPlayer : td.everyEnemy;
                // The round's hit, if any, follows the shot in the same step.
                float distance = kTracerMiss;
                for (std::size_t h = k + 1; h < events.size(); ++h)
                    if (events[h].kind == CombatEvent::Kind::Hit && events[h].sourceId == e.sourceId) {
                        distance = events[h].amount;
                        break;
                    }
                const bool want = ws && every > 0 && ws->shots % every == 0 && distance >= td.minDistance;
                const long got = find(VfxRecord::Kind::Tracer, -1);
                tracersOk &= got == (want ? 1 : 0);
                tracers += static_cast<int>(got);
            } else if (e.kind == CombatEvent::Kind::Hit) {
                ++hits;
                const std::string surface = e.surface.empty() ? std::string("default") : e.surface;
                int impact = set.effect("impact_" + surface);
                if (impact < 0) impact = set.effect("impact_default");
                int decal = set.decal("bullet_" + surface);
                if (decal < 0) decal = set.decal("bullet_default");
                impactsOk &= find(VfxRecord::Kind::Effect, impact) == 1;
                // No decal on the input-driven player (or anything below it).
                bool onPlayer = false;
                for (entt::entity up = e.target; up != entt::null && reg.valid(up);) {
                    if (up == f.player) onPlayer = true;
                    const auto* p = reg.try_get<Parent>(up);
                    up = p ? p->entity : entt::null;
                }
                // A character is decaled only on a mesh box its round crosses; the play scene's enemies are
                // skinned (no box: ruling 18), so none is made there.
                const bool character = reg.try_get<PlayerController>(e.target) != nullptr;
                decalsOk &= find(VfxRecord::Kind::Decal, decal) == (onPlayer || character ? 0 : 1);
                ++checked;
            } else if (e.kind == CombatEvent::Kind::Explode) {
                ++explodes;
                explodeOk &= find(VfxRecord::Kind::Effect, set.effect("grenade_boom")) == 1 &&
                             find(VfxRecord::Kind::Effect, set.effect("smoke_cloud")) == 1 &&
                             find(VfxRecord::Kind::Decal, set.decal("scorch")) == 1;
                for (const VfxRecord& r : written)
                    if (r.sourceSeq == e.seq && r.kind == VfxRecord::Kind::Decal)
                        explodeOk &= glm::length(glm::vec3(r.pose[3]) - e.point) < 1e-4f &&
                                     glm::dot(glm::vec3(r.pose[2]), glm::vec3(0, -1, 0)) > 0.999f;
            }
        }
        for (const VfxRecord& r : written)
            if (r.spawnStep != f.vfx->steps()) muzzlesOk = false;
    }
    std::printf("\n      %d shots, %d hits (%d checked), %d tracers, %d explodes; %zu live", shots, hits, checked, tracers,
                explodes, f.vfx->records().size());
    CHECK(f.vfx->setPath() == kSlice, "the play scene names the slice's set");
    CHECK(shots > 10 && hits > 5 && explodes == 1, "the fight fired, hit and threw");
    CHECK(muzzlesOk, "every shot: one muzzle effect at the eye, 0.5 m along the round, in the event's step");
    CHECK(tracersOk && tracers > 0, "tracers at the cadence (enemy every round, the player's every third), none too near");
    CHECK(impactsOk, "every hit: its surface's impact effect (else impact_default)");
    CHECK(decalsOk, "every hit: its surface's decal, none on the player");
    CHECK(explodeOk, "the explode: boom, smoke and the scorch straight down at its point");
}

// A placed emitter: rate 10 writes exactly 10 records in 60 steps; rate 0 one when it becomes active.
void t31_placed_emitter() {
    World w;
    FileAssetResolver files{OHAO_SOURCE_DIR};
    VfxSystem& sys = installVfx(w, files);
    sys.setSet(puffSet(5.0f));
    const entt::entity a = w.createWithId(0xa1, "A");
    w.setTransform(a, Transform{{1.0f, 2.0f, 3.0f}});
    w.registry().emplace<VfxEmitter>(a, VfxEmitter{"puff", 10.0f, 9u, true});
    for (int i = 0; i < 60; ++i) w.update(kDt);
    CHECK(sys.records().size() == 10 && sys.stats().written == 10, "rate 10: exactly 10 records in 60 steps");
    bool atPose = true;
    for (const VfxRecord& r : sys.records()) atPose &= glm::length(glm::vec3(r.pose[3]) - glm::vec3(1, 2, 3)) < 1e-5f;
    CHECK(atPose, "at the entity's pose");
    std::vector<uint32_t> seeds;
    for (const VfxRecord& r : sys.records()) seeds.push_back(r.seed);
    std::sort(seeds.begin(), seeds.end());
    CHECK(std::adjacent_find(seeds.begin(), seeds.end()) == seeds.end(), "each with its own seed");

    const entt::entity b = w.createWithId(0xb2, "B");
    w.setTransform(b, Transform{{0.0f, 1.0f, 0.0f}});
    w.registry().emplace<VfxEmitter>(b, VfxEmitter{"puff", 0.0f, 3u, false});
    w.registry().patch<VfxEmitter>(a, [](VfxEmitter& e) { e.active = false; });
    for (int i = 0; i < 5; ++i) w.update(kDt);
    const uint64_t before = sys.stats().written;
    w.registry().patch<VfxEmitter>(b, [](VfxEmitter& e) { e.active = true; });
    for (int i = 0; i < 30; ++i) w.update(kDt);
    CHECK(sys.stats().written == before + 1, "rate 0: one record when it becomes active, none after");
}

// 300 records against a cap of 256: the oldest 44 dropped, counted, the same ones every run.
void t31_caps() {
    const auto once = [] {
        World w;
        FileAssetResolver files{OHAO_SOURCE_DIR};
        VfxSystem& sys = installVfx(w, files);
        sys.setSet(puffSet(60.0f));
        const entt::entity a = w.createWithId(0xa1, "A");
        w.setTransform(a, Transform{});
        w.registry().emplace<VfxEmitter>(a, VfxEmitter{"puff", 600.0f, 1u, true});   // 10 a step
        for (int i = 0; i < 30; ++i) w.update(kDt);
        std::vector<std::string> out;
        for (const VfxRecord& r : sys.records()) out.push_back(describe(r));
        const VfxStats st = sys.stats();
        std::printf("\n      written %llu, live %u, dropped %llu", static_cast<unsigned long long>(st.written),
                    st.live[0], static_cast<unsigned long long>(st.dropped[0]));
        const bool ok = st.written == 300 && st.live[0] == 256 && st.dropped[0] == 44 && sys.records().front().seq == 45;
        return std::pair{ok, out};
    };
    const auto [okA, a] = once();
    const auto [okB, b] = once();
    CHECK(okA, "256 live, the oldest 44 dropped and counted (the first live is the 45th)");
    CHECK(a == b, "the same records every run");
}

// The same fight twice gives the same records; a restore mid-fight writes what it wrote before; a
// round restart writes those of the round's first steps again.
void t31_replay() {
    Fight f1, f2;
    f1.face(kEnemyA);
    f2.face(kEnemyA);
    const auto a = lines(f1.run(300, script)), b = lines(f2.run(300, script));
    firstDiff(a, b);
    CHECK(!a.empty() && a == b, "the same fight twice: the same records");

    Fight f;
    f.w.registry().replace<Health>(f.player, Health{100000.0f, 100000.0f});
    f.face(kEnemyA);
    // After the grenade went off (step 170): a grenade in flight is combat's runtime state, never in a
    // snapshot (audio ruling 23), so a restore before its explode would not explode it again.
    (void)f.run(200, script);
    Snapshot snap;
    takeSnapshot(f.w, engineTypes(), snap);
    const auto live = lines(std::vector<VfxRecord>(f.vfx->records().begin(), f.vfx->records().end()));
    const auto x = lines(f.run(120, script, 200));
    restoreSnapshot(f.w, snap);
    const auto liveAfter = lines(std::vector<VfxRecord>(f.vfx->records().begin(), f.vfx->records().end()));
    const auto y = lines(f.run(120, script, 200));
    firstDiff(x, y);
    CHECK(liveAfter == live, "a restore gives back the records it had");
    CHECK(!x.empty() && x == y, "and the same records after it");

    // The round: 90 steps of fire from its start; a pause (fire released); a restart in a step that also
    // presses fire (the round's first step did), then the same inputs. The restart restores the round's
    // start inside the combat step, so the steps after it repeat steps 2-90 of the round: the same
    // records, step numbers and seeds included (their seq differ by the round's first step's records,
    // which the restart step's discarded shot does not write).
    Fight g;
    g.face(kEnemyA);
    std::vector<VfxRecord> first = g.run(90, script);
    std::erase_if(first, [](const VfxRecord& r) { return r.spawnStep == 1; });
    (void)g.run(30, [](int i) { return i == 0 ? std::vector<InputEvent>{actionEvent("fire", false)} : std::vector<InputEvent>{}; });
    (void)g.run(1, [](int) { return std::vector<InputEvent>{actionEvent("restart", true), actionEvent("fire", true)}; });
    const bool cleared = g.vfx->records().empty();
    const auto again = g.run(89, [](int i) {
        auto in = script(i + 1);
        if (i == 0) in.push_back(actionEvent("restart", false));
        return in;
    });
    const auto withoutSeq = [](const std::vector<VfxRecord>& rs) {
        std::vector<std::string> out;
        for (VfxRecord r : rs) {
            r.seq = 0;
            out.push_back(describe(r));
        }
        return out;
    };
    const auto la = withoutSeq(first), lb = withoutSeq(again);
    firstDiff(la, lb);
    std::printf("\n      %zu records in steps 2-90 of the round, %zu in the 89 steps after its restart", la.size(), lb.size());
    CHECK(cleared, "a restart leaves no record of the abandoned timeline");
    CHECK(!la.empty() && la == lb, "after the restart, the round's records again (steps and seeds included)");
}

// world.hash after a fight is the same with and without the vfx system.
void t31_hash_unchanged() {
    Fight with(true), without(false);
    with.face(kEnemyA);
    without.face(kEnemyA);
    (void)with.run(400, script);
    (void)without.run(400, script);
    Snapshot ta, tb;
    const uint64_t a = hashWorld(with.w, engineTypes(), ta), b = hashWorld(without.w, engineTypes(), tb);
    std::printf("\n      world.hash %016llx with vfx, %016llx without", static_cast<unsigned long long>(a),
                static_cast<unsigned long long>(b));
    CHECK(a == b, "the same world.hash");
}

} // namespace

// Decals (sub-project 12, test 33, the CPU half): the ring of 256 is the set's cap (the oldest reused,
// dropped and counted), the rest of the impact marks' rules move to the records: a character's decal
// on the mesh box its round crosses (and it stays on it after its death), none on the player, a
// restart clears them, and no entity is made (the marks' quads are gone).
std::vector<InputEvent> holdFire(int i) {
    std::vector<InputEvent> in;
    if (i == 0) in.push_back(actionEvent("fire", true));
    if (i % 200 == 199) in.push_back(actionEvent("reload", true));
    if (i % 200 == 0 && i > 0) in.push_back(actionEvent("reload", false));
    return in;
}

void t33_decal_ring() {
    Fight f;
    CHECK(f.loaded && f.vfx, "the play scene with the vfx system");
    if (!f.loaded || !f.vfx) return;
    const std::size_t entities = f.w.registry().storage<entt::entity>().size();
    // Nobody dies (a death's restart clears the ring): every combatant has health to spare.
    std::vector<entt::entity> combatants;
    for (auto [e, h] : f.w.registry().view<Health>().each()) combatants.push_back(e);
    for (const entt::entity e : combatants) f.w.registry().replace<Health>(e, Health{100000.0f, 100000.0f});
    const auto decal = static_cast<std::size_t>(VfxRecord::Kind::Decal);
    std::size_t written = 0;
    for (int i = 0; i < 4000; ++i) {
        const auto w = f.run(1, holdFire, i);
        for (const VfxRecord& r : w) written += r.kind == VfxRecord::Kind::Decal ? 1 : 0;
    }
    const VfxStats st = f.vfx->stats();
    std::printf("\n      %zu decals written, %u live, %llu dropped", written, st.live[decal],
                static_cast<unsigned long long>(st.dropped[decal]));
    CHECK(written > 256, "more hits than the ring holds");
    CHECK(st.live[decal] == 256 && st.dropped[decal] == written - 256,
          "256 live; the oldest dropped and counted (the set's cap)");
    CHECK(f.w.registry().storage<entt::entity>().size() == entities, "no entity made by the hits (the marks' quads are gone)");
}

void t33_decal_follows() {
    Fight f;
    CHECK(f.loaded && f.vfx, "the play scene with the vfx system");
    if (!f.loaded || !f.vfx) return;
    f.w.registry().replace<Health>(f.player, Health{100000.0f, 100000.0f});
    const entt::entity enemy = f.w.findById(kEnemyA);
    // The play scene's enemies are skinned: no box, no decal (ruling 18). Give EnemyA a body box in
    // front of its chest, as a mesh box child, so a round that crosses it has one to land on.
    const entt::entity body = f.w.createWithId(0xe1e2, "EnemyA Body");
    f.w.registry().emplace<MeshSource>(body, MeshSource{BoxPrim{{-0.25f, 0.0f, -0.17f}, {0.25f, 1.42f, 0.17f}, glm::vec3(0.6f)}});
    f.w.setParent(body, enemy);
    f.face(kEnemyA);
    const auto kind = VfxRecord::Kind::Decal;
    std::size_t onBody = 0, offBox = 0;
    std::vector<VfxRecord> placed;
    for (int i = 0; i < 400; ++i) {
        const auto w = f.run(1, script, i);
        for (const VfxRecord& r : w)
            if (r.kind == kind && r.on != entt::null) placed.push_back(r);
    }
    // A decal on the level's walls is on the level's own entity; only the enemy's parts count here.
    const auto partOf = [&](entt::entity e) {
        for (int depth = 0; depth < 64 && e != entt::null && f.w.registry().valid(e); ++depth) {
            if (e == enemy) return true;
            const auto* p = f.w.registry().try_get<Parent>(e);
            e = p ? p->entity : entt::null;
        }
        return false;
    };
    for (const VfxRecord& r : placed) {
        if (r.on == body) ++onBody;
        else if (partOf(r.on)) ++offBox;
    }
    std::printf("\n      %zu decals on the body box, %zu on other parts of EnemyA", onBody, offBox);
    CHECK(onBody > 0, "a round across EnemyA's body box leaves its decal on the box");
    CHECK(offBox == 0, "no decal on an enemy but on its box");
    // The decal's place is the box's: its pose in the box's frame is where the round hit it, on the face
    // toward the player (the box's -Z, out of the box, faces the player: the enemy faces it).
    bool onFace = !placed.empty();
    for (const VfxRecord& r : placed) {
        if (r.on != body) continue;
        const glm::vec3 local = glm::vec3(r.pose[3]);   // a decal's pose is local to what it is on
        onFace &= std::fabs(std::fabs(local.z) - 0.17f) < 1e-3f && std::fabs(local.x) <= 0.25f && local.y >= 0.0f &&
                  local.y <= 1.42f;
    }
    CHECK(onFace, "on a face of the body box");
    // Killed (its Transform stays upright; the death clip falls): the decal stays on the box.
    const std::size_t before = placed.size();
    f.combat->damage(enemy, 1000.0f, f.player, {});
    (void)f.run(5, {});
    std::size_t after = 0;
    for (const VfxRecord& r : f.vfx->records()) after += r.kind == kind && r.on == body ? 1 : 0;
    CHECK(after > 0 && before > 0, "the decals on the box survive its death");
}

void t33_no_decal_on_player() {
    Fight f;
    CHECK(f.loaded && f.vfx, "the play scene with the vfx system");
    if (!f.loaded || !f.vfx) return;
    std::vector<CombatEvent> events;
    std::vector<VfxRecord> written;
    f.face(kEnemyA);
    for (int i = 0; i < 600; ++i) {
        const auto w = f.run(1, script, i, &events);
        written.insert(written.end(), w.begin(), w.end());
    }
    std::size_t hitsOnPlayer = 0;
    for (const CombatEvent& e : events) hitsOnPlayer += e.kind == CombatEvent::Kind::Hit && e.target == f.player ? 1 : 0;
    std::size_t onPlayer = 0;
    for (const VfxRecord& r : written) onPlayer += r.kind == VfxRecord::Kind::Decal && r.on == f.player ? 1 : 0;
    std::printf("\n      %zu hits on the player; %zu decals on it", hitsOnPlayer, onPlayer);
    CHECK(hitsOnPlayer == 0 || onPlayer == 0, "a round into the player marks nothing on the player");
    CHECK(onPlayer == 0, "no decal record on the input-driven player");
}

void t33_decal_restart_clears() {
    Fight f;
    CHECK(f.loaded && f.vfx, "the play scene with the vfx system");
    if (!f.loaded || !f.vfx) return;
    for (int i = 0; i < 120; ++i) (void)f.run(1, holdFire, i);
    const auto decal = static_cast<std::size_t>(VfxRecord::Kind::Decal);
    const uint32_t before = f.vfx->stats().live[decal];
    (void)f.run(1, [](int s) {
        std::vector<InputEvent> in;
        if (s == 0) in.push_back(actionEvent("restart", true));
        return in;
    });
    CHECK(before > 0, "decals before the restart");
    CHECK(f.vfx->stats().live[decal] == 0, "a restart clears every decal");
}

// Tracers (sub-project 12, test 33, the timing): the head at speed * age along the round, clamped at
// the hit; the tail `length` behind the head, clamped at it; nothing before the round is fired; a
// streak of the set's width, facing the eye (across the round).
void t33_tracer_timing() {
    const glm::vec3 from(0.0f, 1.5f, 0.0f), to(0.0f, 1.5f, -20.0f);   // a 20 m round
    const glm::vec3 eye(3.0f, 1.5f, 1.0f), dir(0.0f, 0.0f, -1.0f);
    const float speed = 420.0f, length = 1.8f, width = 0.012f;
    bool headOk = true, tailOk = true, faceOk = true, widthOk = true, endOk = true;
    for (int k = 1; k <= 120; ++k) {   // 0.0167 s to 2 s
        const float age = static_cast<float>(k) / 60.0f;
        const vfx::TracerQuad q = vfx::tracerQuad(from, to, speed, length, width, age, eye);
        const float head = std::min(speed * age, 20.0f), tail = std::max(0.0f, head - length);
        if (!q.visible) {
            headOk = false;
            continue;
        }
        headOk &= std::fabs(q.head - head) < 1e-4f;
        tailOk &= std::fabs(q.tail - tail) < 1e-4f;
        const glm::vec3 p0 = q.corners[0].pos, p1 = q.corners[2].pos;   // the tail's and the head's corners
        headOk &= std::fabs(glm::dot(p1 - from, dir) - head) < 1e-3f;
        tailOk &= std::fabs(glm::dot(p0 - from, dir) - tail) < 1e-3f;
        const glm::vec3 across = q.corners[1].pos - q.corners[0].pos;
        widthOk &= std::fabs(glm::length(across) - width) < 1e-5f;
        faceOk &= std::fabs(glm::dot(glm::normalize(across), dir)) < 1e-4f &&
                  std::fabs(glm::dot(glm::normalize(across), glm::normalize(eye - q.centre))) < 1e-4f;
        if (age >= 20.0f / speed) endOk &= std::fabs(q.head - 20.0f) < 1e-4f;   // clamped at the hit
    }
    CHECK(!vfx::tracerQuad(from, to, speed, length, width, 0.0f, eye).visible, "nothing before the round is fired");
    CHECK(headOk, "the head at speed * age along the round, clamped at the hit");
    CHECK(tailOk, "the tail length behind the head, clamped at it");
    CHECK(widthOk && faceOk, "the set's width, across the round and facing the eye");
    CHECK(endOk, "past the hit the head stays at the hit");
}

int main() {
    std::printf("vfx_tests\n");
    RUN(t31_set_parse);
    RUN(t31_default_set);
    RUN(t31_closed_form);
    RUN(t31_records_from_events);
    RUN(t31_placed_emitter);
    RUN(t31_caps);
    RUN(t31_replay);
    RUN(t31_hash_unchanged);
    RUN(t33_decal_ring);
    RUN(t33_decal_follows);
    RUN(t33_no_decal_on_player);
    RUN(t33_decal_restart_clears);
    RUN(t33_tracer_timing);
    return ohao::test::finish("vfx_tests");
}
