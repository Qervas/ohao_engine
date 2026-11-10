#include "slice/enemy_ai.hpp"

#include "slice/combat.hpp"
#include "game/api.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace ohao::slice {

namespace {

constexpr float kTimeEps = 1e-5f;
constexpr float kReachedM = 0.5f;          // close enough to where it last saw the player
constexpr float kChestOfHeight = 0.65f;    // where it aims on the player, of the player's height
constexpr float kLeaveAttack = 1.1f;       // attack range is left beyond this much of it

glm::quat yawQuat(float deg) { return glm::angleAxis(glm::radians(deg), glm::vec3(0.0f, 1.0f, 0.0f)); }

// The yaw (about +Y from -Z) that faces along a horizontal direction.
float yawToward(glm::vec3 d) { return glm::degrees(std::atan2(-d.x, -d.z)); }

// From `from` toward `to`, at most `maxStep` degrees.
float turn(float from, float to, float maxStep) {
    const float delta = std::remainder(to - from, 360.0f);
    return std::remainder(from + std::clamp(delta, -maxStep, maxStep), 360.0f);
}

struct Target {
    entt::entity e{entt::null};
    glm::vec3 feet{0.0f}, chest{0.0f};
};

// The player the enemies fight: the first living character moved by input, in id order.
Target findTarget(const World& world, const entt::registry& reg, const PhysicsScene& physics) {
    Target best;
    uint64_t bestId = UINT64_MAX;
    for (auto [e, pc] : reg.view<const PlayerController>().each()) {
        if (!pc.input) continue;
        if (const auto* h = reg.try_get<Health>(e); h && !(h->current > 0.0f)) continue;
        const auto info = physics.playerInfo(e);
        const uint64_t id = world.idOf(e);
        if (!info || id >= bestId) continue;
        bestId = id;
        best.e = e;
        best.feet = info->pos;
        best.chest = info->pos + glm::vec3(0.0f, pc.height * kChestOfHeight, 0.0f);
    }
    return best;
}

// A dead enemy stops where it stood: its character goes (it blocks nothing and rays pass). Its
// Transform stays upright; the animation's death clip is the fall (sub-project 10, OPEN ruling 7,
// provisional: it replaced a rigid 90-degree tip).
void die(entt::registry& reg, entt::entity e, EnemyState& s) {
    s.mode = EnemyMode::Dead;
    if (auto* mi = reg.try_get<MoveIntent>(e)) mi->move = glm::vec2(0.0f);
    if (auto* t = reg.try_get<WeaponTrigger>(e)) t->pull = false;
    reg.remove<PlayerController>(e);
}

} // namespace

void stepEnemies(World& world, PhysicsScene& physics, float dt) {
    auto& reg = world.registry();
    const auto view = reg.view<Enemy>();
    if (view.empty()) return;
    std::vector<std::pair<uint64_t, entt::entity>> order;
    order.reserve(view.size());
    for (const entt::entity e : view) order.emplace_back(world.idOf(e), e);
    std::sort(order.begin(), order.end());
    const Target target = findTarget(world, reg, physics);

    for (const auto& [id, e] : order) {
        if (!reg.valid(e) || !reg.all_of<Enemy>(e)) continue;
        const Enemy ai = reg.get<Enemy>(e);
        const auto* health = reg.try_get<Health>(e);
        if (!reg.all_of<EnemyState>(e)) {
            EnemyState st;
            st.lastHealth = health ? health->current : -1.0f;
            if (const auto* ws = reg.try_get<WeaponState>(e)) st.shotsSeen = ws->shots;
            reg.emplace<EnemyState>(e, st);
        }
        EnemyState& s = reg.get<EnemyState>(e);
        if (s.mode == EnemyMode::Dead) continue;
        if (health && !(health->current > 0.0f)) {
            die(reg, e, s);
            continue;
        }
        const auto me = reg.all_of<PlayerController>(e) ? physics.playerInfo(e) : std::nullopt;
        if (!me) continue;   // not a character (yet): nothing to move or aim
        const PlayerController& pc = reg.get<PlayerController>(e);
        if (!reg.all_of<MoveIntent>(e)) reg.emplace<MoveIntent>(e, MoveIntent{{0.0f, 0.0f}, me->yawDeg, me->pitchDeg});
        if (!reg.all_of<WeaponTrigger>(e)) reg.emplace<WeaponTrigger>(e);
        MoveIntent& mi = reg.get<MoveIntent>(e);
        WeaponTrigger& trigger = reg.get<WeaponTrigger>(e);
        trigger.pull = false;
        mi.move = glm::vec2(0.0f);

        // Hit since the last step: it turns on the player, wherever it was looking.
        const bool woke = health && s.lastHealth >= 0.0f && health->current < s.lastHealth;
        s.lastHealth = health ? health->current : -1.0f;
        // Rounds its weapon fired since the last step count down the burst.
        const auto* ws = reg.try_get<WeaponState>(e);
        const uint32_t shotsNow = ws ? ws->shots : s.shotsSeen;
        const uint32_t fired = shotsNow - s.shotsSeen;
        s.shotsSeen = shotsNow;
        if (s.burstLeft > 0 && fired > 0) {
            s.burstLeft -= std::min(fired, s.burstLeft);
            if (s.burstLeft == 0) s.pause = ai.burstPause;
        }

        // Sight: in range, in its cone while idle (anywhere once alert), and a clear line.
        const glm::vec3 eye = me->pos + glm::vec3(0.0f, pc.eyeHeight, 0.0f);
        bool seen = false;
        float dist = 0.0f;
        if (target.e != entt::null) {
            const glm::vec3 to = target.chest - eye;
            dist = glm::length(to);
            const bool alert = s.mode != EnemyMode::Idle || woke;
            bool inCone = true;
            if (!alert) {
                const glm::vec3 facing = yawQuat(me->yawDeg) * glm::vec3(0.0f, 0.0f, -1.0f);
                const glm::vec3 flat(to.x, 0.0f, to.z);
                const float len = glm::length(flat);
                inCone = len < 1e-4f || glm::dot(facing, flat / len) >= std::cos(glm::radians(0.5f * ai.fovDeg));
            }
            if (dist <= ai.sightRange && inCone && dist > 1e-4f) {
                const auto hit = physics.raycastFrom(eye, to, dist + 0.5f, e);
                seen = hit && hit->entity == target.e;
            }
        }
        if (woke && target.e != entt::null && s.mode == EnemyMode::Idle) {
            s.mode = EnemyMode::Chase;
            s.lastSeen = target.feet;
            s.lostFor = 0.0f;
        }
        if (seen) {
            s.lastSeen = target.feet;
            s.lostFor = 0.0f;
        } else {
            s.lostFor += dt;
        }

        const float maxTurn = ai.turnRateDegPerSec * dt;
        const auto startAttack = [&] {
            s.mode = EnemyMode::Attack;
            s.reaction = ai.reactionTime;
            s.burstLeft = 0;
            s.pause = 0.0f;
        };
        switch (s.mode) {
            case EnemyMode::Idle:
                if (seen) {
                    if (dist <= ai.attackRange) startAttack();
                    else s.mode = EnemyMode::Chase;
                }
                break;
            case EnemyMode::Chase: {
                if (seen && dist <= ai.attackRange) {
                    startAttack();
                    break;
                }
                if (!seen && s.lostFor >= ai.loseTime) {
                    s.mode = EnemyMode::Idle;
                    break;
                }
                const glm::vec3 to = s.lastSeen - me->pos;
                if (glm::length(glm::vec2(to.x, to.z)) > kReachedM) {
                    mi.yawDeg = turn(mi.yawDeg, yawToward(to), maxTurn);
                    mi.pitchDeg = 0.0f;
                    mi.move = glm::vec2(0.0f, 1.0f);
                }
                break;
            }
            case EnemyMode::Attack: {
                if (!seen || dist > ai.attackRange * kLeaveAttack) {
                    s.mode = EnemyMode::Chase;
                    break;
                }
                // Turned to the player's chest, off by a seeded error drawn per round.
                const uint32_t seed = (ws ? reg.get<Weapon>(e).seed : 1u) ^ 0x5eedU;
                const float errYaw = (2.0f * combat::unitRandom(seed, shotsNow, 5) - 1.0f) * ai.aimErrorDeg;
                const float errPitch = (2.0f * combat::unitRandom(seed, shotsNow, 6) - 1.0f) * ai.aimErrorDeg;
                const glm::vec3 to = target.chest - eye;
                const float pitch = glm::degrees(std::atan2(to.y, glm::length(glm::vec2(to.x, to.z))));
                mi.yawDeg = turn(mi.yawDeg, yawToward(to) + errYaw, maxTurn);
                mi.pitchDeg = std::clamp(pitch + errPitch, -89.0f, 89.0f);
                // Fire after the reaction time, in bursts with a pause between them.
                if (s.reaction > kTimeEps) {
                    s.reaction = std::max(0.0f, s.reaction - dt);
                    if (s.reaction > kTimeEps) break;
                }
                if (s.pause > kTimeEps) {
                    s.pause = std::max(0.0f, s.pause - dt);
                    if (s.pause > kTimeEps) break;
                    s.pause = 0.0f;
                    s.burstLeft = ai.burst;
                } else if (s.burstLeft == 0) {
                    s.burstLeft = ai.burst;
                }
                trigger.pull = true;
                break;
            }
            case EnemyMode::Dead:
                break;
        }
    }
}

} // namespace ohao::slice
