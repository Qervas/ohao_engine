#include "ecs/combat.hpp"

#include "core/log.hpp"
#include "ecs/components.hpp"
#include "ecs/enemy_ai.hpp"
#include "ecs/round_hook.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/world.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>

namespace ohao::ecs {

namespace {

constexpr std::size_t kLogCapacity = 1024;
// Cooldowns and reloads count down by dt; this absorbs the float error of their sums, so 10 Hz at
// 60 Hz steps fires on every sixth step exactly.
constexpr float kTimeEps = 1e-5f;

struct CombatInstalled {
    CombatSystem* system{nullptr};
};

uint32_t mix(uint32_t x) {   // lowbias32 (Wellons): a full-avalanche 32-bit hash
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

glm::quat yawQuat(float deg) { return glm::angleAxis(glm::radians(deg), glm::vec3(0.0f, 1.0f, 0.0f)); }
glm::quat pitchQuat(float deg) { return glm::angleAxis(glm::radians(deg), glm::vec3(1.0f, 0.0f, 0.0f)); }

} // namespace

namespace combat {

float unitRandom(uint32_t seed, uint32_t shot, uint32_t i) {
    const uint32_t h = mix(mix(mix(seed ^ 0x9e3779b9U) ^ shot) + i * 0x85ebca6bU);
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

glm::vec3 spreadDirection(glm::vec3 aim, float coneDeg, uint32_t seed, uint32_t shot) {
    if (!(coneDeg > 0.0f)) return aim;
    const glm::vec3 a = glm::normalize(aim);
    const glm::vec3 helper = std::fabs(a.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 right = glm::normalize(glm::cross(a, helper));
    const glm::vec3 up = glm::cross(right, a);
    // Uniform over the cone's disc one unit ahead: radius tan(cone) * sqrt(u1), angle 2 pi u2.
    const float r = std::tan(glm::radians(coneDeg)) * std::sqrt(unitRandom(seed, shot, 0));
    const float phi = 6.28318530718f * unitRandom(seed, shot, 1);
    return glm::normalize(a + r * (std::cos(phi) * right + std::sin(phi) * up));
}

} // namespace combat

struct CombatSystem::Impl {
    World& world;
    entt::registry& reg;
    std::vector<CombatEvent> stepEvents;
    std::deque<CombatEvent> log;
    uint64_t nextSeq{0};
    uint64_t steps{0};
    double lastMs{0.0};
    float lastDt{1.0f / 60.0f};
    std::vector<std::pair<uint64_t, entt::entity>> order;   // the step's weapons by id

    explicit Impl(World& w) : world(w), reg(w.registry()) {}

    void push(CombatEvent ev) {
        ev.step = steps;
        ev.seq = nextSeq++;
        ev.sourceId = ev.source != entt::null && reg.valid(ev.source) ? world.idOf(ev.source) : 0;
        ev.targetId = ev.target != entt::null && reg.valid(ev.target) ? world.idOf(ev.target) : 0;
        stepEvents.push_back(ev);
        log.push_back(std::move(ev));
        if (log.size() > kLogCapacity) log.pop_front();
    }

    // The entity whose Health a hit on `e` takes from: itself or its nearest ancestor with one.
    [[nodiscard]] entt::entity healthOf(entt::entity e) const {
        for (int depth = 0; e != entt::null && reg.valid(e) && depth < 64; ++depth) {
            if (reg.all_of<Health>(e)) return e;
            const auto* p = reg.try_get<Parent>(e);
            e = p ? p->entity : entt::entity{entt::null};
        }
        return entt::null;
    }

    [[nodiscard]] bool alive(entt::entity e) const {
        const auto* h = reg.try_get<Health>(e);
        return !h || h->current > 0.0f;
    }

    float damage(entt::entity target, float amount, entt::entity source, glm::vec3 point) {
        const entt::entity owner = healthOf(target);
        if (owner == entt::null) return 0.0f;
        const float before = reg.get<Health>(owner).current;
        if (!(amount > 0.0f) || !(before > 0.0f)) return before;
        const float after = std::max(0.0f, before - amount);
        reg.patch<Health>(owner, [&](Health& h) { h.current = after; });
        CombatEvent ev;
        ev.kind = CombatEvent::Kind::Damage;
        ev.source = source;
        ev.target = owner;
        ev.point = point;
        ev.amount = before - after;
        ev.health = after;
        push(ev);
        if (after == 0.0f) {
            ev.kind = CombatEvent::Kind::Death;
            ev.amount = 0.0f;
            push(ev);
            OHAO_LOG(Debug, ecs, "combat: {} killed {}", world.idOf(source), world.idOf(owner));
        }
        return after;
    }

    // Where a weapon's round leaves from and toward: a character's eye along its look; anything
    // else's origin along its -Z.
    void eyeAndAim(entt::entity e, const PhysicsScene& physics, glm::vec3& eye, glm::vec3& aim, bool& player) const {
        if (const auto info = reg.all_of<PlayerController>(e) ? physics.playerInfo(e) : std::nullopt) {
            player = true;
            eye = info->pos + glm::vec3(0.0f, reg.get<PlayerController>(e).eyeHeight, 0.0f);
            aim = yawQuat(info->yawDeg) * pitchQuat(info->pitchDeg) * glm::vec3(0.0f, 0.0f, -1.0f);
            return;
        }
        player = false;
        const auto* wt = reg.all_of<Parent>(e) ? reg.try_get<WorldTransform>(e) : nullptr;
        if (wt) {
            eye = glm::vec3(wt->world[3]);
            aim = glm::normalize(glm::mat3(wt->world) * glm::vec3(0.0f, 0.0f, -1.0f));
        } else if (const auto* t = reg.try_get<Transform>(e)) {
            eye = t->position;
            aim = t->rotation * glm::vec3(0.0f, 0.0f, -1.0f);
        } else {
            eye = glm::vec3(0.0f);
            aim = glm::vec3(0.0f, 0.0f, -1.0f);
        }
    }

    void reloadStarts(entt::entity e, WeaponState& s, const Weapon& w) {
        s.reloading = w.reloadTime;
        CombatEvent ev;
        ev.kind = CombatEvent::Kind::Reload;
        ev.source = e;
        ev.amount = static_cast<float>(s.ammo);
        push(ev);
        if (!(s.reloading > kTimeEps)) reloadEnds(e, s, w);   // an instant reload
    }

    void reloadEnds(entt::entity e, WeaponState& s, const Weapon& w) {
        s.reloading = 0.0f;
        s.ammo = w.magazine;
        CombatEvent ev;
        ev.kind = CombatEvent::Kind::Reloaded;
        ev.source = e;
        ev.amount = static_cast<float>(s.ammo);
        push(ev);
    }

    void fire(entt::entity e, const Weapon& w, WeaponState& s, PhysicsScene& physics) {
        glm::vec3 eye, aim;
        bool player = false;
        eyeAndAim(e, physics, eye, aim, player);
        const float cone = w.spreadDeg + s.bloomDeg;
        const uint32_t shot = s.shots;
        const glm::vec3 dir = combat::spreadDirection(aim, cone, w.seed, shot);
        CombatEvent ev;
        ev.kind = CombatEvent::Kind::Shot;
        ev.source = e;
        ev.point = eye;
        ev.normal = dir;
        ev.amount = cone;
        push(ev);
        if (const auto hit = physics.raycastFrom(eye, dir, w.range, e)) {
            CombatEvent h;
            h.kind = CombatEvent::Kind::Hit;
            h.source = e;
            h.target = hit->entity;
            h.point = hit->point;
            h.normal = hit->normal;
            h.amount = hit->dist;
            const auto* c = hit->entity != entt::null && reg.valid(hit->entity) ? reg.try_get<Collider>(hit->entity) : nullptr;
            h.surface = c && !c->surface.empty() ? c->surface : std::string("default");
            push(h);
            if (hit->entity != entt::null) damage(hit->entity, w.damage, e, hit->point);
        }
        // Recoil turns a player's view now; the vertical part is recovered later.
        if (player) {
            const float yawKick = (2.0f * combat::unitRandom(w.seed, shot, 2) - 1.0f) * w.recoilYawDeg;
            physics.turnPlayer(e, yawKick, w.recoilPitchDeg);
            s.kickDeg += w.recoilPitchDeg;
        }
        s.bloomDeg = std::min(s.bloomDeg + w.bloomPerShotDeg, w.bloomMaxDeg);
        ++s.shots;
    }

    void updateWeapon(entt::entity e, float dt, PhysicsScene& physics) {
        const Weapon w = reg.get<Weapon>(e);   // a copy: damage may touch other pools
        if (!reg.all_of<WeaponState>(e)) reg.emplace<WeaponState>(e, WeaponState{w.magazine});
        WeaponState& s = reg.get<WeaponState>(e);
        if (s.ammo > w.magazine) s.ammo = w.magazine;
        const bool living = alive(e);

        // The trigger: a player's from the World's input, anyone else's from its WeaponTrigger.
        bool held = false, pressed = false, reload = false;
        if (const auto* pc = reg.try_get<PlayerController>(e); pc && pc->input) {
            const InputMap& in = world.input();
            held = in.held(w.fire);
            pressed = in.pressed(w.fire);
            reload = in.pressed(w.reload);
        } else if (auto* t = reg.try_get<WeaponTrigger>(e)) {
            held = t->pull;
            pressed = t->pull && !s.triggerHeld;
            reload = t->reload;
            if (t->reload) t->reload = false;
        }
        s.triggerHeld = held;
        if (!living) held = pressed = reload = false;

        if (s.reloading > 0.0f) {
            s.reloading -= dt;
            if (s.reloading <= kTimeEps) reloadEnds(e, s, w);
        }
        if (reload && s.reloading == 0.0f && s.ammo < w.magazine) reloadStarts(e, s, w);

        bool fired = false;
        const bool wants = w.automatic ? held : pressed;
        if (wants && s.reloading == 0.0f && s.ammo > 0 && s.cooldown <= kTimeEps) {
            fire(e, w, s, physics);
            fired = true;
            s.cooldown += 1.0f / w.fireRate;
            --s.ammo;
            if (s.ammo == 0) reloadStarts(e, s, w);
        }
        // Idle (the fire interval passed without a round): bloom and the vertical kick recover.
        if (!fired && s.cooldown <= kTimeEps) {
            s.cooldown = 0.0f;
            s.bloomDeg = std::max(0.0f, s.bloomDeg - w.bloomRecoveryDegPerSec * dt);
            if (s.kickDeg > 0.0f) {
                const float back = std::min(s.kickDeg, w.recoilRecoveryDegPerSec * dt);
                physics.turnPlayer(e, 0.0f, -back);
                s.kickDeg -= back;
            }
        }
        s.cooldown -= dt;
        if (!fired && s.cooldown < 0.0f) s.cooldown = 0.0f;
        updateFlash(e, s, fired, dt);
    }

    // The weapon's Muzzle: the first Light with one below it, depth first in child order.
    [[nodiscard]] entt::entity muzzleOf(entt::entity e) const {
        const auto* ch = reg.try_get<Children>(e);
        if (!ch) return entt::null;
        for (const entt::entity c : ch->entities) {
            if (!reg.valid(c)) continue;
            if (reg.all_of<Muzzle, Light>(c)) return c;
            if (const entt::entity deeper = muzzleOf(c); deeper != entt::null) return deeper;
        }
        return entt::null;
    }

    // The muzzle flash: on for Muzzle.time after a round (the light at its intensity), else off.
    void updateFlash(entt::entity e, WeaponState& s, bool fired, float dt) {
        const entt::entity muzzle = muzzleOf(e);
        if (fired && muzzle != entt::null) {
            s.flash = reg.get<Muzzle>(muzzle).time;
        } else if (s.flash > 0.0f) {
            s.flash -= dt;
            if (s.flash <= kTimeEps) s.flash = 0.0f;
        }
        if (muzzle == entt::null) return;
        const float want = s.flash > 0.0f ? reg.get<Muzzle>(muzzle).intensity : 0.0f;
        if (reg.get<Light>(muzzle).intensity != want) reg.patch<Light>(muzzle, [&](Light& l) { l.intensity = want; });
    }

    void step(float dt) {
        const auto t0 = std::chrono::steady_clock::now();
        ++steps;
        if (dt > 0.0f) lastDt = dt;
        stepEvents.clear();
        PhysicsScene* physics = physicsScene(world);
        // The enemies decide first: their triggers fire in this step's weapons, their moves walk in
        // the next step's physics.
        if (physics && dt > 0.0f) stepEnemies(world, *physics, dt);
        const auto weapons = reg.view<Weapon>();
        if (physics && !weapons.empty() && dt > 0.0f) {
            order.clear();
            for (const entt::entity e : weapons) order.emplace_back(world.idOf(e), e);
            std::sort(order.begin(), order.end());
            for (const auto& [id, e] : order)
                if (reg.valid(e) && reg.all_of<Weapon>(e)) updateWeapon(e, dt, *physics);
        }
        stepRound(dt);
        lastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    // ---- the round (step 3) ------------------------------------------------------------------
    bool roundStarted{false};
    std::shared_ptr<const void> roundStart;   // the world at the end of the round's first step
    RoundInfo roundInfo;
    float deadFor{0.0f};
    bool warnedNoHook{false};

    void pushRound(CombatEvent::Kind kind) {
        CombatEvent ev;
        ev.kind = kind;
        push(ev);
    }

    // The world with a Round: starts it (a snapshot of the world after this step), watches the
    // player's death and the enemies', and goes back to the start restartDelay after the death or
    // when `restart` is pressed (at the end of this step: the world after it is the round's start).
    void stepRound(float dt) {
        const auto rounds = reg.view<const Round>();
        if (rounds.empty()) return;
        const Round round = reg.get<Round>(rounds.front());
        const auto* hook = reg.ctx().find<RoundHook>();
        if (!hook || !hook->save || !hook->restore) {
            if (!warnedNoHook)
                OHAO_LOG(Warn, ecs, "combat: the world has a Round but its host installed no RoundHook; no round runs");
            warnedNoHook = true;
            return;
        }
        if (!roundStarted) {
            roundStarted = true;
            roundInfo = RoundInfo{};
            roundInfo.started = true;
            deadFor = 0.0f;
            pushRound(CombatEvent::Kind::RoundStart);
            roundStart = hook->save();
            return;
        }
        // The player: the first character moved by input, in id order.
        bool playerDead = false;
        uint64_t bestId = UINT64_MAX;
        for (auto [e, pc] : reg.view<const PlayerController>().each()) {
            const uint64_t id = world.idOf(e);
            if (!pc.input || id >= bestId) continue;
            bestId = id;
            const auto* h = reg.try_get<Health>(e);
            playerDead = h && !(h->current > 0.0f);
        }
        std::size_t enemies = 0, dead = 0;
        for (auto [e, en] : reg.view<const Enemy>().each()) {
            ++enemies;
            const auto* h = reg.try_get<Health>(e);
            dead += h && !(h->current > 0.0f) ? 1 : 0;
        }
        if (enemies > 0 && dead == enemies && roundInfo.phase != RoundPhase::Won && !playerDead) {
            roundInfo.phase = RoundPhase::Won;
            pushRound(CombatEvent::Kind::Win);
        }
        if (playerDead) {
            roundInfo.phase = RoundPhase::Dead;
            deadFor += dt;
        } else {
            deadFor = 0.0f;
            if (roundInfo.phase == RoundPhase::Dead) roundInfo.phase = RoundPhase::Playing;
        }
        roundInfo.restartIn = playerDead ? std::max(0.0f, round.restartDelay - deadFor) : 0.0f;
        const bool pressed = !round.restart.empty() && world.input().pressed(round.restart);
        if (pressed || (playerDead && deadFor + kTimeEps >= round.restartDelay)) {
            const auto t0 = std::chrono::steady_clock::now();
            hook->restore(roundStart);
            roundInfo.lastRestartMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++roundInfo.restarts;
            roundInfo.phase = RoundPhase::Playing;
            roundInfo.restartIn = 0.0f;
            deadFor = 0.0f;
            pushRound(CombatEvent::Kind::Restart);
        }
    }
};

CombatSystem::CombatSystem(World& world) : m(std::make_unique<Impl>(world)) {}
CombatSystem::~CombatSystem() = default;

void CombatSystem::step(float dt) { m->step(dt); }

float CombatSystem::damage(entt::entity target, float amount, entt::entity source, glm::vec3 point) {
    if (target == entt::null || !m->reg.valid(target)) return 0.0f;
    return m->damage(target, amount, source, point);
}

std::span<const CombatEvent> CombatSystem::stepEvents() const noexcept { return m->stepEvents; }

std::vector<CombatEvent> CombatSystem::eventsSince(uint64_t since, std::size_t max, uint64_t& dropped) const {
    const uint64_t first = m->nextSeq - m->log.size();
    dropped = since < first ? first - since : 0;
    std::vector<CombatEvent> out;
    for (std::size_t i = static_cast<std::size_t>(std::max(since, first) - first); i < m->log.size() && out.size() < max; ++i)
        out.push_back(m->log[i]);
    return out;
}

uint64_t CombatSystem::nextEventSeq() const noexcept { return m->nextSeq; }
uint64_t CombatSystem::steps() const noexcept { return m->steps; }
double CombatSystem::lastStepMs() const noexcept { return m->lastMs; }
float CombatSystem::stepSeconds() const noexcept { return m->lastDt; }
RoundInfo CombatSystem::round() const noexcept { return m->roundInfo; }

CombatSystem& installCombat(World& world) {
    auto& reg = world.registry();
    if (const auto* installed = reg.ctx().find<CombatInstalled>()) return *installed->system;
    // The system owns the combat state: it goes with the World's systems.
    auto system = std::make_shared<CombatSystem>(world);
    reg.ctx().emplace<CombatInstalled>(CombatInstalled{system.get()});
    world.addSimulate("combat", [system](World&, float dt) { system->step(dt); });
    return *system;
}

CombatSystem* combatSystem(World& world) {
    const auto* installed = world.registry().ctx().find<CombatInstalled>();
    return installed ? installed->system : nullptr;
}

const CombatSystem* combatSystem(const World& world) {
    const auto* installed = world.registry().ctx().find<CombatInstalled>();
    return installed ? installed->system : nullptr;
}

std::span<const CombatEvent> combatEvents(const World& world) {
    const CombatSystem* s = combatSystem(world);
    return s ? s->stepEvents() : std::span<const CombatEvent>{};
}

} // namespace ohao::ecs
