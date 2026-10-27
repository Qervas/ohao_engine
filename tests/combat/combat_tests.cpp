// Combat (sub-project 7): test 15, the weapon, health and damage, CPU only.
// docs/specs/2026-10-27-combat-design.md, "The combat step", "Components", "Events".

#include "ecs/asset_resolver.hpp"
#include "ecs/combat.hpp"
#include "ecs/components.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/snapshot.hpp"
#include "ecs/world.hpp"
#include "ecs/world_hash.hpp"
#include "level/level_system.hpp"
#include "reflect/registry.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "common/harness.hpp"   // CHECK, RUN, the JSON summary line

using namespace ohao::ecs;
using ohao::InputEvent;
using Kind = CombatEvent::Kind;

namespace {

const ohao::reflect::TypeRegistry& engineTypes() {
    static ohao::reflect::TypeRegistry r;
    static const bool once = (registerSceneTypes(r), true);
    (void)once;
    return r;
}

constexpr const char* kBlockout = "assets/levels/blockout_room.glb";
constexpr float kDt = 1.0f / 60.0f;

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool nearVec(const glm::vec3& a, const glm::vec3& b, float eps = 1e-3f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}
float angleDeg(glm::vec3 a, glm::vec3 b) {
    return glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0f, 1.0f)));
}
glm::quat yawRotation(float deg) { return glm::angleAxis(glm::radians(deg), glm::vec3(0.0f, 1.0f, 0.0f)); }

InputEvent actionEvent(const char* name, bool down) {
    InputEvent e;
    e.type = down ? InputEvent::Type::ActionDown : InputEvent::Type::ActionUp;
    e.name = name;
    return e;
}
InputEvent axisSet(const char* name, float value) {
    InputEvent e;
    e.type = InputEvent::Type::AxisSet;
    e.name = name;
    e.value = value;
    return e;
}

// The blockout with a player holding a rifle (a head with the camera), placed at `at` facing
// `yawDeg` and settled; level, physics and combat systems installed.
struct Arena {
    World world;
    FileAssetResolver files{OHAO_SOURCE_DIR};
    PhysicsScene* physics{nullptr};
    CombatSystem* combat{nullptr};
    entt::entity player{entt::null};
    entt::entity head{entt::null};

    explicit Arena(glm::vec3 at = {-2.0f, 0.0f, 2.0f}, float yawDeg = 0.0f, Weapon weapon = {}) {
        world.seedIds(7);
        auto& reg = world.registry();
        ohao::level::installLevels(world, files, engineTypes());
        physics = &installPhysics(world, files);
        combat = &installCombat(world);
        const entt::entity level = world.createWithId(0x1e7e1, "Level");
        reg.emplace<Level>(level, Level{AssetRef{kBlockout, {}}});
        player = world.createWithId(0xb1a7e2, "Player");
        reg.emplace<PlayerController>(player);
        reg.emplace<Health>(player);
        reg.emplace<Weapon>(player, weapon);
        head = world.createWithId(0xb1a7e3, "Head");
        reg.emplace<Camera>(head, Camera{75.0f, 0.05f, 100.0f, true});
        world.setParent(head, player);
        world.sync();
        for (auto [e, cam, ln] : reg.view<Camera, const LevelNode>().each()) cam.active = false;
        world.setTransform(player, Transform{at, yawRotation(yawDeg)});
        (void)run(10);   // settle onto the floor
    }

    // n world steps, the first with these input events (later steps keep actions held and axes
    // set); every combat event of those steps, in order.
    std::vector<CombatEvent> run(int n, std::vector<InputEvent> first = {}) {
        std::vector<CombatEvent> all;
        for (int i = 0; i < n; ++i) {
            world.stepInput(i == 0 ? first : std::vector<InputEvent>{});
            world.update(kDt);
            const auto ev = combatEvents(world);
            all.insert(all.end(), ev.begin(), ev.end());
        }
        return all;
    }
    // One round: fire pressed for one step, released the next (an automatic fires once).
    std::vector<CombatEvent> oneRound() {
        auto ev = run(1, {actionEvent("fire", true)});
        const auto rest = run(1, {actionEvent("fire", false)});
        ev.insert(ev.end(), rest.begin(), rest.end());
        return ev;
    }
    entt::entity named(std::string_view name) const {
        for (auto [e, n, ln] : world.registry().view<const Name, const LevelNode>().each())
            if (n.value == name) return e;
        return entt::null;
    }
    // A box collider centred at `centre` (half extents `half`), with Health if given.
    entt::entity box(uint64_t id, const char* name, glm::vec3 centre, glm::vec3 half, std::optional<float> health,
                     bool trigger = false) {
        const entt::entity e = world.createWithId(id, name);
        world.setTransform(e, Transform{centre});
        world.registry().emplace<Collider>(e, Collider{ColliderShape::Box, half, glm::vec3(0.0f), trigger, ""});
        if (health) world.registry().emplace<Health>(e, Health{*health, *health});
        return e;
    }
    const WeaponState& state() const { return world.registry().get<WeaponState>(player); }
    PlayerInfo info() const { return *physics->playerInfo(player); }
};

std::vector<CombatEvent> only(const std::vector<CombatEvent>& ev, Kind k) {
    std::vector<CombatEvent> out;
    for (const CombatEvent& e : ev)
        if (e.kind == k) out.push_back(e);
    return out;
}

uint64_t hashOf(const World& w) {
    Snapshot table;
    return hashWorld(w, engineTypes(), table);
}

bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }
bool sameBits(glm::vec3 a, glm::vec3 b) { return sameBits(a.x, b.x) && sameBits(a.y, b.y) && sameBits(a.z, b.z); }

// The same events bit for bit (`withStep`: the step and sequence numbers too).
bool sameEvents(const std::vector<CombatEvent>& a, const std::vector<CombatEvent>& b, bool withStep) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const CombatEvent &x = a[i], &y = b[i];
        if (x.kind != y.kind || x.sourceId != y.sourceId || x.targetId != y.targetId || !sameBits(x.point, y.point) ||
            !sameBits(x.normal, y.normal) || !sameBits(x.amount, y.amount) || !sameBits(x.health, y.health) ||
            x.surface != y.surface)
            return false;
        if (withStep && (x.step != y.step || x.seq != y.seq)) return false;
    }
    return true;
}

} // namespace

// ---- test 15: weapon, health, damage ------------------------------------------------------------

static void t15_damage_and_death() {
    Arena a;
    const entt::entity target = a.box(0x7a1, "Target", {-2.0f, 1.0f, -1.0f}, glm::vec3(0.3f), 100.0f);
    const uint64_t before = a.combat->nextEventSeq();
    std::vector<float> left;
    for (int i = 0; i < 4; ++i) left.push_back(a.combat->damage(target, 30.0f, a.player, {-2.0f, 1.0f, -0.7f}));
    CHECK(near(left[0], 70.0f) && near(left[1], 40.0f) && near(left[2], 10.0f) && near(left[3], 0.0f),
          "30 at a time: 70, 40, 10, then 0");
    CHECK(near(a.world.registry().get<Health>(target).current, 0.0f), "Health.current is 0");
    uint64_t dropped = 0;
    const auto ev = a.combat->eventsSince(before, 64, dropped);
    const auto damage = only(ev, Kind::Damage), death = only(ev, Kind::Death);
    CHECK(damage.size() == 4 && near(damage[3].amount, 10.0f) && near(damage[3].health, 0.0f),
          "four damage events; the last takes only the 10 left");
    CHECK(!damage.empty() && damage[0].sourceId == a.world.idOf(a.player) && damage[0].targetId == 0x7a1 &&
              nearVec(damage[0].point, {-2.0f, 1.0f, -0.7f}),
          "who, what, where");
    CHECK(death.size() == 1 && death[0].targetId == 0x7a1 && death[0].sourceId == a.world.idOf(a.player),
          "one death, the killer as its source");
    const uint64_t after = a.combat->nextEventSeq();
    CHECK(near(a.combat->damage(target, 30.0f, a.player, {}), 0.0f) && a.combat->nextEventSeq() == after,
          "the dead take no more damage and make no event");
    const entt::entity other = a.box(0x7a2, "Other", {2.0f, 1.0f, 1.0f}, glm::vec3(0.3f), 50.0f);
    CHECK(near(a.combat->damage(other, -5.0f, a.player, {}), 50.0f) &&
              near(a.combat->damage(other, std::nanf(""), a.player, {}), 50.0f) && a.combat->nextEventSeq() == after,
          "negative or NaN damage changes nothing");
}

static void t15_fire_rate_ten_hz() {
    Arena a;
    const uint64_t s0 = a.combat->steps();
    const auto ev = a.run(60, {actionEvent("fire", true)});
    const auto shots = only(ev, Kind::Shot);
    std::printf("\n      %zu shots, first at step +%llu", shots.size(),
                shots.empty() ? 0ull : static_cast<unsigned long long>(shots[0].step - s0));
    CHECK(shots.size() == 10, "fire held 60 steps at 10 Hz: 10 rounds");
    bool everySixth = !shots.empty() && shots[0].step == s0 + 1;
    for (std::size_t i = 1; i < shots.size(); ++i) everySixth &= shots[i].step - shots[i - 1].step == 6;
    CHECK(everySixth, "the first on the press's step, then every sixth step exactly");
    CHECK(a.state().ammo == 20, "ammo 30 - 10 = 20");
    CHECK(only(ev, Kind::Hit).size() == 10, "every round hits something (the north wall)");
}

static void t15_semi_automatic_one_per_press() {
    Weapon w;
    w.automatic = false;
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, w);
    CHECK(only(a.run(60, {actionEvent("fire", true)}), Kind::Shot).size() == 1, "held 60 steps: one round");
    std::size_t shots = 0;
    for (int i = 0; i < 3; ++i) {
        shots += only(a.run(10, {actionEvent("fire", false)}), Kind::Shot).size();
        shots += only(a.run(10, {actionEvent("fire", true)}), Kind::Shot).size();
    }
    CHECK(shots == 3, "three presses: three rounds");
    CHECK(a.state().ammo == 26, "ammo 26");
}

static void t15_magazine_and_reload() {
    Arena a;
    const auto ev = a.run(300, {actionEvent("fire", true)});
    const auto shots = only(ev, Kind::Shot), reload = only(ev, Kind::Reload), reloaded = only(ev, Kind::Reloaded);
    CHECK(reload.size() == 1 && reloaded.size() == 1, "one reload, started and finished");
    if (shots.size() < 31 || reload.size() != 1 || reloaded.size() != 1) {
        std::printf("\n      %zu shots, %zu reload, %zu reloaded", shots.size(), reload.size(), reloaded.size());
        CHECK(false, "a magazine and more");
        return;
    }
    CHECK(reload[0].step == shots[29].step, "the reload starts on the step the 30th round empties the magazine");
    CHECK(reloaded[0].step - reload[0].step == 90, "1.5 s: 90 steps");
    bool quiet = true;
    for (const CombatEvent& s : shots) quiet &= !(s.step > reload[0].step && s.step < reloaded[0].step);
    CHECK(quiet, "no round during the reload");
    CHECK(shots[30].step == reloaded[0].step, "firing resumes on the step the reload ends");
    CHECK(a.state().ammo == 30 - (shots.size() - 30), "the new magazine counts down");
}

static void t15_reload_key() {
    Arena a;
    (void)a.run(25, {actionEvent("fire", true)});
    (void)a.run(5, {actionEvent("fire", false)});
    CHECK(a.state().ammo == 25, "five rounds fired");
    auto ev = a.run(1, {actionEvent("reload", true)});
    ev = a.run(1, {actionEvent("reload", false)});
    CHECK(a.state().reloading > 0.0f, "R with a part-empty magazine reloads");
    ev = a.run(90);
    CHECK(only(ev, Kind::Reloaded).size() == 1 && a.state().ammo == 30, "and fills it");
    ev = a.run(1, {actionEvent("reload", true)});
    const auto more = a.run(1, {actionEvent("reload", false)});
    CHECK(only(ev, Kind::Reload).empty() && only(more, Kind::Reload).empty() && near(a.state().reloading, 0.0f),
          "R with a full magazine does nothing");
}

static void t15_hit_damages_target() {
    Arena a;
    const entt::entity target = a.box(0x7a1, "Target", {-2.0f, 1.62f, -1.0f}, glm::vec3(0.3f), 100.0f);
    const auto ev = a.oneRound();
    const auto shots = only(ev, Kind::Shot), hits = only(ev, Kind::Hit), damage = only(ev, Kind::Damage);
    CHECK(shots.size() == 1 && nearVec(shots[0].point, {-2.0f, 1.62f, 2.0f}, 0.01f) &&
              angleDeg(shots[0].normal, {0.0f, 0.0f, -1.0f}) <= 0.26f,
          "one round from the eye, within the spread of straight ahead");
    CHECK(hits.size() == 1 && hits[0].target == target && near(hits[0].point.z, -0.7f, 0.01f) &&
              hits[0].normal.z > 0.999f,
          "it hits the target's near face, the normal toward the player");
    CHECK(damage.size() == 1 && damage[0].target == target && near(damage[0].amount, 25.0f) &&
              near(damage[0].health, 75.0f),
          "25 damage, 75 left");
    CHECK(near(a.world.registry().get<Health>(target).current, 75.0f), "its Health says so");
}

static void t15_shot_ignores_shooter_and_triggers() {
    Arena a;
    const entt::entity target = a.box(0x7a1, "Target", {-2.0f, 1.62f, -1.0f}, glm::vec3(0.3f), 100.0f);
    (void)a.box(0x7a2, "Volume", {-2.0f, 1.62f, 0.5f}, {0.5f, 0.5f, 0.1f}, std::nullopt, true);
    (void)a.run(1);   // the volume's sensor is built
    const auto hits = only(a.oneRound(), Kind::Hit);
    CHECK(hits.size() == 1 && hits[0].target == target,
          "from inside its own capsule, through a trigger volume, the round hits the target");
    const auto r = a.physics->raycastFrom({-2.0f, 1.62f, 2.0f}, {0.0f, 0.0f, -1.0f}, 100.0f, a.player);
    CHECK(r && r->entity == target, "raycastFrom skips the shooter and triggers");
    const auto plain = a.physics->raycast({-2.0f, 1.62f, 2.0f}, {0.0f, 0.0f, -1.0f}, 100.0f);
    CHECK(plain && plain->entity == a.player, "a plain ray from the eye stops in the shooter's own capsule");
}

static void t15_hit_wall_reports_surface() {
    Arena a;
    const auto ev = a.oneRound();
    const auto hits = only(ev, Kind::Hit);
    CHECK(hits.size() == 1, "one hit");
    if (hits.size() != 1) return;
    const auto* name = a.world.registry().try_get<Name>(hits[0].target);
    const auto* col = a.world.registry().try_get<Collider>(hits[0].target);
    std::printf("\n      hit %s at (%.3f, %.3f, %.3f) surface %s", name ? name->value.c_str() : "?", hits[0].point.x,
                hits[0].point.y, hits[0].point.z, hits[0].surface.c_str());
    CHECK(name && name->value == "WallNorth" && near(hits[0].point.z, -3.0f, 0.01f) && hits[0].normal.z > 0.999f,
          "the north wall, at z -3, facing +Z");
    CHECK(col && hits[0].surface == (col->surface.empty() ? std::string("default") : col->surface),
          "with its Collider.surface");
    CHECK(near(hits[0].amount, 5.0f, 0.02f), "amount is the distance, 5 m");
    CHECK(only(ev, Kind::Damage).empty(), "a wall takes no damage");
}

static void t15_spread_cone_distribution() {
    const glm::vec3 aims[] = {{0.0f, 0.0f, -1.0f}, glm::normalize(glm::vec3(1.0f, 2.0f, -3.0f)), {0.0f, 1.0f, 0.0f}};
    for (const glm::vec3 aim : aims) {
        double sum = 0.0;
        float worst = 0.0f;
        for (uint32_t i = 0; i < 10000; ++i) {
            const float a = angleDeg(combat::spreadDirection(aim, 3.0f, 1u, i), aim);
            sum += a;
            worst = std::max(worst, a);
        }
        const double mean = sum / 10000.0;
        std::printf("\n      aim (%.2f, %.2f, %.2f): mean %.4f deg, max %.4f deg", aim.x, aim.y, aim.z, mean, worst);
        CHECK(worst <= 3.0f + 1e-3f, "every draw inside the 3 degree cone");
        CHECK(std::fabs(mean - 2.0) <= 0.06, "uniform over the disc: mean angle 2/3 of the cone (2 deg) +- 3 %");
    }
    CHECK(sameBits(combat::spreadDirection({0, 0, -1}, 3.0f, 9u, 4u), combat::spreadDirection({0, 0, -1}, 3.0f, 9u, 4u)),
          "a draw is a function of (aim, cone, seed, shot)");
    CHECK(combat::spreadDirection({0, 0, -1}, 0.0f, 9u, 4u) == glm::vec3(0, 0, -1), "a zero cone is the aim");
    float lo = 1.0f, hi = 0.0f;
    for (uint32_t i = 0; i < 1000; ++i) {
        const float u = combat::unitRandom(3u, i, 0u);
        lo = std::min(lo, u);
        hi = std::max(hi, u);
    }
    CHECK(lo >= 0.0f && hi < 1.0f && lo < 0.01f && hi > 0.99f, "unitRandom covers [0, 1)");
}

static void t15_spread_blooms_and_recovers() {
    Arena a;
    const auto shots = only(a.run(60, {actionEvent("fire", true)}), Kind::Shot);
    bool grows = shots.size() == 10;
    for (std::size_t k = 0; grows && k < shots.size(); ++k)
        grows &= near(shots[k].amount, 0.25f + std::min(0.35f * static_cast<float>(k), 3.0f), 1e-4f);
    std::printf("\n      cones %.3f %.3f %.3f ... %.3f", shots.size() > 0 ? shots[0].amount : 0.0f,
                shots.size() > 1 ? shots[1].amount : 0.0f, shots.size() > 2 ? shots[2].amount : 0.0f,
                shots.empty() ? 0.0f : shots.back().amount);
    CHECK(grows, "round k leaves within 0.25 + min(0.35 k, 3) degrees: bloom grows with sustained fire");
    CHECK(a.state().bloomDeg > 2.9f, "the bloom holds while firing");
    (void)a.run(60, {actionEvent("fire", false)});
    CHECK(near(a.state().bloomDeg, 0.0f, 1e-6f) && near(a.state().kickDeg, 0.0f, 1e-6f),
          "a second without firing: bloom and kick recovered");
}

static void t15_spread_is_seeded() {
    std::vector<glm::vec3> dirs[3];
    for (int i = 0; i < 3; ++i) {
        Weapon w;
        w.seed = i == 2 ? 2u : 1u;
        Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, w);
        for (const CombatEvent& s : only(a.run(60, {actionEvent("fire", true)}), Kind::Shot)) dirs[i].push_back(s.normal);
    }
    bool same = dirs[0].size() == 10 && dirs[1].size() == 10;
    for (std::size_t i = 0; same && i < dirs[0].size(); ++i) same &= sameBits(dirs[0][i], dirs[1][i]);
    CHECK(same, "the same seed: the same ten directions, bit for bit");
    bool differs = false;
    for (std::size_t i = 1; i < std::min(dirs[0].size(), dirs[2].size()); ++i) differs |= !sameBits(dirs[0][i], dirs[2][i]);
    CHECK(differs, "another seed: another pattern");
}

static void t15_recoil_kicks_and_recovers() {
    Arena a;
    const PlayerInfo p0 = a.info();
    (void)a.run(1, {actionEvent("fire", true)});
    const PlayerInfo p1 = a.info();
    std::printf("\n      pitch %.4f -> %.4f, yaw %.4f -> %.4f", p0.pitchDeg, p1.pitchDeg, p0.yawDeg, p1.yawDeg);
    CHECK(near(p1.pitchDeg - p0.pitchDeg, 0.35f, 1e-4f), "a round kicks the view up 0.35 degrees at once");
    const float yawKick = std::remainder(p1.yawDeg - p0.yawDeg, 360.0f);
    CHECK(std::fabs(yawKick) <= 0.15f + 1e-4f, "and sideways within 0.15 degrees");
    CHECK(near(a.state().kickDeg, 0.35f, 1e-4f), "the kick to recover is kept");
    const glm::quat headRot = a.world.registry().get<Transform>(a.head).rotation;
    const float headPitch = glm::degrees(std::asin(std::clamp((headRot * glm::vec3(0, 0, -1)).y, -1.0f, 1.0f)));
    CHECK(near(headPitch, p1.pitchDeg, 1e-3f), "the head shows it");
    CHECK(nearVec(a.info().pos, p0.pos, 1e-4f), "the player does not move");
    (void)a.run(60, {actionEvent("fire", false)});
    const PlayerInfo p2 = a.info();
    CHECK(near(p2.pitchDeg, p0.pitchDeg, 1e-4f), "a second later the vertical kick has come back down");
    CHECK(near(std::remainder(p2.yawDeg - p1.yawDeg, 360.0f), 0.0f, 1e-4f), "the sideways kick stays");
    CHECK(near(a.state().kickDeg, 0.0f, 1e-6f), "nothing left to recover");
}

static void t15_turret_shoots_player() {
    Arena a;
    auto& reg = a.world.registry();
    const entt::entity turret = a.world.createWithId(0x7e1, "Turret");
    a.world.setTransform(turret, Transform{{-2.0f, 1.0f, -1.5f}, yawRotation(180.0f)});   // its -Z is +Z
    Weapon w;
    w.damage = 10.0f;
    w.fireRate = 2.0f;
    w.spreadDeg = 0.0f;
    w.bloomPerShotDeg = 0.0f;
    reg.emplace<Weapon>(turret, w);
    reg.emplace<WeaponTrigger>(turret, WeaponTrigger{true, false});
    const auto ev = a.run(60);
    const auto shots = only(ev, Kind::Shot), hits = only(ev, Kind::Hit), damage = only(ev, Kind::Damage);
    CHECK(shots.size() == 2 && nearVec(shots[0].point, {-2.0f, 1.0f, -1.5f}) && nearVec(shots[0].normal, {0, 0, 1}),
          "a turret without a character fires from its origin along its -Z, at 2 Hz: two rounds in a second");
    CHECK(hits.size() == 2 && hits[0].target == a.player, "and hits the player");
    CHECK(damage.size() == 2 && damage[0].sourceId == 0x7e1 && near(reg.get<Health>(a.player).current, 80.0f),
          "the player takes 2 x 10");
    CHECK(near(a.info().pitchDeg, 0.0f, 1e-6f), "a turret's recoil turns no player");
}

static void t15_dead_do_not_fire() {
    Arena a;
    auto& reg = a.world.registry();
    Weapon w;
    w.damage = 25.0f;
    w.spreadDeg = 0.0f;
    // B first: id order, not creation order, decides who fires first.
    const entt::entity b = a.box(0x7e2, "TurretB", {-0.8f, 1.0f, -1.0f}, glm::vec3(0.2f), 25.0f);
    const entt::entity t = a.box(0x7e1, "TurretA", {-2.6f, 1.0f, -1.0f}, glm::vec3(0.2f), 25.0f);
    a.world.setTransform(b, Transform{{-0.8f, 1.0f, -1.0f}, yawRotation(90.0f)});    // facing -X
    a.world.setTransform(t, Transform{{-2.6f, 1.0f, -1.0f}, yawRotation(-90.0f)});   // facing +X
    for (const entt::entity e : {b, t}) {
        reg.emplace<Weapon>(e, w);
        reg.emplace<WeaponTrigger>(e, WeaponTrigger{true, false});
    }
    const auto ev = a.run(1);
    const auto shots = only(ev, Kind::Shot), death = only(ev, Kind::Death);
    CHECK(shots.size() == 1 && shots[0].sourceId == 0x7e1, "the lower id fires first: one round");
    CHECK(death.size() == 1 && death[0].target == b, "B dies before its turn");
    CHECK(near(reg.get<Health>(t).current, 25.0f), "and never fires back");
    const auto later = only(a.run(30), Kind::Shot);
    bool onlyA = later.size() == 5;   // 10 Hz: steps 7, 13, 19, 25, 31
    for (const CombatEvent& s : later) onlyA &= s.sourceId == 0x7e1;
    CHECK(onlyA, "A fires on at 10 Hz; B never again");
    reg.patch<Health>(a.player, [](Health& h) { h.current = 0.0f; });
    const auto mine = only(a.run(30, {actionEvent("fire", true)}), Kind::Shot);
    bool playerFired = false;
    for (const CombatEvent& s : mine) playerFired |= s.source == a.player;
    CHECK(!playerFired, "a dead player's trigger does nothing");
}

namespace {

// A fixed fight: fire, turn, look up, release, reload, fire again.
std::vector<CombatEvent> script(Arena& a) {
    std::vector<CombatEvent> all;
    const auto add = [&](std::vector<CombatEvent> ev) { all.insert(all.end(), ev.begin(), ev.end()); };
    add(a.run(20, {actionEvent("fire", true), axisSet("look_x", 2.0f)}));
    add(a.run(20, {axisSet("look_x", 0.0f), axisSet("look_y", -1.0f)}));
    add(a.run(5, {actionEvent("fire", false), axisSet("look_y", 0.0f)}));
    add(a.run(5, {actionEvent("reload", true)}));
    add(a.run(150, {actionEvent("reload", false), actionEvent("fire", true)}));   // fires again after the reload
    return all;
}

} // namespace

static void t15_runs_are_identical() {
    Arena a, b;
    // Ahead and to the right: the first rounds hit it, the turn sweeps the aim off it.
    (void)a.box(0x7a1, "Target", {-1.7f, 1.62f, -1.0f}, glm::vec3(0.4f), 1000.0f);
    (void)b.box(0x7a1, "Target", {-1.7f, 1.62f, -1.0f}, glm::vec3(0.4f), 1000.0f);
    const auto ea = script(a), eb = script(b);
    std::printf("\n      %zu events, %zu shots, %zu damage", ea.size(), only(ea, Kind::Shot).size(),
                only(ea, Kind::Damage).size());
    CHECK(!only(ea, Kind::Damage).empty(), "the fight hits the target");
    CHECK(sameEvents(ea, eb, true), "two runs: identical events, bit for bit");
    CHECK(hashOf(a.world) == hashOf(b.world), "and the same world.hash");
}

static void t15_snapshot_replays_a_burst() {
    for (const int at : {15, 190}) {   // mid-burst (cooldown, bloom, kick in flight); mid-reload
        Arena a;
        (void)a.box(0x7a1, "Target", {-2.0f, 1.62f, -1.0f}, glm::vec3(0.4f), 10000.0f);
        (void)a.run(at, {actionEvent("fire", true)});
        const Snapshot mid = takeSnapshot(a.world, engineTypes());
        const auto first = a.run(100);
        const uint64_t h1 = hashOf(a.world);
        restoreSnapshot(a.world, mid);
        const auto again = a.run(100);
        std::printf("\n      at %d: %zu events after the snapshot", at, first.size());
        CHECK(!only(first, Kind::Shot).empty() && sameEvents(first, again, false),
              "a restore replays the same rounds, hits and damage, bit for bit");
        CHECK(hashOf(a.world) == h1, "to the same world.hash");
    }
}

static void t15_play_scene_has_a_rifle() {
    World w;
    FileAssetResolver files(OHAO_SOURCE_DIR);
    ohao::level::installLevels(w, files, engineTypes());
    installPhysics(w, files);
    installCombat(w);
    const auto loaded = loadSceneFile(w, std::filesystem::path(OHAO_SOURCE_DIR) / "assets/scenes/blockout_play.scene.json",
                                      engineTypes());
    CHECK(loaded, "blockout_play.scene.json loads");
    if (!loaded) return;
    w.sync();
    auto& reg = w.registry();
    entt::entity player = entt::null;
    for (const entt::entity e : reg.view<PlayerController>()) player = e;
    const auto* health = player != entt::null ? reg.try_get<Health>(player) : nullptr;
    CHECK(health && near(health->max, 100.0f) && near(health->current, 100.0f), "the player has 100 health");
    CHECK(player != entt::null && reg.all_of<Weapon>(player), "and a rifle");
    bool fire = false, reload = false;
    for (auto [e, in] : reg.view<const InputActions>().each())
        for (const ActionBinding& b : in.actions) {
            fire |= b.action == "fire" && b.key == ohao::Key::MouseLeft;
            reload |= b.action == "reload" && b.key == ohao::Key::R;
        }
    CHECK(fire && reload, "fire on the left mouse button, reload on R");
    std::vector<CombatEvent> ev;
    for (int i = 0; i < 7; ++i) {
        w.stepInput(i == 0 ? std::vector<InputEvent>{actionEvent("fire", true)} : std::vector<InputEvent>{});
        w.update(kDt);
        const auto step = combatEvents(w);
        ev.insert(ev.end(), step.begin(), step.end());
    }
    CHECK(only(ev, Kind::Shot).size() == 2 && reg.get<WeaponState>(player).ammo == 28, "two rounds in 7 steps, 28 left");
}

// --dump-burst FILE: a 30-round burst at the north wall from (-2, 0, 2), as JSON (each round's step,
// cone, eye, direction, hit point; the view's pitch and yaw each step), for the look shots' plot.
static int dumpBurst(const char* path) {
    Arena a;
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return 2;
    std::fprintf(f, "{\"rounds\":[");
    const uint64_t s0 = a.combat->steps();
    bool first = true;
    std::vector<std::pair<float, float>> view;
    for (int i = 0; i < 180; ++i) {
        const auto ev = a.run(1, i == 0 ? std::vector<InputEvent>{actionEvent("fire", true)} : std::vector<InputEvent>{});
        const PlayerInfo p = a.info();
        view.emplace_back(p.pitchDeg, p.yawDeg);
        for (std::size_t k = 0; k < ev.size(); ++k) {
            if (ev[k].kind != Kind::Shot) continue;
            const CombatEvent& s = ev[k];
            const CombatEvent* h = k + 1 < ev.size() && ev[k + 1].kind == Kind::Hit ? &ev[k + 1] : nullptr;
            std::fprintf(f, "%s{\"step\":%llu,\"cone\":%.5f,\"eye\":[%.5f,%.5f,%.5f],\"dir\":[%.6f,%.6f,%.6f],\"hit\":", first ? "" : ",",
                         static_cast<unsigned long long>(s.step - s0), s.amount, s.point.x, s.point.y, s.point.z, s.normal.x,
                         s.normal.y, s.normal.z);
            if (h) std::fprintf(f, "[%.5f,%.5f,%.5f]}", h->point.x, h->point.y, h->point.z);
            else std::fprintf(f, "null}");
            first = false;
        }
    }
    std::fprintf(f, "],\"view\":[");
    for (std::size_t i = 0; i < view.size(); ++i) std::fprintf(f, "%s[%.5f,%.5f]", i ? "," : "", view[i].first, view[i].second);
    std::fprintf(f, "]}\n");
    std::fclose(f);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--dump-burst") return dumpBurst(argv[2]);
    std::printf("combat_tests\n");
    RUN(t15_damage_and_death);
    RUN(t15_fire_rate_ten_hz);
    RUN(t15_semi_automatic_one_per_press);
    RUN(t15_magazine_and_reload);
    RUN(t15_reload_key);
    RUN(t15_hit_damages_target);
    RUN(t15_shot_ignores_shooter_and_triggers);
    RUN(t15_hit_wall_reports_surface);
    RUN(t15_spread_cone_distribution);
    RUN(t15_spread_blooms_and_recovers);
    RUN(t15_spread_is_seeded);
    RUN(t15_recoil_kicks_and_recovers);
    RUN(t15_turret_shoots_player);
    RUN(t15_dead_do_not_fire);
    RUN(t15_runs_are_identical);
    RUN(t15_snapshot_replays_a_burst);
    RUN(t15_play_scene_has_a_rifle);
    return ohao::test::finish("combat_tests");
}
