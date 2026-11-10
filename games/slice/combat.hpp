#pragma once
// Combat (sub-project 7; docs/specs/2026-10-27-combat-design.md): the `combat` simulate system,
// once per fixed step after `physics`. Every entity with a Weapon, in id order, reads its trigger
// (a player's from the World's input, anyone else's from its WeaponTrigger), advances cooldown,
// reload, bloom and recoil, and fires at most one round: one ray from the eye within the spread
// cone, ignoring the shooter and triggers; what it hits takes damage if it or an ancestor has
// Health. Shots, hits, damage, deaths, reloads, the round's and the grenade's are the slice's events: channels of
// the World's log (ecs/events.hpp), each with the cue the engine's data binds to.

#include "slice/components.hpp"
#include "game/api.hpp"

#include <glm/glm.hpp>
#include <entt/entt.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ohao::slice {

// The slice's channels. Entities by EntityId.
namespace slice_events {
inline constexpr const char* kShot = "slice.shot";               // ShotEvent
inline constexpr const char* kHit = "slice.hit";                 // HitEvent
inline constexpr const char* kDamage = "slice.damage";           // DamageEvent
inline constexpr const char* kDeath = "slice.death";             // DeathEvent
inline constexpr const char* kReload = "slice.reload";           // ReloadEvent
inline constexpr const char* kReloaded = "slice.reloaded";       // ReloadEvent
inline constexpr const char* kRoundStart = "slice.round_start";  // RoundEvent
inline constexpr const char* kRestart = "slice.restart";         // RoundEvent
inline constexpr const char* kWin = "slice.win";                 // RoundEvent
inline constexpr const char* kExplode = "slice.explode";         // ExplodeEvent
} // namespace slice_events

// A round left a weapon. Cue: at the eye, dir the round's, to where it stopped, source the shooter, n its round.
struct ShotEvent {
    uint64_t shooter{0};
    glm::vec3 eye{0.0f};              // where the round left from
    glm::vec3 dir{0.0f};              // its direction, within the cone
    float coneDeg{0.0f};              // the cone's half angle
    uint32_t round{0};                // the weapon's rounds fired so far, this one included
    std::optional<glm::vec3> hit;     // where it stopped; none: it hit nothing within range
};
// A round met a body. Cue: at the point, normal the surface's, dir the round's, source the shooter, target the
// body, tag its surface.
struct HitEvent {
    uint64_t shooter{0}, body{0};
    glm::vec3 point{0.0f}, normal{0.0f}, dir{0.0f};
    float dist{0.0f};                 // from the eye
    std::string surface;              // the body's Collider.surface ("default" if none)
};
// Health taken. Cue: at the point, source, target.
struct DamageEvent {
    uint64_t source{0}, target{0};    // target: the entity with the Health
    glm::vec3 point{0.0f};
    float amount{0.0f};               // hp taken
    float health{0.0f};               // after
};
// Health reached 0. Cue: at the point, source the killer, target.
struct DeathEvent {
    uint64_t killer{0}, target{0};
    glm::vec3 point{0.0f};
};
// A reload started (slice.reload: ammo the rounds left, 0 from empty) or ended (slice.reloaded: ammo the
// magazine). Cue: source the holder, tag "empty" for a reload from empty.
struct ReloadEvent {
    uint64_t holder{0};
    uint32_t ammo{0};
};
// The round started, restarted or was won (slice.round_start, slice.restart, slice.win).
struct RoundEvent {};
// A grenade went off. Cue: at its place, source the thrower.
struct ExplodeEvent {
    uint64_t thrower{0};
    glm::vec3 point{0.0f};
};

// The step's events the slice's animation drivers read (slice/arms_anim.hpp, slice/enemy_anim.hpp).
struct DriverEvents {
    std::span<const Event<ShotEvent>> shots;
    std::span<const Event<DamageEvent>> damage;
    std::span<const Event<ReloadEvent>> reloads;
    std::span<const Event<DeathEvent>> deaths;
};
// The readers they come from, taken for the `animation` system.
struct DriverReaders {
    EventReader<ShotEvent> shots;
    EventReader<DamageEvent> damage;
    EventReader<ReloadEvent> reloads;
    EventReader<DeathEvent> deaths;
    explicit DriverReaders(EventLog& log)
        : shots(log.reader<ShotEvent>(slice_events::kShot, "animation")),
          damage(log.reader<DamageEvent>(slice_events::kDamage, "animation")),
          reloads(log.reader<ReloadEvent>(slice_events::kReload, "animation")),
          deaths(log.reader<DeathEvent>(slice_events::kDeath, "animation")) {}
    [[nodiscard]] DriverEvents read() const { return {shots.read(), damage.read(), reloads.read(), deaths.read()}; }
};

enum class RoundPhase : uint8_t { Playing, Dead, Won };

// The round as the last step left it (a world with a Round and a WorldSnapshots).
struct RoundInfo {
    bool started{false};
    RoundPhase phase{RoundPhase::Playing};
    float restartIn{0.0f};     // s until the restart, while the player is dead
    uint32_t restarts{0};
    double lastRestartMs{0.0}; // what the last restart took (the snapshot's restore)
};

// A test grenade in flight: where the last step left it and where this step did.
struct CombatGrenade {
    glm::vec3 prev{0.0f}, pos{0.0f};
};

class CombatSystem {
public:
    explicit CombatSystem(World& world);
    ~CombatSystem();
    CombatSystem(const CombatSystem&) = delete;
    CombatSystem& operator=(const CombatSystem&) = delete;

    // One fixed step (the `combat` simulate system).
    void step(float dt);
    // The `combat` snapshot hook: grenades in flight, the round (its phase, timer and start).
    [[nodiscard]] std::shared_ptr<const void> saveState() const;
    void restoreState(const std::shared_ptr<const void>& state);
    void hashState(StateHash& h) const;
    // Takes `amount` hp from `target` (or the nearest ancestor with Health); returns its health
    // after (the current health for a target without Health, a dead one, or an amount not > 0).
    // A damage event, and a death event when it reaches 0 (between steps: the next step's).
    float damage(entt::entity target, float amount, entt::entity source, glm::vec3 point);

    [[nodiscard]] uint64_t steps() const noexcept;         // the World's
    [[nodiscard]] double lastStepMs() const noexcept;   // the last step's time in this system
    [[nodiscard]] float stepSeconds() const noexcept;   // the World's dt (s)
    [[nodiscard]] RoundInfo round() const noexcept;
    // The test grenades in flight (runtime: snapshotted and hashed, never saved to a file), in throw order: where the last step left each
    // and where this one did (the presentation draws between the two, sub-project 12).
    [[nodiscard]] std::vector<CombatGrenade> grenadesInFlight() const;

    struct Impl;   // public for its State's layout (the snapshot hook's: core/layout_key.hpp)

private:
    std::unique_ptr<Impl> m;
};

// Installs the `combat` simulate system (after `physics`, which must be installed first) and its channels.
// Returns the installed system if there is one. The slice's module installs it (slice/module.hpp).
CombatSystem& installCombat(World& world);
// What installCombat made that the engine does not remove itself (game::uninstallGame): its context entry, the view
// model's.
void uninstallCombat(World& world);
[[nodiscard]] CombatSystem* combatSystem(World& world);
[[nodiscard]] const CombatSystem* combatSystem(const World& world);

namespace combat {
// The test grenade (Frank's hand test, audio spec ruling 23): a sound source a player can place.
// The input-driven, living player's `grenade` action (G in the play scene) throws one from the eye
// along the look plus a lift; it flies a ballistic arc in the combat step, stops where it first
// meets a body, and goes off kFuse after the throw: an Explode event at its place, which the audio
// schedule plays as `explode`. No damage. Runtime only: snapshotted and hashed, never saved to a file; a
// round's restart clears those in flight.
inline constexpr const char* kGrenadeAction = "grenade";
inline constexpr float kGrenadeSpeed = 9.0f;       // m/s along the look
inline constexpr float kGrenadeLift = 3.0f;        // m/s added straight up
inline constexpr float kGrenadeGravity = -9.81f;   // m/s^2
inline constexpr float kGrenadeFuse = 2.5f;        // s from the throw
inline constexpr float kGrenadeCooldown = 1.0f;    // s between throws
inline constexpr float kGrenadeAhead = 0.4f;       // m in front of the eye it leaves from
inline constexpr float kGrenadeRadius = 0.05f;     // m: its drawn sphere (sub-project 12, presentation only)
} // namespace combat

namespace combat {
// Counter-based random numbers: a function of (seed, shot, i), in [0, 1).
[[nodiscard]] float unitRandom(uint32_t seed, uint32_t shot, uint32_t i);
// A direction within `coneDeg` of `aim` (unit), uniform over the cone's disc, drawn by (seed, shot).
[[nodiscard]] glm::vec3 spreadDirection(glm::vec3 aim, float coneDeg, uint32_t seed, uint32_t shot);
} // namespace combat

} // namespace ohao::slice
