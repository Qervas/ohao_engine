#pragma once
// Combat (sub-project 7; docs/specs/2026-10-27-combat-design.md): the `combat` simulate system,
// once per fixed step after `physics`. Every entity with a Weapon, in id order, reads its trigger
// (a player's from the World's input, anyone else's from its WeaponTrigger), advances cooldown,
// reload, bloom and recoil, and fires at most one round: one ray from the eye within the spread
// cone, ignoring the shooter and triggers; what it hits takes damage if it or an ancestor has
// Health. Shots, hits, damage, deaths and reloads are events: the step's list and a log of 1,024.

#include <glm/glm.hpp>
#include <entt/entt.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ohao::ecs {

class World;

struct CombatEvent {
    // RoundStart, Restart and Win (step 3) name no source or target.
    enum class Kind : uint8_t { Shot, Hit, Damage, Death, Reload, Reloaded, RoundStart, Restart, Win };
    Kind kind{Kind::Shot};
    uint64_t step{0};                 // the combat step it happened in (1 = the first)
    uint64_t seq{0};                  // position in the log
    entt::entity source{entt::null};  // the shooter (Shot, Hit, Damage, Death: the killer; Reload*: the weapon's entity)
    entt::entity target{entt::null};  // Hit: the body hit; Damage, Death: the entity with Health
    uint64_t sourceId{0}, targetId{0};
    glm::vec3 point{0.0f};            // Shot: the eye; Hit, Damage: where it hit
    glm::vec3 normal{0.0f};           // Shot: the round's direction; Hit: the surface's normal
    float amount{0.0f};               // Shot: the cone's half angle (deg); Hit: the distance (m); Damage: hp taken
    float health{0.0f};               // Damage, Death: the target's health after
    std::string surface;              // Hit: the body's Collider.surface ("default" if none)
};

enum class RoundPhase : uint8_t { Playing, Dead, Won };

// The round as the last step left it (a world with a Round and a RoundHook).
struct RoundInfo {
    bool started{false};
    RoundPhase phase{RoundPhase::Playing};
    float restartIn{0.0f};     // s until the restart, while the player is dead
    uint32_t restarts{0};
    double lastRestartMs{0.0}; // what the last restart took (the snapshot's restore)
};

class CombatSystem {
public:
    explicit CombatSystem(World& world);
    ~CombatSystem();
    CombatSystem(const CombatSystem&) = delete;
    CombatSystem& operator=(const CombatSystem&) = delete;

    // One fixed step (the `combat` simulate system).
    void step(float dt);
    // Takes `amount` hp from `target` (or the nearest ancestor with Health); returns its health
    // after (the current health for a target without Health, a dead one, or an amount not > 0).
    // A damage event, and a death event when it reaches 0.
    float damage(entt::entity target, float amount, entt::entity source, glm::vec3 point);

    [[nodiscard]] std::span<const CombatEvent> stepEvents() const noexcept;
    // Events with seq >= since, oldest first, at most max; `dropped`: how many of them the log
    // no longer holds.
    [[nodiscard]] std::vector<CombatEvent> eventsSince(uint64_t since, std::size_t max, uint64_t& dropped) const;
    [[nodiscard]] uint64_t nextEventSeq() const noexcept;
    [[nodiscard]] uint64_t steps() const noexcept;
    [[nodiscard]] double lastStepMs() const noexcept;   // the last step's time in this system
    [[nodiscard]] float stepSeconds() const noexcept;   // the last step's dt (s); 1/60 before any step
    [[nodiscard]] RoundInfo round() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Installs the `combat` simulate system (after `physics`, which must be installed first). Returns
// the installed system if there is one.
CombatSystem& installCombat(World& world);
[[nodiscard]] CombatSystem* combatSystem(World& world);
[[nodiscard]] const CombatSystem* combatSystem(const World& world);
// The latest step's events (empty without combat).
[[nodiscard]] std::span<const CombatEvent> combatEvents(const World& world);
// shot, hit, damage, death, reload, reloaded, round_start, restart, win.
[[nodiscard]] const char* combatEventKindName(CombatEvent::Kind kind) noexcept;

namespace combat {
// Counter-based random numbers: a function of (seed, shot, i), in [0, 1).
[[nodiscard]] float unitRandom(uint32_t seed, uint32_t shot, uint32_t i);
// A direction within `coneDeg` of `aim` (unit), uniform over the cone's disc, drawn by (seed, shot).
[[nodiscard]] glm::vec3 spreadDirection(glm::vec3 aim, float coneDeg, uint32_t seed, uint32_t shot);
} // namespace combat

} // namespace ohao::ecs
