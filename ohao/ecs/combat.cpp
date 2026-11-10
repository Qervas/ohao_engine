#include "ecs/combat.hpp"

#include "core/log.hpp"
#include "ecs/components.hpp"
#include "ecs/audio_schedule.hpp"
#include "ecs/enemy_ai.hpp"
#include "ecs/round_hook.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/player.hpp"
#include "ecs/view_model.hpp"
#include "ecs/world.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace ohao::ecs {

namespace {

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

// The cues the engine's data binds to (ecs/events.hpp).
Cue shotCue(const ShotEvent& e) {
    Cue c;
    c.at = e.eye;
    c.dir = e.dir;
    c.to = e.hit;
    c.source = e.shooter;
    c.n = e.round;
    return c;
}
Cue hitCue(const HitEvent& e) {
    Cue c;
    c.at = e.point;
    c.normal = e.normal;
    c.dir = e.dir;
    c.source = e.shooter;
    c.target = e.body;
    c.tag = e.surface;
    return c;
}
Cue damageCue(const DamageEvent& e) {
    Cue c;
    c.at = e.point;
    c.source = e.source;
    c.target = e.target;
    return c;
}
Cue deathCue(const DeathEvent& e) {
    Cue c;
    c.at = e.point;
    c.source = e.killer;
    c.target = e.target;
    return c;
}
Cue reloadCue(const ReloadEvent& e) {
    Cue c;
    c.source = e.holder;
    if (e.ammo == 0) c.tag = "empty";
    return c;
}
Cue roundCue(const RoundEvent&) { return Cue{}; }
Cue explodeCue(const ExplodeEvent& e) {
    Cue c;
    c.at = e.point;
    c.source = e.thrower;
    return c;
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
    struct Out {
        EventWriter<ShotEvent> shot;
        EventWriter<HitEvent> hit;
        EventWriter<DamageEvent> damage;
        EventWriter<DeathEvent> death;
        EventWriter<ReloadEvent> reload, reloaded;
        EventWriter<RoundEvent> roundStart, restart, win;
        EventWriter<ExplodeEvent> explode;
    } out;
    double lastMs{0.0};
    std::vector<std::pair<uint64_t, entt::entity>> order;   // the step's weapons by id

    explicit Impl(World& w) : world(w), reg(w.registry()) {
        namespace se = slice_events;
        EventLog& log = w.events();
        out.shot = log.writer<ShotEvent>(se::kShot, "combat", &shotCue);
        out.hit = log.writer<HitEvent>(se::kHit, "combat", &hitCue);
        out.damage = log.writer<DamageEvent>(se::kDamage, "combat", &damageCue);
        out.death = log.writer<DeathEvent>(se::kDeath, "combat", &deathCue);
        out.reload = log.writer<ReloadEvent>(se::kReload, "combat", &reloadCue);
        out.reloaded = log.writer<ReloadEvent>(se::kReloaded, "combat", &reloadCue);
        out.roundStart = log.writer<RoundEvent>(se::kRoundStart, "combat", &roundCue);
        out.restart = log.writer<RoundEvent>(se::kRestart, "combat", &roundCue);
        out.win = log.writer<RoundEvent>(se::kWin, "combat", &roundCue);
        out.explode = log.writer<ExplodeEvent>(se::kExplode, "combat", &explodeCue);
    }

    [[nodiscard]] uint64_t idOf(entt::entity e) const { return e != entt::null && reg.valid(e) ? world.idOf(e) : 0; }

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
        out.damage.emit(DamageEvent{idOf(source), idOf(owner), point, before - after, after});
        if (after == 0.0f) {
            out.death.emit(DeathEvent{idOf(source), idOf(owner), point});
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
        out.reload.emit(ReloadEvent{idOf(e), s.ammo});
        if (!(s.reloading > kTimeEps)) reloadEnds(e, s, w);   // an instant reload
    }

    void reloadEnds(entt::entity e, WeaponState& s, const Weapon& w) {
        s.reloading = 0.0f;
        s.ammo = w.magazine;
        out.reloaded.emit(ReloadEvent{idOf(e), s.ammo});
    }

    void fire(entt::entity e, const Weapon& w, WeaponState& s, PhysicsScene& physics) {
        glm::vec3 eye, aim;
        bool player = false;
        eyeAndAim(e, physics, eye, aim, player);
        // Aimed, the cone narrows linearly with WeaponState.aim, to half at 1 (provisional ruling 11).
        const float cone = (w.spreadDeg + s.bloomDeg) * (1.0f - 0.5f * s.aim);
        const uint32_t shot = s.shots;
        const glm::vec3 dir = combat::spreadDirection(aim, cone, w.seed, shot);
        const auto hit = physics.raycastFrom(eye, dir, w.range, e);
        const uint64_t shooter = idOf(e);
        out.shot.emit(ShotEvent{shooter, eye, dir, cone, shot + 1, hit ? std::optional<glm::vec3>(hit->point) : std::nullopt});
        if (hit) {
            const auto* c = hit->entity != entt::null && reg.valid(hit->entity) ? reg.try_get<Collider>(hit->entity) : nullptr;
            out.hit.emit(HitEvent{shooter, idOf(hit->entity), hit->point, hit->normal, dir, hit->dist,
                                  c && !c->surface.empty() ? c->surface : std::string("default")});
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
        bool held = false, pressed = false, reload = false, aiming = false;
        if (const auto* pc = reg.try_get<PlayerController>(e); pc && pc->input) {
            const InputMap& in = std::as_const(world).input(pc->stream);   // its player's stream
            held = in.held(w.fire);
            pressed = in.pressed(w.fire);
            reload = in.pressed(w.reload);
            aiming = in.held(w.aim);
        } else if (auto* t = reg.try_get<WeaponTrigger>(e)) {
            held = t->pull;
            pressed = t->pull && !s.triggerHeld;
            reload = t->reload;
            if (t->reload) t->reload = false;
        }
        s.triggerHeld = held;
        if (!living) held = pressed = reload = aiming = false;

        // Down the sights (provisional ruling 11): toward 1 while `aim` is held and the weapon is not
        // reloading (the arms leave the sights for it), back to 0 otherwise, at 1 / adsTime a second,
        // before this step's round (whose cone it narrows).
        aiming = aiming && !(s.reloading > 0.0f);
        if (const float to = aiming ? 1.0f : 0.0f; s.aim != to) {
            const float by = w.adsTime > kTimeEps ? dt / w.adsTime : 1.0f;
            s.aim = aiming ? std::min(1.0f, s.aim + by) : std::max(0.0f, s.aim - by);
            if (std::fabs(s.aim - to) <= kTimeEps) s.aim = to;
        }

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
        if (physics && dt > 0.0f) stepGrenades(dt, *physics);
        stepRound(dt);
        // The mix the round asks for: dead while the player is, won once the room is cleared.
        requestMixSnapshot(world, roundInfo.phase == RoundPhase::Dead ? "dead" : roundInfo.phase == RoundPhase::Won ? "won" : "default");
        lastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    // ---- the test grenade (audio spec ruling 23; combat.hpp, combat::kGrenade*) ------------------
    struct Grenade {
        glm::vec3 prev{0.0f}, pos{0.0f}, vel{0.0f};   // prev: where the last step left it (drawn between steps)
        float fuse{0.0f};   // s left
        bool resting{false};
        entt::entity thrower{entt::null};
    };
    std::vector<Grenade> grenades;   // in throw order
    float grenadeCooldown{0.0f};

    // The input-driven, living player throws on `grenade`; every grenade flies (a ballistic arc,
    // stopping where a ray along this step's move first meets a body other than the thrower's) and
    // goes off when its fuse runs out: an Explode event at its place.
    void stepGrenades(float dt, PhysicsScene& physics) {
        grenadeCooldown = std::max(0.0f, grenadeCooldown - dt);
        const entt::entity player = playerOf(world);
        if (player != entt::null && alive(player) && grenadeCooldown <= kTimeEps &&
            std::as_const(world).input(reg.get<PlayerController>(player).stream).pressed(combat::kGrenadeAction))
            if (const auto info = physics.playerInfo(player)) {
                const glm::vec3 eye = info->pos + glm::vec3(0.0f, reg.get<PlayerController>(player).eyeHeight, 0.0f);
                const glm::vec3 look = yawQuat(info->yawDeg) * pitchQuat(info->pitchDeg) * glm::vec3(0.0f, 0.0f, -1.0f);
                Grenade g;
                g.pos = eye + look * combat::kGrenadeAhead;
                g.prev = g.pos;
                g.vel = look * combat::kGrenadeSpeed + glm::vec3(0.0f, combat::kGrenadeLift, 0.0f);
                g.fuse = combat::kGrenadeFuse;
                g.thrower = player;
                grenades.push_back(g);
                grenadeCooldown = combat::kGrenadeCooldown;
            }
        for (Grenade& g : grenades) {
            g.prev = g.pos;
            if (!g.resting) {
                const glm::vec3 next = g.pos + g.vel * dt + glm::vec3(0.0f, 0.5f * combat::kGrenadeGravity * dt * dt, 0.0f);
                const glm::vec3 move = next - g.pos;
                const float len = glm::length(move);
                const auto hit = len > 0.0f ? physics.raycastFrom(g.pos, move / len, len, g.thrower) : std::nullopt;
                if (hit) {
                    g.pos = hit->point + hit->normal * 0.05f;
                    g.vel = glm::vec3(0.0f);
                    g.resting = true;
                } else {
                    g.pos = next;
                    g.vel.y += combat::kGrenadeGravity * dt;
                }
            }
            g.fuse -= dt;
        }
        for (const Grenade& g : grenades)
            if (g.fuse <= kTimeEps) out.explode.emit(ExplodeEvent{idOf(g.thrower), g.pos});
        std::erase_if(grenades, [](const Grenade& g) { return g.fuse <= kTimeEps; });
    }

    // ---- the round (step 3) ------------------------------------------------------------------
    bool roundStarted{false};
    std::shared_ptr<const void> roundStart;   // the world at the end of the round's first step
    RoundInfo roundInfo;
    float deadFor{0.0f};
    bool warnedNoHook{false};


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
            out.roundStart.emit(RoundEvent{});
            roundStartStep = world.steps();
            // The round's start: the world at the end of this step, between steps (World::defer).
            world.defer([this, hook](World&) { roundStart = hook->save(); });
            return;
        }
        // The player (ecs/player.hpp).
        const entt::entity player = playerOf(world);
        const auto* playerHealth = player != entt::null ? reg.try_get<Health>(player) : nullptr;
        const bool playerDead = playerHealth && !(playerHealth->current > 0.0f);
        std::size_t enemies = 0, dead = 0;
        for (auto [e, en] : reg.view<const Enemy>().each()) {
            ++enemies;
            const auto* h = reg.try_get<Health>(e);
            dead += h && !(h->current > 0.0f) ? 1 : 0;
        }
        if (enemies > 0 && dead == enemies && roundInfo.phase != RoundPhase::Won && !playerDead) {
            roundInfo.phase = RoundPhase::Won;
            out.win.emit(RoundEvent{});
        }
        if (playerDead) {
            roundInfo.phase = RoundPhase::Dead;
            deadFor += dt;
        } else {
            deadFor = 0.0f;
            if (roundInfo.phase == RoundPhase::Dead) roundInfo.phase = RoundPhase::Playing;
        }
        roundInfo.restartIn = playerDead ? std::max(0.0f, round.restartDelay - deadFor) : 0.0f;
        const bool pressed = !round.restart.empty() && player != entt::null &&
                             std::as_const(world).input(reg.get<PlayerController>(player).stream).pressed(round.restart);
        if ((pressed || (playerDead && deadFor + kTimeEps >= round.restartDelay)) && roundStart)
            world.defer([this, hook](World&) { restart(*hook); });   // after this step's last system
    }

    // Back to the round's start, between steps: the next step is the round's second again, and starts with the
    // restore's `world.restore` and then `slice.restart`.
    void restart(const RoundHook& hook) {
        const auto t0 = std::chrono::steady_clock::now();
        // The restore puts back combat's own state too (the `combat` hook): what counts the round's history goes on
        // from here instead.
        const auto start = roundStart;
        const uint32_t restartsNow = roundInfo.restarts;
        hook.restore(start);
        roundStart = start;
        roundInfo.restarts = restartsNow;
        roundInfo.lastRestartMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++roundInfo.restarts;
        roundInfo.phase = RoundPhase::Playing;
        roundInfo.restartIn = 0.0f;
        deadFor = 0.0f;
        grenades.clear();   // runtime: the round's start had none in flight
        grenadeCooldown = 0.0f;
        out.restart.emit(RoundEvent{});
    }

    // ---- what a snapshot carries (the `combat` hook): everything a later step reads --------------
    struct State {
        std::vector<Grenade> grenades;
        float grenadeCooldown{0.0f};
        bool roundStarted{false};
        std::shared_ptr<const void> roundStart;
        uint64_t roundStartStep{0};
        RoundInfo roundInfo;
        float deadFor{0.0f};
    };
    uint64_t roundStartStep{0};   // the step the round's start was taken at (what world.hash counts of it)

    [[nodiscard]] std::shared_ptr<const void> save() const {
        return std::make_shared<const State>(State{grenades, grenadeCooldown, roundStarted, roundStart, roundStartStep,
                                                   roundInfo, deadFor});
    }
    void restore(const State& s) {
        grenades = s.grenades;
        grenadeCooldown = s.grenadeCooldown;
        roundStarted = s.roundStarted;
        roundStart = s.roundStart;
        roundStartStep = s.roundStartStep;
        roundInfo = s.roundInfo;
        deadFor = s.deadFor;
    }
    void hash(StateHash& h) const {
        h.u64(grenades.size());
        for (const Grenade& g : grenades) {
            h.vec3(g.prev);
            h.vec3(g.pos);
            h.vec3(g.vel);
            h.f32(g.fuse);
            h.boolean(g.resting);
            h.entity(g.thrower);
        }
        h.f32(grenadeCooldown);
        h.boolean(roundStarted);
        h.boolean(roundStart != nullptr);
        h.u64(roundStartStep);
        h.boolean(roundInfo.started);
        h.u32(static_cast<uint32_t>(roundInfo.phase));
        h.f32(roundInfo.restartIn);
        h.u32(roundInfo.restarts);
        h.f32(deadFor);
    }
};

CombatSystem::CombatSystem(World& world) : m(std::make_unique<Impl>(world)) {}
CombatSystem::~CombatSystem() = default;

void CombatSystem::step(float dt) { m->step(dt); }
std::shared_ptr<const void> CombatSystem::saveState() const { return m->save(); }
void CombatSystem::restoreState(const std::shared_ptr<const void>& state) {
    if (const auto* s = static_cast<const Impl::State*>(state.get())) m->restore(*s);
}
void CombatSystem::hashState(StateHash& h) const { m->hash(h); }

float CombatSystem::damage(entt::entity target, float amount, entt::entity source, glm::vec3 point) {
    if (target == entt::null || !m->reg.valid(target)) return 0.0f;
    return m->damage(target, amount, source, point);
}

uint64_t CombatSystem::steps() const noexcept { return m->world.steps(); }
double CombatSystem::lastStepMs() const noexcept { return m->lastMs; }
float CombatSystem::stepSeconds() const noexcept { return m->world.stepDt(); }
RoundInfo CombatSystem::round() const noexcept { return m->roundInfo; }
std::vector<CombatGrenade> CombatSystem::grenadesInFlight() const {
    std::vector<CombatGrenade> out;
    out.reserve(m->grenades.size());
    for (const auto& g : m->grenades) out.push_back({g.prev, g.pos});
    return out;
}

CombatSystem& installCombat(World& world) {
    auto& reg = world.registry();
    if (const auto* installed = reg.ctx().find<CombatInstalled>()) return *installed->system;
    // The system owns the combat state: it goes with the World's systems.
    auto system = std::make_shared<CombatSystem>(world);
    reg.ctx().emplace<CombatInstalled>(CombatInstalled{system.get()});
    // Rounds and grenades meet the world as the step's physics left it.
    world.addSimulate("combat", [system](World&, float dt) { system->step(dt); }, {.after = {"physics"}});
    CombatSystem* raw = system.get();
    addSnapshotHook(reg, SnapshotHook{"combat", [raw] { return raw->saveState(); },
                                      [raw](const std::shared_ptr<const void>& s) { raw->restoreState(s); },
                                      [raw](StateHash& h) { raw->hashState(h); }});
    // The first-person gun's placement and motion (presentation, render rate).
    installViewModel(world);
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

} // namespace ohao::ecs
