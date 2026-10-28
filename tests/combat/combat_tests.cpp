// Combat (sub-project 7): test 15, the weapon, health and damage, CPU only.
// docs/specs/2026-10-27-combat-design.md, "The combat step", "Components", "Events".

#include "ecs/animation.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/combat.hpp"
#include "ecs/combat_hud.hpp"
#include "ecs/components.hpp"
#include "ecs/impact_marks.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/snapshot.hpp"
#include "ecs/view_model.hpp"
#include "ecs/world.hpp"
#include "ecs/world_hash.hpp"
#include "level/level_system.hpp"
#include "reflect/registry.hpp"
#include "render/scene/light_table.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
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
    AnimationSystem* anim{nullptr};
    entt::entity player{entt::null};
    entt::entity head{entt::null};

    // `extra` runs before the first sync (what it adds spawns with the player).
    explicit Arena(glm::vec3 at = {-2.0f, 0.0f, 2.0f}, float yawDeg = 0.0f, Weapon weapon = {},
                   const std::function<void(World&)>& extra = {}) {
        world.seedIds(7);
        auto& reg = world.registry();
        ohao::level::installLevels(world, files, engineTypes());
        physics = &installPhysics(world, files);
        combat = &installCombat(world);
        anim = &installAnimation(world, files);
        const entt::entity level = world.createWithId(0x1e7e1, "Level");
        reg.emplace<Level>(level, Level{AssetRef{kBlockout, {}}});
        player = world.createWithId(0xb1a7e2, "Player");
        reg.emplace<PlayerController>(player);
        reg.emplace<Health>(player);
        reg.emplace<Weapon>(player, weapon);
        head = world.createWithId(0xb1a7e3, "Head");
        reg.emplace<Camera>(head, Camera{75.0f, 0.05f, 100.0f, true});
        world.setParent(head, player);
        if (extra) extra(world);
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

// ---- test 16: enemies -------------------------------------------------------------------------
// docs/specs/2026-10-27-combat-design.md, "Enemies": characters on the player's code (spawn
// "enemy", input false) driven by the enemy AI at the start of each combat step.

namespace {

constexpr uint64_t kEnemyA = 0xe1, kEnemyB = 0xe2;
// Behind both enemies (they face +Z from z -2): outside their cones, in the clear.
const glm::vec3 kHidden{-0.8f, 0.0f, -2.6f};

Weapon enemyWeapon() {
    Weapon w;
    w.damage = 8.0f;
    w.fireRate = 4.0f;
    w.magazine = 12;
    w.reloadTime = 2.0f;
    w.recoilPitchDeg = w.recoilYawDeg = 0.0f;
    w.seed = 7;
    return w;
}

// Two enemies, EnemyA and EnemyB, spawning at the blockout's enemy spawns.
std::function<void(World&)> enemies(Enemy ai = {}) {
    return [ai](World& w) {
        for (const auto& [id, name] : {std::pair{kEnemyA, "EnemyA"}, std::pair{kEnemyB, "EnemyB"}}) {
            const entt::entity e = w.createWithId(id, name);
            PlayerController pc;
            pc.spawn = "enemy";
            pc.input = false;
            pc.walkSpeed = 2.5f;
            w.registry().emplace<PlayerController>(e, pc);
            w.registry().emplace<Health>(e);
            w.registry().emplace<Weapon>(e, enemyWeapon());
            w.registry().emplace<Enemy>(e, ai);
        }
    };
}

entt::entity byId(const Arena& a, uint64_t id) { return a.world.findById(id); }
EnemyMode modeOf(const Arena& a, uint64_t id) {
    const auto* s = a.world.registry().try_get<EnemyState>(byId(a, id));
    return s ? s->mode : EnemyMode::Idle;
}
glm::vec3 feetOf(const Arena& a, uint64_t id) { return a.world.registry().get<Transform>(byId(a, id)).position; }
std::vector<CombatEvent> from(const std::vector<CombatEvent>& ev, Kind k, uint64_t source) {
    std::vector<CombatEvent> out;
    for (const CombatEvent& e : ev)
        if (e.kind == k && e.sourceId == source) out.push_back(e);
    return out;
}
void toughen(Arena& a) { a.world.registry().replace<Health>(a.player, Health{10000.0f, 10000.0f}); }

// Sub-project 10: the same enemies with the enemy graph's Animator (no mesh: the step needs none).
constexpr const char* kEnemyGraph = "assets/anim/enemy.animgraph.json";
constexpr std::size_t kBody = 0, kFire = 2, kReload = 3, kHit = 4;   // the graph's layers (1: aim)

std::function<void(World&)> animatedEnemies(Enemy ai = {}, Weapon weapon = enemyWeapon()) {
    return [ai, weapon](World& w) {
        enemies(ai)(w);
        for (const uint64_t id : {kEnemyA, kEnemyB}) {
            const entt::entity e = w.findById(id);
            w.registry().replace<Weapon>(e, weapon);
            w.registry().emplace<Animator>(e, Animator{AssetRef{kEnemyGraph, {}}, {}});
        }
    };
}
const AnimLayerState* layerOf(const World& w, entt::entity e, std::size_t layer) {
    const auto* an = w.registry().try_get<Animator>(e);
    return an && layer < an->layers.size() ? &an->layers[layer] : nullptr;
}
std::string stateOf(const World& w, entt::entity e, std::size_t layer) {
    const auto* l = layerOf(w, e, layer);
    return l ? l->state : std::string("<none>");
}
uint32_t stepsIn(const World& w, entt::entity e, std::size_t layer) {
    const auto* l = layerOf(w, e, layer);
    return l ? l->steps : 0u;
}
float paramOf(const World& w, entt::entity e, std::string_view name) {
    if (const auto* p = w.registry().try_get<AnimParams>(e))
        for (const auto& [n, v] : p->values)
            if (n == name) return v;
    return -999.0f;
}
std::vector<AnimEvent> animEventsOf(const World& w, uint64_t id, std::string_view name = {}) {
    std::vector<AnimEvent> out;
    uint64_t dropped = 0;
    if (const AnimationSystem* s = animationSystem(w))
        for (const AnimEvent& e : s->eventsSince(0, AnimationSystem::kLogCapacity, dropped))
            if (e.entity == id && (name.empty() || e.name == name)) out.push_back(e);
    return out;
}
bool upright(const World& w, entt::entity e) {
    return (w.registry().get<Transform>(e).rotation * glm::vec3(0.0f, 1.0f, 0.0f)).y > 0.9999f;
}

} // namespace

static void t16_enemies_spawn_at_enemy_spawns() {
    Arena a(kHidden, 0.0f, {}, enemies());
    for (const auto& [id, x, z] : {std::tuple{kEnemyA, -2.5f, -2.0f}, std::tuple{kEnemyB, 3.0f, -2.0f}}) {
        const auto info = a.physics->playerInfo(byId(a, id));
        std::printf("\n      %llx at (%.3f, %.3f, %.3f) yaw %.2f", static_cast<unsigned long long>(id),
                    info ? info->pos.x : 0.0f, info ? info->pos.y : 0.0f, info ? info->pos.z : 0.0f,
                    info ? info->yawDeg : 0.0f);
        CHECK(info && near(info->pos.x, x, 0.01f) && near(info->pos.z, z, 0.01f),
              "at its enemy spawn (B's is on the ramp: it settles a millimetre down it)");
        CHECK(info && std::cos(glm::radians(info->yawDeg)) < -0.9999f, "facing the spawn's yaw (180: +Z)");
    }
    CHECK(a.physics->playerInfo(a.player) && near(a.info().pos.x, kHidden.x, 1e-3f), "the player is not moved by them");
}

static void t16_idle_without_sight() {
    Arena a(kHidden, 0.0f, {}, enemies());
    const glm::vec3 a0 = feetOf(a, kEnemyA), b0 = feetOf(a, kEnemyB);
    const auto ev = a.run(120);
    CHECK(modeOf(a, kEnemyA) == EnemyMode::Idle && modeOf(a, kEnemyB) == EnemyMode::Idle,
          "the player behind them: both stay idle");
    CHECK(only(ev, Kind::Shot).empty(), "and fire nothing");
    CHECK(nearVec(feetOf(a, kEnemyA), a0, 1e-3f) && nearVec(feetOf(a, kEnemyB), b0, 1e-3f), "or move");
}

static void t16_sees_and_chases() {
    Enemy ai;
    ai.attackRange = 2.0f;   // so it walks rather than fires
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, {}, enemies(ai));
    toughen(a);
    const float d0 = glm::length(glm::vec2(feetOf(a, kEnemyA).x - a.info().pos.x, feetOf(a, kEnemyA).z - a.info().pos.z));
    (void)a.run(30);
    const float d1 = glm::length(glm::vec2(feetOf(a, kEnemyA).x - a.info().pos.x, feetOf(a, kEnemyA).z - a.info().pos.z));
    std::printf("\n      EnemyA %.3f m from the player, then %.3f", d0, d1);
    CHECK(modeOf(a, kEnemyA) == EnemyMode::Chase, "in sight, out of attack range: it chases");
    CHECK(d0 - d1 > 0.8f, "and has walked most of a metre closer in half a second (2.5 m/s)");
    (void)a.run(60);
    CHECK(modeOf(a, kEnemyA) == EnemyMode::Attack, "in range: it stops and attacks");
}

static void t16_attacks_in_range_after_reaction_time() {
    Arena a(kHidden, 0.0f, {}, enemies());   // unseen while it settles,
    toughen(a);
    a.physics->teleportPlayer(a.player, {-2.0f, 0.0f, 2.0f}, 0.0f);   // then in front of them
    const glm::vec3 a0 = feetOf(a, kEnemyA);
    uint64_t attackAt = 0, firstShot = 0;
    for (int i = 0; i < 90 && !firstShot; ++i) {
        const auto ev = a.run(1);
        if (!attackAt && modeOf(a, kEnemyA) == EnemyMode::Attack) attackAt = a.combat->steps();
        if (const auto s = from(ev, Kind::Shot, kEnemyA); !s.empty()) firstShot = s[0].step;
    }
    std::printf("\n      attack at step %llu, first round at %llu", static_cast<unsigned long long>(attackAt),
                static_cast<unsigned long long>(firstShot));
    CHECK(attackAt != 0, "in sight and in range: it attacks");
    CHECK(firstShot && firstShot - attackAt >= 20 && firstShot - attackAt <= 22,
          "its first round after the reaction time (0.35 s: 21 steps)");
    CHECK(glm::length(glm::vec2(feetOf(a, kEnemyA).x - a0.x, feetOf(a, kEnemyA).z - a0.z)) < 0.05f,
          "standing where it stood");
}

static void t16_bursts_and_pauses() {
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, {}, enemies());
    toughen(a);
    const auto shots = from(a.run(300), Kind::Shot, kEnemyA);
    std::printf("\n      %zu rounds:", shots.size());
    for (std::size_t i = 1; i < shots.size(); ++i)
        std::printf(" %llu", static_cast<unsigned long long>(shots[i].step - shots[i - 1].step));
    bool shape = shots.size() >= 9;
    for (std::size_t i = 1; shape && i < 9; ++i) {
        const uint64_t gap = shots[i].step - shots[i - 1].step;
        shape &= i % 3 == 0 ? gap >= 48 && gap <= 50 : gap == 15;
    }
    CHECK(shape, "bursts of 3 at 4 Hz (15 steps apart), 0.8 s between bursts");
}

static void t16_loses_player_returns_to_idle() {
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, {}, enemies());
    toughen(a);
    for (int i = 0; i < 10 && modeOf(a, kEnemyA) != EnemyMode::Attack; ++i) (void)a.run(1);
    CHECK(modeOf(a, kEnemyA) == EnemyMode::Attack, "it attacks");
    (void)a.box(0x3a11, "Wall", {-2.25f, 1.5f, 0.0f}, {1.5f, 1.5f, 0.1f}, std::nullopt);   // between them
    const auto ev = a.run(3);
    CHECK(modeOf(a, kEnemyA) == EnemyMode::Chase, "the line blocked: it chases where it last saw the player");
    int steps = 0;
    std::vector<CombatEvent> later;
    while (modeOf(a, kEnemyA) != EnemyMode::Idle && steps < 400) {
        const auto e = a.run(1);
        later.insert(later.end(), e.begin(), e.end());
        ++steps;
    }
    std::printf("\n      idle %d steps after the wall", steps + 3);
    CHECK(modeOf(a, kEnemyA) == EnemyMode::Idle && steps + 3 >= 180 && steps + 3 <= 200,
          "and after 3 s without sight gives up: idle");
    CHECK(from(later, Kind::Shot, kEnemyA).empty(), "firing nothing it cannot see");
}

static void t16_hit_wakes_idle_enemy() {
    Arena a(kHidden, 0.0f, {}, enemies());
    toughen(a);
    (void)a.run(5);
    CHECK(modeOf(a, kEnemyA) == EnemyMode::Idle, "idle, the player behind it");
    a.combat->damage(byId(a, kEnemyA), 10.0f, a.player, feetOf(a, kEnemyA));
    (void)a.run(2);
    CHECK(modeOf(a, kEnemyA) == EnemyMode::Attack, "hit: it turns on the player (in range, in the clear): attack");
    CHECK(modeOf(a, kEnemyB) == EnemyMode::Idle, "the other one did not notice");
}

static void t16_dead_enemy_blocks_nothing() {
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, {}, animatedEnemies());
    toughen(a);
    (void)a.run(5);
    const entt::entity e = byId(a, kEnemyA);
    const glm::vec3 chest = feetOf(a, kEnemyA) + glm::vec3(0.0f, 1.2f, 0.0f);
    a.combat->damage(e, 1000.0f, a.player, chest);
    const auto ev = a.run(60);
    auto& reg = a.world.registry();
    CHECK(modeOf(a, kEnemyA) == EnemyMode::Dead, "dead");
    CHECK(!reg.all_of<PlayerController>(e) && !a.physics->playerInfo(e), "its character is gone");
    // Sub-project 10, OPEN ruling 7 (provisional): the death clip falls; the saved Transform stays upright.
    CHECK(stateOf(a.world, e, kBody) == "dead", "its death clip played to its end (the held `dead` state)");
    CHECK(upright(a.world, e), "its Transform upright where it stood");
    const glm::vec3 eye = a.info().pos + glm::vec3(0.0f, 1.62f, 0.0f);
    const auto r = a.physics->raycastFrom(eye, chest - eye, 50.0f, a.player);
    CHECK(r && r->entity != e, "a ray through where it stood passes");
    CHECK(from(ev, Kind::Shot, kEnemyA).empty(), "and it fires no more");
}

static void t16_enemy_fire_kills_player() {
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, {}, enemies());
    a.world.registry().replace<Health>(a.player, Health{100.0f, 30.0f});
    const auto ev = a.run(900);
    const auto deaths = only(ev, Kind::Death);
    const bool killed = !deaths.empty() && deaths[0].target == a.player &&
                        (deaths[0].sourceId == kEnemyA || deaths[0].sourceId == kEnemyB);
    std::printf("\n      %zu enemy rounds, %zu hits on the player", from(ev, Kind::Shot, kEnemyA).size() +
                from(ev, Kind::Shot, kEnemyB).size(), only(ev, Kind::Damage).size());
    CHECK(killed, "their rounds kill the player");
    CHECK(near(a.world.registry().get<Health>(a.player).current, 0.0f), "Health 0");
}

namespace {

std::vector<CombatEvent> fight(Arena& a) {
    std::vector<CombatEvent> all;
    const auto add = [&](std::vector<CombatEvent> ev) { all.insert(all.end(), ev.begin(), ev.end()); };
    add(a.run(40, {actionEvent("fire", true), axisSet("look_x", 3.0f)}));
    add(a.run(60, {axisSet("look_x", -2.0f), axisSet("move_right", 1.0f)}));
    add(a.run(200, {axisSet("look_x", 0.0f), axisSet("move_right", 0.0f)}));
    return all;
}

} // namespace

static void t16_runs_are_identical() {
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, {}, enemies()), b({-2.0f, 0.0f, 2.0f}, 0.0f, {}, enemies());
    toughen(a);
    toughen(b);
    const auto ea = fight(a), eb = fight(b);
    std::printf("\n      %zu events, %zu damage", ea.size(), only(ea, Kind::Damage).size());
    CHECK(!from(ea, Kind::Shot, kEnemyA).empty(), "the enemies fight back");
    CHECK(sameEvents(ea, eb, true), "two runs: identical events, bit for bit");
    CHECK(hashOf(a.world) == hashOf(b.world), "and world.hash");
}

static void t16_snapshot_replays_a_fight() {
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, {}, enemies());
    toughen(a);
    (void)a.run(60, {actionEvent("fire", true)});
    const Snapshot mid = takeSnapshot(a.world, engineTypes());
    const auto first = a.run(150);
    const uint64_t h1 = hashOf(a.world);
    restoreSnapshot(a.world, mid);
    const auto again = a.run(150);
    CHECK(!from(first, Kind::Shot, kEnemyB).empty() && sameEvents(first, again, false),
          "a restore mid-fight replays the enemies' and the player's rounds bit for bit");
    CHECK(hashOf(a.world) == h1, "to the same world.hash");
}

static void t16_play_scene_has_two_enemies() {
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
    std::size_t enemiesFound = 0, atSpawn = 0, withBody = 0;
    for (auto [e, en, pc] : reg.view<const Enemy, const PlayerController>().each()) {
        ++enemiesFound;
        const glm::vec3 p = reg.get<Transform>(e).position;
        atSpawn += (near(p.x, -2.5f, 1e-3f) || near(p.x, 3.0f, 1e-3f)) && near(p.z, -2.0f, 1e-3f) && !pc.input &&
                   pc.spawn == "enemy";
        if (const auto* ch = reg.try_get<Children>(e))
            for (const entt::entity c : ch->entities) withBody += reg.all_of<MeshSource>(c) ? 1 : 0;
    }
    CHECK(enemiesFound == 2 && atSpawn == 2, "two enemies, at the enemy spawns, moved by the AI");
    CHECK(withBody >= 2, "each with a body to see");
    std::size_t players = 0;
    for (auto [e, pc] : reg.view<const PlayerController>().each()) players += pc.input ? 1 : 0;
    CHECK(players == 1, "one player");
}

// ---- test 17: death, restart, win ----------------------------------------------------------------
// docs/specs/2026-10-27-combat-design.md, "Death, restart, win": a world with a Round snapshots
// itself at the end of its first combat step (through the host's RoundHook); the player dead for
// restartDelay, or `restart` pressed, restores it at the end of that step; every Enemy dead wins.

namespace {

// Gives the arena a Round now: the next step is the round's first (its snapshot is the world
// after that step). Returns that world's hash.
uint64_t startRound(Arena& a, Round round = {}, std::size_t* starts = nullptr) {
    installRoundHook(a.world, engineTypes());
    const entt::entity r = a.world.createWithId(0x40a1d, "Round");
    a.world.registry().emplace<Round>(r, round);
    const auto ev = a.run(1);
    if (starts) *starts = only(ev, Kind::RoundStart).size();
    return hashOf(a.world);
}

} // namespace

static void t17_round_start_event() {
    Arena a;
    std::size_t starts = 0;
    (void)startRound(a, {}, &starts);
    CHECK(starts == 1, "the round starts on the first step with a Round");
    CHECK(a.combat->round().started && a.combat->round().restarts == 0, "round() says it started, no restarts");
    CHECK(only(a.run(30), Kind::RoundStart).empty(), "and only once");
}

static void t17_restart_equals_round_start() {
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, {}, enemies());
    const uint64_t h0 = startRound(a);
    auto& reg = a.world.registry();
    const glm::vec3 p0 = a.info().pos;
    const entt::entity crate = a.named("CrateA");
    const glm::vec3 crate0 = reg.get<Transform>(crate).position;
    // A fight: the player fires and walks, a crate is knocked into the air, the enemies shoot.
    (void)a.run(40, {actionEvent("fire", true), axisSet("move_right", 1.0f)});
    Transform lifted = reg.get<Transform>(crate);
    lifted.position.y += 1.0f;
    a.world.setTransform(crate, lifted);
    (void)a.run(10, {actionEvent("fire", false), axisSet("move_right", 0.0f)});
    CHECK(hashOf(a.world) != h0, "the fight changed the world");
    // The player dies; 3 s later the round restarts.
    a.combat->damage(a.player, 10000.0f, byId(a, kEnemyA), a.info().pos);
    std::vector<CombatEvent> ev;
    int steps = 0;
    while (only(ev, Kind::Restart).empty() && steps < 400) {
        const auto e = a.run(1);
        ev.insert(ev.end(), e.begin(), e.end());
        ++steps;
    }
    std::printf("\n      restart %d steps after the death", steps);
    CHECK(only(ev, Kind::Restart).size() == 1 && steps >= 179 && steps <= 181, "restartDelay (3 s, 180 steps) after the death");
    CHECK(hashOf(a.world) == h0, "the world after the restart step is the round's start: world.hash");
    CHECK(nearVec(a.info().pos, p0, 1e-6f), "the player back where it stood");
    CHECK(near(reg.get<Health>(a.player).current, 100.0f) && reg.get<WeaponState>(a.player).ammo == 30,
          "with full health and a full magazine");
    CHECK(nearVec(reg.get<Transform>(crate).position, crate0, 1e-6f), "the crate back in its place");
    CHECK(near(reg.get<Health>(byId(a, kEnemyA)).current, 100.0f) && modeOf(a, kEnemyA) != EnemyMode::Dead,
          "the enemies back to their start");
    CHECK(a.combat->round().restarts == 1, "round() counts the restart");
}

static void t17_restart_key() {
    Arena a;
    Round round;
    const uint64_t h0 = startRound(a, round);
    (void)a.run(20, {actionEvent("fire", true)});
    const auto ev = a.run(1, {actionEvent("fire", false), actionEvent("restart", true)});
    CHECK(only(ev, Kind::Restart).size() == 1, "restart pressed: the round restarts in that step");
    CHECK(hashOf(a.world) == h0, "to the round's start");
}

static void t17_dead_player_cannot_move_or_fire() {
    Arena a;
    const glm::vec3 p0 = a.info().pos;
    a.world.registry().patch<Health>(a.player, [](Health& h) { h.current = 0.0f; });
    const auto ev = a.run(30, {axisSet("move_forward", 1.0f), axisSet("look_x", 5.0f), actionEvent("fire", true)});
    CHECK(nearVec(a.info().pos, p0, 1e-4f), "a dead player does not walk");
    CHECK(near(a.info().yawDeg, 0.0f, 1e-4f), "or turn");
    CHECK(only(ev, Kind::Shot).empty(), "or fire");
}

static void t17_win_when_all_enemies_dead() {
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, {}, enemies());
    toughen(a);
    const uint64_t h0 = startRound(a);
    a.combat->damage(byId(a, kEnemyA), 1000.0f, a.player, {});
    auto ev = a.run(5);
    CHECK(only(ev, Kind::Win).empty() && a.combat->round().phase == RoundPhase::Playing, "one enemy left: no win");
    a.combat->damage(byId(a, kEnemyB), 1000.0f, a.player, {});
    ev = a.run(240);
    CHECK(only(ev, Kind::Win).size() == 1 && a.combat->round().phase == RoundPhase::Won, "both dead: won, once");
    CHECK(only(ev, Kind::Restart).empty(), "a won round waits for `restart`");
    ev = a.run(1, {actionEvent("restart", true)});
    CHECK(only(ev, Kind::Restart).size() == 1 && modeOf(a, kEnemyA) != EnemyMode::Dead &&
              a.world.registry().all_of<PlayerController>(byId(a, kEnemyB)),
          "restart: both enemies back, characters and all");
    CHECK(hashOf(a.world) == h0, "the world after the restart step is the round's start: world.hash");
    CHECK(a.combat->round().phase == RoundPhase::Playing, "playing again");
}

static void t17_no_round_no_restart() {
    Arena a;
    a.combat->damage(a.player, 1000.0f, entt::null, {});
    const auto ev = a.run(300, {actionEvent("restart", true)});
    CHECK(only(ev, Kind::Restart).empty() && only(ev, Kind::RoundStart).empty(), "no Round: no round, no restart");
    CHECK(near(a.world.registry().get<Health>(a.player).current, 0.0f), "the player stays dead");
}

static void t17_restart_replays_identically() {
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, {}, enemies());
    toughen(a);
    (void)startRound(a);
    const auto play = [&] {
        std::vector<CombatEvent> all;
        const auto add = [&](std::vector<CombatEvent> e) { all.insert(all.end(), e.begin(), e.end()); };
        add(a.run(30, {actionEvent("fire", true), axisSet("look_x", 2.0f)}));
        add(a.run(60, {axisSet("look_x", 0.0f), axisSet("move_forward", -1.0f)}));
        add(a.run(30, {actionEvent("fire", false), axisSet("move_forward", 0.0f)}));
        return all;
    };
    const auto first = play();
    const uint64_t h1 = hashOf(a.world);
    (void)a.run(1, {actionEvent("restart", true)});   // restarts on the press; held, it does nothing more
    const auto again = play();
    std::printf("\n      %zu events in the round's first 120 steps", first.size());
    CHECK(!first.empty() && sameEvents(first, again, false), "after a restart the same play gives the same fight, bit for bit");
    CHECK(hashOf(a.world) == h1, "and the same world.hash");
}

// A won round's restart brings the dead enemies' characters back (the ones they had, parked while
// they lay dead, where the round's start has them, not at their spawns), so the snapshot's
// simulation restores and the same play gives the same fight.
static void t17_restart_after_win_replays_identically() {
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, {}, enemies());
    toughen(a);
    (void)startRound(a);
    const auto play = [&] {
        std::vector<CombatEvent> all;
        const auto add = [&](std::vector<CombatEvent> e) { all.insert(all.end(), e.begin(), e.end()); };
        add(a.run(30, {actionEvent("fire", true), axisSet("look_x", 2.0f)}));
        add(a.run(60, {axisSet("look_x", 0.0f), axisSet("move_forward", -1.0f)}));
        add(a.run(30, {actionEvent("fire", false), axisSet("move_forward", 0.0f)}));
        return all;
    };
    const auto first = play();
    const uint64_t h1 = hashOf(a.world);
    a.combat->damage(byId(a, kEnemyA), 1000.0f, a.player, {});
    a.combat->damage(byId(a, kEnemyB), 1000.0f, a.player, {});
    CHECK(!only(a.run(30), Kind::Win).empty(), "both dead: won");
    (void)a.run(1, {actionEvent("restart", true)});
    const auto again = play();
    CHECK(!first.empty() && sameEvents(first, again, false), "after the won round's restart the same play gives the same fight");
    CHECK(hashOf(a.world) == h1, "and the same world.hash");
}

// The play scene's enemies stand at their spawns' yaw, exactly 180 degrees. A restart that brings
// them back from the dead keeps the rotation the snapshot restored, bit for bit: the yaw read back
// from it is +180 where the round start held -180 (the same rotation, another quaternion), and
// world.hash after the restart step must be the round start's (found by step 5's api tests).
static void t17_restart_after_kills_keeps_restored_rotations() {
    World w;
    w.seedIds(7);
    FileAssetResolver files(OHAO_SOURCE_DIR);
    ohao::level::installLevels(w, files, engineTypes());
    installPhysics(w, files);
    CombatSystem& combat = installCombat(w);
    installRoundHook(w, engineTypes());
    const auto loaded = loadSceneFile(w, std::filesystem::path(OHAO_SOURCE_DIR) / "assets/scenes/blockout_play.scene.json",
                                      engineTypes());
    CHECK(loaded, "blockout_play.scene.json loads");
    if (!loaded) return;
    for (const char* sync : {"level", "player_spawn", "transform", "physics"}) w.runSync(sync);   // as scene.load does
    const auto step = [&](int n, std::vector<InputEvent> first = {}) {
        for (int i = 0; i < n; ++i) {
            w.stepInput(i == 0 ? first : std::vector<InputEvent>{});
            w.update(kDt);
        }
    };
    step(1);
    const uint64_t h0 = hashOf(w);   // the round's start
    for (const uint64_t id : {0x5c0b1a7e00000010ull, 0x5c0b1a7e00000020ull})
        (void)combat.damage(w.findById(id), 1000.0f, entt::null, {});
    step(2);
    CHECK(combat.round().phase == RoundPhase::Won, "both enemies dead: won");
    step(1, {actionEvent("restart", true)});
    CHECK(combat.round().restarts == 1, "restart: the round restarted");
    CHECK(hashOf(w) == h0, "the world after the restart step is the round's start: world.hash, rotations bit for bit");
}

// ---- step 4: presentation (no numbered test in the spec; fx_*) ------------------------------------
// docs/specs/2026-10-27-combat-design.md, "Presentation (step 4)": the muzzle flash, impact marks,
// the HUD's content. The GPU side (the hud pass) is in engine_tests, sounds and the capturing click
// in runtime_tests.

namespace {

// A muzzle light under the player's head (off), as the play scene has it.
std::function<void(World&)> withMuzzle(std::function<void(World&)> more = {}) {
    return [more](World& w) {
        const entt::entity head = w.findById(0xb1a7e3);
        const entt::entity m = w.createWithId(0xb1a7e4, "Muzzle");
        w.setTransform(m, Transform{{0.18f, -0.12f, -0.6f}});
        Light l;
        l.intensity = 0.0f;
        l.range = 6.0f;
        l.radius = 0.03f;
        l.castShadows = false;
        w.registry().emplace<Light>(m, l);
        w.registry().emplace<Muzzle>(m, Muzzle{6.0f, 0.05f});
        w.setParent(m, head);
        if (more) more(w);
    };
}

// The texts a HUD list draws: glyphs on one line (same top), left to right, a space where a glyph
// cell is skipped.
std::vector<std::string> texts(const ohao::render::HudList& list) {
    std::map<float, std::vector<const ohao::render::HudItem*>> lines;
    for (const auto& it : list.items)
        if (it.kind == static_cast<uint32_t>(ohao::render::HudKind::Glyph)) lines[it.rect.y].push_back(&it);
    std::vector<std::string> out;
    for (auto& [y, glyphs] : lines) {
        std::sort(glyphs.begin(), glyphs.end(), [](auto* a, auto* b) { return a->rect.x < b->rect.x; });
        std::string s;
        for (std::size_t i = 0; i < glyphs.size(); ++i) {
            const float w = glyphs[i]->rect.z - glyphs[i]->rect.x;
            if (i > 0 && glyphs[i]->rect.x - glyphs[i - 1]->rect.x > w * 1.2f * 1.5f) s.push_back(' ');
            const uint32_t g = glyphs[i]->glyph;
            s.push_back(g < 10 ? static_cast<char>('0' + g) : g < 36 ? static_cast<char>('A' + g - 10) : '/');
        }
        out.push_back(s);
    }
    return out;
}
bool hasText(const ohao::render::HudList& list, std::string_view want) {
    for (const auto& t : texts(list))
        if (t.find(want) != std::string::npos) return true;
    return false;
}
std::size_t countKind(const ohao::render::HudList& list, ohao::render::HudKind k) {
    std::size_t n = 0;
    for (const auto& it : list.items) n += it.kind == static_cast<uint32_t>(k) ? 1 : 0;
    return n;
}

Weapon steadyRifle() {   // no spread, bloom or recoil: every round where the view looks
    Weapon w;
    w.spreadDeg = 0.0f;
    w.bloomPerShotDeg = 0.0f;
    w.recoilPitchDeg = w.recoilYawDeg = 0.0f;
    return w;
}

} // namespace

static void fx_muzzle_flash_on_for_flash_time() {
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, {}, withMuzzle());
    auto& reg = a.world.registry();
    const entt::entity m = a.world.findById(0xb1a7e4);
    CHECK(reg.get<Light>(m).intensity == 0.0f, "the muzzle light is off before a round");
    std::vector<float> seen;
    for (int i = 0; i < 6; ++i) {
        (void)a.run(1, i == 0 ? std::vector<InputEvent>{actionEvent("fire", true)}
                              : i == 1 ? std::vector<InputEvent>{actionEvent("fire", false)} : std::vector<InputEvent>{});
        seen.push_back(reg.get<Light>(m).intensity);
    }
    std::printf("\n      intensity by step: %.1f %.1f %.1f %.1f %.1f %.1f", seen[0], seen[1], seen[2], seen[3], seen[4], seen[5]);
    CHECK(seen[0] == 6.0f && seen[1] == 6.0f && seen[2] == 6.0f, "on at its intensity for 0.05 s: the round's step and two more");
    CHECK(seen[3] == 0.0f && seen[4] == 0.0f && seen[5] == 0.0f, "then off");
    CHECK(reg.get<WeaponState>(a.player).flash == 0.0f, "WeaponState.flash back to 0");
    // A light at 0 has no emitter mesh; on, it has one: the flash.
    ohao::render::LightDesc d;
    d.kind = ohao::render::LightKind::Point;
    d.radius = 0.03f;
    d.intensity = 0.0f;
    CHECK(!ohao::render::lightEmitter(d, std::nullopt).has_value(), "a light at 0 draws no emitter mesh");
    d.intensity = 6.0f;
    CHECK(ohao::render::lightEmitter(d, std::nullopt).has_value(), "on, it does");
}

static void fx_impact_pool_caps_and_reuses_oldest() {
    World w;
    ImpactMarks marks;
    for (int i = 0; i < 70; ++i)
        marks.add(w, entt::null, glm::vec3(static_cast<float>(i), 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    const auto pool = marks.marks();
    CHECK(pool.size() == ImpactMarks::kCapacity && marks.made() == 70, "64 marks at most; 70 made");
    CHECK(pool[0].serial == 65 && pool[5].serial == 70 && pool[6].serial == 7 && pool[63].serial == 64,
          "the 65th..70th took the oldest slots (0..5); the rest are the 7th..64th");
    const glm::mat4& m = pool[5].world;
    CHECK(nearVec(glm::vec3(m[3]), {69.0f, 1.0f, ImpactMarks::kLift}, 1e-5f), "at the point, 2 mm off along the normal");
    CHECK(nearVec(glm::normalize(glm::vec3(m[2])), {0.0f, 0.0f, 1.0f}, 1e-5f), "facing the normal (its +Z)");
    CHECK(near(glm::length(glm::vec3(m[0])), ImpactMarks::kSize, 1e-5f), "8 cm across");
    marks.clear();
    CHECK(marks.marks().empty(), "clear empties the pool");
}

static void fx_marks_from_hits_never_saved_or_hashed() {
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, steadyRifle());
    ImpactMarks marks;
    marks.observe(a.world);   // starts at the log's end
    const uint64_t h0 = hashOf(a.world);
    const std::size_t entities = a.world.registry().view<EntityId>().size();
    const auto ev = a.oneRound();
    marks.observe(a.world);
    marks.follow(a.world);
    const auto hits = only(ev, Kind::Hit);
    CHECK(hits.size() == 1 && marks.marks().size() == 1, "one round, one hit, one mark");
    if (hits.size() == 1 && marks.marks().size() == 1) {
        const glm::vec3 at(marks.marks()[0].world[3]);
        CHECK(nearVec(at, hits[0].point + hits[0].normal * ImpactMarks::kLift, 1e-4f), "where the round hit the wall");
    }
    CHECK(a.world.registry().view<EntityId>().size() == entities, "no entity made");
    // Same world with no marks observing: the same hash.
    Arena b({-2.0f, 0.0f, 2.0f}, 0.0f, steadyRifle());
    (void)b.oneRound();
    CHECK(hashOf(a.world) == hashOf(b.world) && hashOf(a.world) != h0, "the marks are not in world.hash");
}

static void fx_marks_on_an_enemy_land_on_its_body_and_follow_it() {
    // EnemyA straight ahead (it spawns at (-2.5, -2) facing +Z), with the play scene's body and head boxes.
    const auto bodies = [](World& w) {
        enemies()(w);
        const entt::entity e = w.findById(kEnemyA);
        const entt::entity head = w.createWithId(0xe1e1, "EnemyA Head");
        w.registry().emplace<MeshSource>(head, MeshSource{BoxPrim{{-0.14f, 1.46f, -0.14f}, {0.14f, 1.78f, 0.14f}, glm::vec3(0.8f)}});
        w.setParent(head, e);
        const entt::entity body = w.createWithId(0xe1e2, "EnemyA Body");
        w.registry().emplace<MeshSource>(body, MeshSource{BoxPrim{{-0.25f, 0.0f, -0.17f}, {0.25f, 1.42f, 0.17f}, glm::vec3(0.6f)}});
        w.setParent(body, e);
    };
    Arena a({-2.5f, 0.0f, 2.0f}, 0.0f, steadyRifle(), bodies);
    toughen(a);
    ImpactMarks marks;
    marks.observe(a.world);
    const auto ev = a.oneRound();
    marks.observe(a.world);
    marks.follow(a.world);
    const auto hits = only(ev, Kind::Hit);
    const entt::entity enemy = byId(a, kEnemyA);
    CHECK(hits.size() == 1 && hits[0].targetId == kEnemyA, "the round hit EnemyA's capsule");
    CHECK(marks.marks().size() == 1 && marks.marks()[0].on == enemy, "a mark, attached to EnemyA");
    if (marks.marks().size() != 1) return;
    // In the enemy's frame (its boxes are its children's, at its origin): the eye's height (1.62 above
    // the player's feet, the enemy standing on the 0.3 m step) is the body box's, so the mark is on the
    // body's face toward the player (the enemy faces the player: its -Z), 2 mm off it, not on the capsule.
    const auto inEnemy = [&] {
        const glm::mat4 toEnemy = glm::inverse(a.world.registry().get<WorldTransform>(enemy).world);
        return glm::vec3(toEnemy * marks.marks()[0].world[3]);
    };
    const glm::vec3 p = inEnemy();
    std::printf("\n      the mark in the enemy's frame: (%.4f, %.4f, %.4f)", p.x, p.y, p.z);
    CHECK(near(std::fabs(p.z), 0.17f + ImpactMarks::kLift, 1e-3f) && std::fabs(p.x) <= 0.25f && p.y >= 0.0f &&
              p.y <= 1.42f,
          "on a face of the body box, 2 mm off it (not on the capsule, 0.3 m round)");
    const glm::vec3 facing = glm::normalize(glm::vec3(marks.marks()[0].world[2]));
    const glm::vec3 toPlayer = a.info().pos - glm::vec3(a.world.registry().get<WorldTransform>(enemy).world[3]);
    CHECK(glm::dot(facing, toPlayer) > 0.0f, "the face toward the player, the mark facing out of it");
    // Killed (its Transform stays upright: the death clip falls, sub-project 10 ruling 7): the mark
    // stays where it is on the body.
    a.combat->damage(enemy, 1000.0f, a.player, {});
    (void)a.run(5);
    marks.follow(a.world);
    CHECK(nearVec(inEnemy(), p, 1e-4f), "the mark stays with the enemy after its death");
}

static void fx_no_marks_on_the_player() {
    // An enemy's rounds hit the player: no mark hangs in front of the player's eyes.
    Arena a({-2.5f, 0.0f, 2.0f}, 0.0f, Weapon{}, enemies());
    toughen(a);
    ImpactMarks marks;
    marks.observe(a.world);
    const auto ev = a.run(120);
    marks.observe(a.world);
    std::size_t onPlayer = 0;
    for (const CombatEvent& e : only(ev, Kind::Hit)) onPlayer += e.target == a.player ? 1 : 0;
    std::printf("\n      %zu enemy rounds hit the player", onPlayer);
    CHECK(onPlayer > 0, "the enemies hit the player");
    for (const ImpactMark& m : marks.marks()) CHECK(m.on != a.player, "no mark on the player");
}

static void fx_marks_cleared_on_restart() {
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, steadyRifle());
    (void)startRound(a);
    ImpactMarks marks;
    marks.observe(a.world);
    (void)a.oneRound();
    (void)a.run(10);   // past the 10 Hz cooldown
    (void)a.oneRound();
    marks.observe(a.world);
    CHECK(marks.marks().size() == 2, "two rounds, two marks");
    (void)a.run(1, {actionEvent("restart", true)});
    marks.observe(a.world);
    CHECK(marks.marks().empty(), "the restart clears them");
}

static void fx_hud_shows_health_ammo_and_a_crosshair() {
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, Weapon{});
    CombatHud hud;
    hud.observe(a.world);
    HudState s = hud.state(a.world, 1920, 1080);
    CHECK(s.player && s.armed && s.health == 100.0f && s.maxHealth == 100.0f, "the player's health");
    CHECK(s.ammo == 30 && s.magazine == 30 && !s.reloading, "a full magazine");
    CHECK(!s.hitMarker && s.hurt == 0.0f && s.phase == RoundPhase::Playing && !s.round, "nothing else");
    const float fov = 75.0f;
    const auto gapFor = [&](float cone) {
        return 6.0f + 540.0f * std::tan(glm::radians(cone)) / std::tan(glm::radians(fov * 0.5f));
    };
    CHECK(near(s.coneDeg, 0.25f, 1e-5f) && near(s.crosshairGap, gapFor(0.25f), 0.01f),
          "the crosshair opens with the cone at the camera's field of view (6 px + the cone's radius on screen)");
    ohao::render::HudList list = CombatHud::layout(s, 1920, 1080);
    CHECK(hasText(list, "100") && hasText(list, "30/30"), "health and ammo as text");
    CHECK(countKind(list, ohao::render::HudKind::Rect) >= 6, "a health bar and four crosshair arms (and more)");
    // Ten rounds: ammo 20, the cone bloomed.
    (void)a.run(60, {actionEvent("fire", true)});
    (void)a.run(1, {actionEvent("fire", false)});
    hud.observe(a.world);
    s = hud.state(a.world, 1920, 1080);
    CHECK(s.ammo == 20 && hasText(CombatHud::layout(s, 1920, 1080), "20/30"), "the magazine goes down: 20/30");
    // Empty it: RELOADING.
    (void)a.run(120, {actionEvent("fire", true)});
    hud.observe(a.world);
    s = hud.state(a.world, 1920, 1080);
    CHECK(s.reloading && hasText(CombatHud::layout(s, 1920, 1080), "RELOADING"), "the reload is shown");
    // The bloom shows: fire a burst from rest and compare.
    Arena b({-2.0f, 0.0f, 2.0f}, 0.0f, Weapon{});
    CombatHud hb;
    (void)b.run(13, {actionEvent("fire", true)});   // three rounds
    hb.observe(b.world);
    const HudState sb = hb.state(b.world, 1920, 1080);
    std::printf("\n      cone %.3f deg -> gap %.2f px (rest %.2f)", sb.coneDeg, sb.crosshairGap, gapFor(0.25f));
    CHECK(sb.coneDeg > 0.25f + 0.5f && near(sb.crosshairGap, gapFor(sb.coneDeg), 0.01f), "the crosshair opens with bloom");
}

static void fx_hud_hit_marker_and_damage_vignette_timing() {
    // EnemyA straight ahead: a round that damages it shows the hit marker for 0.15 s (9 steps).
    Arena a({-2.5f, 0.0f, 2.0f}, 0.0f, steadyRifle(), enemies());
    toughen(a);
    CombatHud hud;
    hud.observe(a.world);
    (void)a.run(1, {actionEvent("fire", true)});
    hud.observe(a.world);
    CHECK(hud.state(a.world, 1280, 720).hitMarker, "the round's step: the hit marker shows");
    int shown = 1;
    (void)a.run(1, {actionEvent("fire", false)});
    for (int i = 0; i < 20; ++i) {
        hud.observe(a.world);
        shown += hud.state(a.world, 1280, 720).hitMarker ? 1 : 0;
        (void)a.run(1);
    }
    std::printf("\n      hit marker shown for %d steps", shown);
    CHECK(shown == 9, "for 0.15 s: nine 60 Hz steps");
    // Damage taken: the vignette for 0.3 s (18 steps), fading.
    Arena b({-2.0f, 0.0f, 2.0f}, 0.0f, Weapon{});
    CombatHud hb;
    hb.observe(b.world);
    b.combat->damage(b.player, 20.0f, entt::null, {});
    (void)b.run(1);
    hb.observe(b.world);
    const float first = hb.state(b.world, 1280, 720).hurt;
    int hurt = 0;
    float last = first;
    bool fading = true;
    for (int i = 0; i < 30; ++i) {
        const float v = hb.state(b.world, 1280, 720).hurt;
        hurt += v > 0.0f ? 1 : 0;
        fading = fading && v <= last;
        last = v;
        (void)b.run(1);
        hb.observe(b.world);
    }
    std::printf("\n      vignette %.3f at first, shown for %d steps", first, hurt);
    CHECK(first > 0.0f && first <= CombatHud::kHurtMaxAlpha && fading, "the vignette fades from the hit");
    CHECK(hurt >= 17 && hurt <= 18, "for 0.3 s");
    CHECK(countKind(CombatHud::layout(HudState{true, 80.0f, 100.0f, true, 30, 30, false, 0.25f, 10.0f, false, 0.4f},
                                      1280, 720),
                    ohao::render::HudKind::Vignette) == 1,
          "the layout draws it as one vignette");
}

static void fx_hud_dead_and_cleared_screens() {
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, Weapon{}, enemies());
    toughen(a);
    (void)startRound(a);
    CombatHud hud;
    hud.observe(a.world);
    HudState s = hud.state(a.world, 1920, 1080);
    CHECK(s.round && s.phase == RoundPhase::Playing, "a running round");
    a.combat->damage(byId(a, kEnemyA), 1000.0f, a.player, {});
    a.combat->damage(byId(a, kEnemyB), 1000.0f, a.player, {});
    (void)a.run(2);
    hud.observe(a.world);
    s = hud.state(a.world, 1920, 1080);
    ohao::render::HudList list = CombatHud::layout(s, 1920, 1080);
    CHECK(s.phase == RoundPhase::Won && hasText(list, "CLEARED") && hasText(list, "ENTER TO RESTART"), "won: CLEARED");
    (void)a.run(1, {actionEvent("restart", true)});
    a.combat->damage(a.player, 100000.0f, byId(a, kEnemyA), {});
    (void)a.run(1, {actionEvent("restart", false)});
    hud.observe(a.world);
    s = hud.state(a.world, 1920, 1080);
    list = CombatHud::layout(s, 1920, 1080);
    CHECK(s.phase == RoundPhase::Dead && s.restartIn == 3 && hasText(list, "DEAD") && hasText(list, "RESTART 3"),
          "dead: DEAD, RESTART 3");
    (void)a.run(60);
    hud.observe(a.world);
    CHECK(hud.state(a.world, 1920, 1080).restartIn == 2, "a second later: 2");
    CHECK(countKind(list, ohao::render::HudKind::Rect) >= 1, "a red wash over the screen");
    (void)a.run(125);
    hud.observe(a.world);
    s = hud.state(a.world, 1920, 1080);
    CHECK(s.phase == RoundPhase::Playing && s.health == s.maxHealth && !hasText(CombatHud::layout(s, 1920, 1080), "DEAD"),
          "the restart: playing, full health, no screen");
}

// The won screen (sub-project 9, "SP7 leftovers"): no crosshair or hit marker over ENTER TO RESTART.
static void t21_won_screen_no_crosshair() {
    const auto centreRects = [](const ohao::render::HudList& list) {
        std::size_t n = 0;
        for (const auto& it : list.items)
            if (it.kind == static_cast<uint32_t>(ohao::render::HudKind::Rect)) {
                const glm::vec2 c((it.rect.x + it.rect.z) * 0.5f, (it.rect.y + it.rect.w) * 0.5f);
                n += glm::length(c - glm::vec2(960.0f, 540.0f)) < 120.0f ? 1 : 0;
            }
        return n;
    };
    HudState s{true, 100.0f, 100.0f, true, 30, 30, false, 0.25f, 10.0f, true, 0.0f};
    s.round = true;
    const std::size_t playing = centreRects(CombatHud::layout(s, 1920, 1080));
    CHECK(playing == 8, "playing: four crosshair arms and four hit-marker corners at the centre");
    s.phase = RoundPhase::Won;
    const ohao::render::HudList won = CombatHud::layout(s, 1920, 1080);
    std::printf("\n      centre rectangles: playing %zu, won %zu", playing, centreRects(won));
    CHECK(centreRects(won) == 0, "won: none (no crosshair, no hit marker)");
    CHECK(hasText(won, "CLEARED") && hasText(won, "ENTER TO RESTART") && hasText(won, "30/30"),
          "the won screen and the rest of the HUD stay");
}

static void fx_hud_without_a_player_is_empty() {
    World w;
    installCombat(w);
    CombatHud hud;
    hud.observe(w);
    const HudState s = hud.state(w, 1280, 720);
    CHECK(!s.player && CombatHud::layout(s, 1280, 720).empty(), "no player: nothing drawn");
}

// --dump-burst FILE: a 30-round burst at the north wall from (-2, 0, 2), as JSON (each round's step,
// cone, eye, direction, hit point; the view's pitch and yaw each step), for the look shots' plot.
// ---- test 21: the first-person gun (sub-project 9) ----------------------------------------------
// docs/specs/2026-10-27-feel-design.md, "The gun: placement and procedural motion".

namespace {

constexpr glm::vec3 kGunAt{0.22f, -0.21f, -0.26f};   // the play scene's gun, below the head

// The arena's player with the play scene's gun (a ViewModel below the head) and its muzzle.
struct GunArena : Arena {
    entt::entity gun{entt::null};
    entt::entity muzzle{entt::null};
    explicit GunArena(Weapon weapon = {}) : Arena({-2.0f, 0.0f, 2.0f}, 0.0f, weapon) {
        gun = world.createWithId(0xb1a7e4, "Gun");
        world.setTransform(gun, Transform{kGunAt});
        world.setParent(gun, head);
        world.registry().emplace<ViewModel>(gun);
        muzzle = world.createWithId(0xb1a7e5, "Muzzle");
        world.setTransform(muzzle, Transform{{0.0f, 0.01f, -0.26f}});
        world.setParent(muzzle, gun);
        frame(0.0f);
    }
    // One drawn frame: `dt` seconds since the last (0: lockstep, the rest pose).
    void frame(float dt) {
        world.setPresentationDt(dt);
        world.sync();
    }
    // A step as a window runs it: simulate only (the frame syncs).
    std::vector<CombatEvent> stepOnly(std::vector<InputEvent> events = {}) {
        world.stepInput(events);
        world.simulate(kDt);
        const auto ev = combatEvents(world);
        return {ev.begin(), ev.end()};
    }
    const ViewModelMotion& motion() const { return *viewModelMotion(world, gun); }
    glm::mat4 drawn(entt::entity e) const {
        const auto* rp = world.registry().try_get<RenderPose>(e);
        return rp ? rp->world : world.registry().get<WorldTransform>(e).world;
    }
    glm::mat4 headWorld() const { return world.registry().get<WorldTransform>(head).world; }
};

bool nearMat(const glm::mat4& a, const glm::mat4& b, float eps) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::fabs(a[c][r] - b[c][r]) > eps) return false;
    return true;
}

} // namespace

static void t21_view_model_rest_pose() {
    GunArena a;
    const ViewModel vm;
    const glm::mat4 local = Transform{kGunAt}.matrix();
    const glm::mat4 expect = a.headWorld() * viewModelRemap(local, 75.0f, vm.fovDeg, vm.depthScale);
    CHECK(nearMat(a.drawn(a.gun), expect, 1e-6f), "no turn, walk or round: the gun is drawn at the remap of its pose");
    const glm::mat4 muzzleExpect =
        a.headWorld() * viewModelRemap(local * Transform{{0.0f, 0.01f, -0.26f}}.matrix(), 75.0f, vm.fovDeg, vm.depthScale);
    CHECK(nearMat(a.drawn(a.muzzle), muzzleExpect, 1e-6f), "its subtree (the muzzle) with it");
    a.frame(0.0f);
    const auto& rp = a.world.registry().get<RenderPose>(a.gun);
    CHECK(rp.prevWorld == rp.world, "a still frame: no motion vector");
    CHECK(a.motion().swayYawDeg == 0.0f && a.motion().kickBack == 0.0f && a.motion().bobWeight == 0.0f, "motion at rest");
}

static void t21_view_model_placement() {
    // Points in front of the eye, in view space: shrunk to at most depthScale of their distance, and
    // through the scene's 75 degrees they land where they land unremapped through the gun's 60.
    bool closer = true, sameImage = true;
    const float sceneHalf = std::tan(glm::radians(37.5f)), gunHalf = std::tan(glm::radians(30.0f));
    for (int i = 0; i < 1000; ++i) {
        const float u = std::fmod(i * 0.6180339887f, 1.0f), v = std::fmod(i * 0.7548776662f, 1.0f);
        const glm::vec3 p(u - 0.5f, (v - 0.5f) * 0.8f, -0.1f - 0.9f * std::fmod(i * 0.5698402910f, 1.0f));
        const glm::vec3 q(viewModelRemap(glm::translate(glm::mat4(1.0f), p), 75.0f, 60.0f, 0.25f)[3]);
        closer = closer && glm::length(q) <= 0.25f * glm::length(p) * (1.0f + 1e-6f);
        sameImage = sameImage && near(q.x / -q.z / sceneHalf, p.x / -p.z / gunHalf, 1e-5f) &&
                    near(q.y / -q.z / sceneHalf, p.y / -p.z / gunHalf, 1e-5f);
    }
    CHECK(closer, "every remapped point within depthScale of its distance from the eye");
    CHECK(sameImage, "projected through 75 degrees it lands where the original lands through 60");
}

static void t21_sway_lags_and_returns() {
    GunArena a;
    a.frame(kDt);   // the first live frame sees the view
    Transform t = a.world.registry().get<Transform>(a.player);
    t.rotation = yawRotation(10.0f) * t.rotation;   // a 10 degree turn left in one frame
    a.world.setTransform(a.player, t);
    a.frame(kDt);
    const float lag = a.motion().swayYawDeg;
    std::printf("\n      after a 10 degree turn: sway %.4f degrees", lag);
    CHECK(near(lag, -0.8f, 1e-3f), "the gun lags 0.8 degrees (swayDegPerDeg 0.08), turned the other way");
    CHECK(!nearMat(a.drawn(a.gun), a.headWorld() * viewModelRemap(Transform{kGunAt}.matrix(), 75.0f, 60.0f, 0.25f), 1e-5f),
          "and is drawn swayed");
    for (int i = 0; i < 30; ++i) a.frame(kDt);
    CHECK(std::fabs(a.motion().swayYawDeg) < 0.1f, "back within 0.1 degrees after 0.5 s");
}

static void t21_bob_follows_distance() {
    GunArena a;
    auto& ps = a.world.registry().get<PlayerState>(a.player);
    const float dt = 0.05f;
    a.frame(dt);
    ps.onGround = true;
    ps.velocity = {1.5f, 0.0f, 0.0f};   // 0.075 m a frame: a tenth of a stride
    for (int i = 0; i < 5; ++i) a.frame(dt);
    CHECK(near(a.motion().bobPhase, 0.5f, 1e-4f), "half a cycle after 0.375 m walked (one a 0.75 m stride)");
    CHECK(a.motion().bobWeight > 0.5f, "the bob eased in");
    for (int i = 0; i < 5; ++i) a.frame(dt);
    CHECK(a.motion().bobPhase < 1e-3f || a.motion().bobPhase > 1.0f - 1e-3f, "a whole cycle after 0.75 m");
    const float phase = a.motion().bobPhase;
    ps.velocity = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 10; ++i) a.frame(dt);
    CHECK(a.motion().bobPhase == phase && a.motion().bobWeight < 0.01f, "none standing: the phase holds, the bob eases out");
    ps.velocity = {1.5f, 0.0f, 0.0f};
    ps.onGround = false;
    for (int i = 0; i < 10; ++i) a.frame(dt);
    CHECK(a.motion().bobPhase == phase, "none in the air");
}

static void t21_kick_per_round_returns() {
    GunArena a(steadyRifle());
    a.frame(kDt);
    (void)a.stepOnly({actionEvent("fire", true)});
    a.frame(kDt);
    const ViewModel vm;
    std::printf("\n      after a round: back %.4f m, up %.3f degrees", a.motion().kickBack, a.motion().kickPitchDeg);
    CHECK(near(a.motion().kickBack, vm.kickBack, 1e-6f) && near(a.motion().kickPitchDeg, vm.kickPitchDeg, 1e-5f),
          "a round kicks the gun back 0.03 m and up 2.5 degrees");
    CHECK(a.motion().rounds == 1, "one round seen");
    (void)a.stepOnly({actionEvent("fire", false)});
    for (int i = 0; i < 30; ++i) a.frame(kDt);
    CHECK(a.motion().kickBack < 0.01f * vm.kickBack && a.motion().kickPitchDeg < 0.01f * vm.kickPitchDeg,
          "and returns within 0.5 s");
}

static void t21_view_model_resets_on_restart() {
    GunArena a(steadyRifle());
    installRoundHook(a.world, engineTypes());
    a.world.registry().emplace<Round>(a.world.createWithId(0x40a1d, "Round"));
    (void)a.stepOnly();   // the round's first step: its start
    a.frame(kDt);
    (void)a.stepOnly({actionEvent("fire", true)});
    Transform t = a.world.registry().get<Transform>(a.player);
    t.rotation = yawRotation(20.0f) * t.rotation;
    a.world.setTransform(a.player, t);
    a.frame(kDt);
    CHECK(a.motion().kickBack > 0.0f && a.motion().swayYawDeg != 0.0f, "mid-burst: kicked and swayed");
    const auto ev = a.stepOnly({actionEvent("fire", false), actionEvent("restart", true)});
    CHECK(only(ev, Kind::Restart).size() == 1, "restart pressed");
    a.frame(kDt);
    CHECK(a.motion().kickBack == 0.0f && a.motion().kickPitchDeg == 0.0f && a.motion().swayYawDeg == 0.0f,
          "the restored round starts with the gun at rest: no kick or sway carried over");
}

static void t21_view_model_never_in_world() {
    GunArena a;
    const uint64_t h0 = hashOf(a.world);
    const auto saved0 = saveScene(a.world, engineTypes());
    const glm::mat4 rest = a.drawn(a.gun);
    auto& ps = a.world.registry().get<PlayerState>(a.player);
    ps.onGround = true;
    ps.velocity = {2.0f, 0.0f, 0.0f};
    for (int i = 0; i < 20; ++i) a.frame(kDt);
    CHECK(!nearMat(a.drawn(a.gun), rest, 1e-5f), "the gun moved (bob)");
    const auto saved1 = saveScene(a.world, engineTypes());
    CHECK(hashOf(a.world) == h0, "world.hash unchanged by the motion");
    CHECK(saved0 && saved1 && saved0.value() == saved1.value(), "the saved file unchanged");
    const Snapshot s = takeSnapshot(a.world, engineTypes());
    a.frame(0.0f);
    CHECK(nearMat(a.drawn(a.gun), rest, 1e-6f), "a lockstep frame draws the rest pose again");
    restoreSnapshot(a.world, s);
    CHECK(hashOf(a.world) == h0, "a snapshot carries none of it");
}

// ---- sub-project 10, step 2: the enemy's animation (tests 23 and 25) -----------------------------
// docs/specs/2026-10-28-animation-design.md, "The step": the enemy driver (ecs/enemy_anim.cpp) writes
// the graph's parameters from saved state and the step's combat events; the enemy graph
// (assets/anim/enemy.animgraph.json) turns them into states within that step.

static void t23_enemy_states_from_ai() {
    {   // idle, then attack: the combat state from the step its mode turns, a kick per round, the aim
        Arena a(kHidden, 0.0f, {}, animatedEnemies());
        toughen(a);
        (void)a.run(5);
        const entt::entity e = byId(a, kEnemyA);
        CHECK(stateOf(a.world, e, kBody) == "move" && near(paramOf(a.world, e, "speed"), 0.0f, 0.05f) &&
                  near(paramOf(a.world, e, "alive"), 1.0f) && near(paramOf(a.world, e, "mode"), 0.0f),
              "idle: the move blend at speed 0, alive, mode 0");
        a.physics->teleportPlayer(a.player, {-2.0f, 0.0f, 2.0f}, 0.0f);
        int attackStep = -1, shotStep = -1;
        bool combatAtOnce = false, kickAtOnce = false, aimFollows = true;
        for (int i = 0; i < 120 && shotStep < 0; ++i) {
            const auto ev = a.run(1);
            if (attackStep < 0 && modeOf(a, kEnemyA) == EnemyMode::Attack) {
                attackStep = i;
                combatAtOnce = stateOf(a.world, e, kBody) == "combat";
            }
            if (modeOf(a, kEnemyA) == EnemyMode::Attack) {
                const float pitch = std::clamp(a.world.registry().get<MoveIntent>(e).pitchDeg, -60.0f, 60.0f);
                aimFollows &= near(paramOf(a.world, e, "aimPitch"), pitch, 1e-4f);
            }
            if (!from(ev, Kind::Shot, kEnemyA).empty()) {
                shotStep = i;
                kickAtOnce = stateOf(a.world, e, kFire) == "kick" && stepsIn(a.world, e, kFire) == 1;
            }
        }
        std::printf("\n      attack at step %d, first round at %d", attackStep, shotStep);
        CHECK(attackStep >= 0 && combatAtOnce, "attack: the combat state in the same step");
        CHECK(aimFollows, "aimPitch is the AI's pitch, within +-60");
        CHECK(shotStep >= 0 && kickAtOnce, "a round: the fire layer's kick from that step");
    }
    {   // chase: the move blend at its walking speed
        Enemy ai;
        ai.attackRange = 2.0f;
        Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, {}, animatedEnemies(ai));
        toughen(a);
        (void)a.run(30);
        const entt::entity e = byId(a, kEnemyA);
        std::printf("\n      chasing at %.3f m/s", paramOf(a.world, e, "speed"));
        CHECK(modeOf(a, kEnemyA) == EnemyMode::Chase && stateOf(a.world, e, kBody) == "move" &&
                  paramOf(a.world, e, "speed") > 2.0f,
              "chase: the move blend, toward the run (2.5 m/s)");
    }
    {   // a round that damages it: a flinch from that step; death: the death clip, then held
        Arena a({-2.5f, 0.0f, 2.0f}, 0.0f, steadyRifle(), animatedEnemies());
        toughen(a);
        const entt::entity e = byId(a, kEnemyA);
        const auto ev = a.run(1, {actionEvent("fire", true)});
        (void)a.run(1, {actionEvent("fire", false)});
        bool damaged = false;
        for (const CombatEvent& c : only(ev, Kind::Damage)) damaged |= c.targetId == kEnemyA;
        CHECK(damaged, "the player's round damaged EnemyA");
        // One step later the flinch is at its second step.
        CHECK(stateOf(a.world, e, kHit) == "flinch" && stepsIn(a.world, e, kHit) == 2, "damage: the hit layer's flinch");
        a.combat->damage(e, 1000.0f, a.player, {});
        (void)a.run(1);
        CHECK(stateOf(a.world, e, kBody) == "death" && near(paramOf(a.world, e, "alive"), 0.0f),
              "death: the body's death clip from the next step");
        (void)a.run(50);
        CHECK(stateOf(a.world, e, kBody) == "dead" && upright(a.world, e) && modeOf(a, kEnemyA) == EnemyMode::Dead,
              "its clip ended (held), the Transform upright");
        const auto falls = animEventsOf(a.world, kEnemyA, "body_fall");
        CHECK(falls.size() == 1, "body_fall once");
    }
}

static void t23_enemy_reload_events() {
    Weapon three = enemyWeapon();
    three.magazine = 3;   // a burst empties it
    Arena a({-2.0f, 0.0f, 2.0f}, 0.0f, {}, animatedEnemies({}, three));
    toughen(a);
    const entt::entity e = byId(a, kEnemyA);
    uint64_t reloadAt = 0;
    for (int i = 0; i < 300 && !reloadAt; ++i) {
        (void)a.run(1);
        if (stateOf(a.world, e, kReload) == "reload" && stepsIn(a.world, e, kReload) == 1) reloadAt = a.anim->steps();
    }
    CHECK(reloadAt != 0, "an empty magazine: the reload layer's reload");
    (void)a.run(130);
    const auto out = animEventsOf(a.world, kEnemyA, "mag_out"), in = animEventsOf(a.world, kEnemyA, "mag_in"),
               bolt = animEventsOf(a.world, kEnemyA, "bolt");
    const auto at = [&](const std::vector<AnimEvent>& v) { return v.empty() ? -1 : static_cast<int>(v[0].step - reloadAt) + 1; };
    std::printf("\n      mag_out at step %d of the clip, mag_in %d, bolt %d", at(out), at(in), at(bolt));
    CHECK(at(out) == 36 && at(in) == 78 && at(bolt) == 105, "mag_out 0.6 s, mag_in 1.3 s, bolt 1.75 s into it");
}

namespace {

// The play scene as hosts load it (levels, physics, combat, the round hook, animation).
struct PlayScene {
    World w;
    FileAssetResolver files{OHAO_SOURCE_DIR};
    CombatSystem* combat{nullptr};
    PhysicsScene* physics{nullptr};
    bool loaded{false};
    PlayScene() {
        w.seedIds(7);
        ohao::level::installLevels(w, files, engineTypes());
        physics = &installPhysics(w, files);
        combat = &installCombat(w);
        installRoundHook(w, engineTypes());
        installAnimation(w, files);
        loaded = static_cast<bool>(loadSceneFile(
            w, std::filesystem::path(OHAO_SOURCE_DIR) / "assets/scenes/blockout_play.scene.json", engineTypes()));
        if (loaded)
            for (const char* sync : {"level", "player_spawn", "transform", "physics"}) w.runSync(sync);
    }
    std::vector<CombatEvent> step(int n, std::vector<InputEvent> first = {}) {
        std::vector<CombatEvent> all;
        for (int i = 0; i < n; ++i) {
            w.stepInput(i == 0 ? first : std::vector<InputEvent>{});
            w.update(kDt);
            const auto ev = combatEvents(w);
            all.insert(all.end(), ev.begin(), ev.end());
        }
        return all;
    }
};

constexpr uint64_t kPlayEnemyA = 0x5c0b1a7e00000010ull, kPlayEnemyB = 0x5c0b1a7e00000020ull;

} // namespace

static void t25_play_scene_skinned() {
    PlayScene s;
    CHECK(s.loaded, "blockout_play.scene.json loads");
    if (!s.loaded) return;
    s.w.sync();   // the first frame, before any step
    auto& reg = s.w.registry();
    for (const uint64_t id : {kPlayEnemyA, kPlayEnemyB}) {
        const entt::entity e = s.w.findById(id);
        const auto* pose = reg.try_get<AnimPose>(e);
        const bool parts = reg.all_of<SkinnedMesh, Animator, Enemy>(e);
        std::printf("\n      %llx: %zu bones in its palette, posed box top %.3f m", static_cast<unsigned long long>(id),
                    pose ? pose->palette.size() : 0u, pose ? pose->boundsMax.y : 0.0f);
        // The palette holds the bones that weight vertices (Assimp's bone list): the IK targets have none.
        CHECK(parts && pose && pose->palette.size() >= 30 && pose->model.size() == 43 && pose->boundsMax.y > 1.5f &&
                  pose->boundsMax.y < 2.3f,
              "an enemy: skinned, animated, posed from the first frame (43 joints, about 1.8 m)");
        std::size_t rifles = 0, muzzles = 0, boxes = 0;
        for (auto [c, p] : reg.view<const Parent>().each()) {
            if (p.entity != e) continue;
            boxes += reg.all_of<MeshSource>(c) && !reg.all_of<JointAttach>(c);
            if (!reg.all_of<JointAttach>(c)) continue;
            const auto* rp = reg.try_get<RenderPose>(c);
            rifles += rp && reg.get<JointAttach>(c).joint == "Index1.R" && rp->world[3].y > 0.5f && rp->world[3].y < 1.8f;
            for (auto [m, mp] : reg.view<const Parent>().each())
                muzzles += mp.entity == c && reg.all_of<Muzzle>(m) && reg.all_of<RenderPose>(m);
        }
        CHECK(rifles == 1 && muzzles == 1, "its rifle in its right hand (JointAttach), the muzzle light on it, drawn there");
        CHECK(boxes == 0, "its body and head boxes draw nothing (hit volumes: the capsule, ruling 18)");
    }
}

static void t25_fight_states() {
    PlayScene s;
    CHECK(s.loaded, "blockout_play.scene.json loads");
    if (!s.loaded) return;
    auto& reg = s.w.registry();
    entt::entity player = entt::null;
    for (auto [e, pc] : reg.view<const PlayerController>().each())
        if (pc.input) player = e;
    reg.replace<Health>(player, Health{10000.0f, 10000.0f});
    (void)s.step(1);
    const uint64_t h0 = hashOf(s.w);   // the round's start
    const entt::entity a = s.w.findById(kPlayEnemyA), b = s.w.findById(kPlayEnemyB);
    const bool animated = reg.all_of<Animator>(a) && reg.all_of<Animator>(b);
    CHECK(animated, "both enemies animated");
    if (!animated) return;
    const Animator animA = reg.get<Animator>(a), animB = reg.get<Animator>(b);
    // Face EnemyA where the player stands, fire bursts of three until it is dead.
    const glm::vec3 me = s.physics->playerInfo(player)->pos, it = reg.get<Transform>(a).position;
    s.physics->teleportPlayer(player, me, glm::degrees(std::atan2(-(it.x - me.x), -(it.z - me.z))));
    int damageSteps = 0, flinched = 0, deathStep = -1, i = 0;
    bool combatBefore = false;
    while (reg.get<Health>(a).current > 0.0f && i < 600) {
        const bool down = i % 40 == 0, up = i % 40 == 13;
        const auto ev = s.step(1, down ? std::vector{actionEvent("fire", true)}
                                       : up ? std::vector{actionEvent("fire", false)} : std::vector<InputEvent>{});
        combatBefore |= stateOf(s.w, a, kBody) == "combat";
        bool hurt = false, died = false;
        for (const CombatEvent& c : ev) {
            hurt |= c.kind == Kind::Damage && c.targetId == kPlayEnemyA;
            died |= c.kind == Kind::Death && c.targetId == kPlayEnemyA;
        }
        if (hurt && !died) {
            ++damageSteps;
            flinched += stateOf(s.w, a, kHit) == "flinch" && stepsIn(s.w, a, kHit) == 1;
        }
        if (died) deathStep = i;
        ++i;
    }
    std::printf("\n      EnemyA died at step %d after %d damaging steps (%d flinched)", deathStep, damageSteps, flinched);
    CHECK(deathStep >= 0, "EnemyA shot down");
    CHECK(combatBefore, "it fought back in its combat state first");
    CHECK(damageSteps > 0 && flinched == damageSteps, "every round that hurt it: a flinch in that step");
    CHECK(stateOf(s.w, a, kBody) == "death", "its death: the death clip in the step it died");
    (void)s.step(50);
    CHECK(stateOf(s.w, a, kBody) == "dead" && upright(s.w, a), "held at its end, the Transform upright");
    CHECK(animEventsOf(s.w, kPlayEnemyA, "body_fall").size() == 1, "body_fall once");
    CHECK(layerOf(s.w, b, kBody) != nullptr, "EnemyB's animator stepped too");
    (void)s.combat->damage(b, 1000.0f, entt::null, {});
    (void)s.step(2);
    CHECK(s.combat->round().phase == RoundPhase::Won, "both dead: won");
    (void)s.step(1, {actionEvent("restart", true)});
    const auto same = [](const Animator& x, const Animator& y) {
        if (x.layers.size() != y.layers.size()) return false;
        for (std::size_t l = 0; l < x.layers.size(); ++l)
            if (x.layers[l].state != y.layers[l].state || x.layers[l].steps != y.layers[l].steps ||
                x.layers[l].from != y.layers[l].from || x.layers[l].fromSteps != y.layers[l].fromSteps ||
                x.layers[l].fade != y.layers[l].fade || x.layers[l].faded != y.layers[l].faded)
                return false;
        return true;
    };
    CHECK(same(reg.get<Animator>(a), animA) && same(reg.get<Animator>(b), animB),
          "the restart (t23_restart_resets): both Animators as at the round's start");
    CHECK(hashOf(s.w) == h0, "and world.hash the round start's");
}

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
    RUN(t16_enemies_spawn_at_enemy_spawns);
    RUN(t16_idle_without_sight);
    RUN(t16_sees_and_chases);
    RUN(t16_attacks_in_range_after_reaction_time);
    RUN(t16_bursts_and_pauses);
    RUN(t16_loses_player_returns_to_idle);
    RUN(t16_hit_wakes_idle_enemy);
    RUN(t16_dead_enemy_blocks_nothing);
    RUN(t16_enemy_fire_kills_player);
    RUN(t16_runs_are_identical);
    RUN(t16_snapshot_replays_a_fight);
    RUN(t16_play_scene_has_two_enemies);
    RUN(t17_round_start_event);
    RUN(t17_restart_equals_round_start);
    RUN(t17_restart_key);
    RUN(t17_dead_player_cannot_move_or_fire);
    RUN(t17_win_when_all_enemies_dead);
    RUN(t17_no_round_no_restart);
    RUN(t17_restart_replays_identically);
    RUN(t17_restart_after_win_replays_identically);
    RUN(t17_restart_after_kills_keeps_restored_rotations);
    RUN(fx_muzzle_flash_on_for_flash_time);
    RUN(fx_impact_pool_caps_and_reuses_oldest);
    RUN(fx_marks_from_hits_never_saved_or_hashed);
    RUN(fx_marks_on_an_enemy_land_on_its_body_and_follow_it);
    RUN(fx_no_marks_on_the_player);
    RUN(fx_marks_cleared_on_restart);
    RUN(fx_hud_shows_health_ammo_and_a_crosshair);
    RUN(fx_hud_hit_marker_and_damage_vignette_timing);
    RUN(fx_hud_dead_and_cleared_screens);
    RUN(t21_won_screen_no_crosshair);
    RUN(fx_hud_without_a_player_is_empty);
    // test 21: the first-person gun
    RUN(t21_view_model_rest_pose);
    RUN(t21_view_model_placement);
    RUN(t21_sway_lags_and_returns);
    RUN(t21_bob_follows_distance);
    RUN(t21_kick_per_round_returns);
    RUN(t21_view_model_resets_on_restart);
    RUN(t21_view_model_never_in_world);
    RUN(t23_enemy_states_from_ai);
    RUN(t23_enemy_reload_events);
    RUN(t25_play_scene_skinned);
    RUN(t25_fight_states);
    return ohao::test::finish("combat_tests");
}
