#pragma once
// The slice's components (sub-projects 7, 9 and 10; moved from the engine's ecs/components.hpp in step 3i, their
// names unchanged, so scene files need no migration). The slice is written against the engine's world, so its
// namespace sees ohao::ecs's names; it adds its own beside them.
#include "ecs/components.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace ohao::ecs {
class AnimationSystem;
class EventLog;
class PhysicsScene;
class World;
} // namespace ohao::ecs

namespace ohao::slice {

using namespace ohao::ecs;

// Hit points. Damage takes current down to 0, which is dead: the dead take no more damage and
// do not fire.
struct Health {
    float max{100.0f};
    float current{100.0f};
};

// A hitscan weapon. A player fires it from its eye along its look, by the World's input (`fire`
// held or pressed, `reload` pressed); anything else from its own origin along its -Z, by its
// WeaponTrigger. Each round is one ray to `range`; what it hits takes `damage` if it (or an
// ancestor) has Health.
struct Weapon {
    float damage{25.0f};                   // hp a round
    float fireRate{10.0f};                 // Hz, rounds a second
    bool automatic{true};                  // fires while held; false: one round a press
    uint32_t magazine{30};                 // rounds a magazine (reserve is unlimited)
    float reloadTime{1.5f};                // s
    float range{100.0f};                   // m
    float spreadDeg{0.25f};                // the cone's half angle at rest
    float bloomPerShotDeg{0.35f};          // added to the cone by each round
    float bloomMaxDeg{3.0f};               // the most bloom adds
    float bloomRecoveryDegPerSec{6.0f};    // how fast bloom falls once the weapon is idle
    float recoilPitchDeg{0.35f};           // view kick up a round
    float recoilYawDeg{0.15f};             // view kick sideways a round, at most (seeded, either way)
    float recoilRecoveryDegPerSec{6.0f};   // how fast the vertical kick comes back once idle
    uint32_t seed{1};                      // the spread's and recoil's random numbers
    std::string fire{"fire"};
    std::string reload{"reload"};
    // Aiming down the sights (sub-project 10, provisional ruling 11): a player holding `aim` steps
    // WeaponState.aim to 1 over adsTime (and back); the cone narrows linearly with it, to half at 1.
    float adsTime{0.2f};                   // s from the hip to the sights
    std::string aim{"aim"};
};

// What the weapon has and is doing. The combat step writes it (and emplaces it, a full magazine,
// for a Weapon without one); saved, so files, world.hash and snapshots hold a fight exactly.
struct WeaponState {
    uint32_t ammo{30};          // rounds in the magazine
    float cooldown{0.0f};       // s until the next round may fire
    float reloading{0.0f};      // s of reload left; 0: not reloading
    float bloomDeg{0.0f};       // added to the spread cone now
    float kickDeg{0.0f};        // vertical view kick not yet recovered
    uint32_t shots{0};          // rounds fired: the index of the next round's random numbers
    bool triggerHeld{false};    // the trigger at the last step (one round a press)
    float flash{0.0f};          // s of muzzle flash left (the Muzzle's light is on while > 0)
    float aim{0.0f};            // 0 from the hip .. 1 down the sights (Weapon.adsTime from one to the other)
};

// A weapon's muzzle flash: on a Light below the weapon's entity (its child, or deeper). The combat
// step turns the light to `intensity` for `time` after each round and to 0 otherwise; a light at 0
// has no emitter mesh, so the flash is the light's mesh while it is on.
struct Muzzle {
    float intensity{6.0f};   // the Light's intensity while the flash is on
    float time{0.05f};       // s the flash stays on after a round
};

// A first-person gun, on its root below the camera's entity (sub-project 9, "The gun"): its subtree
// is drawn remapped in view space, toward the eye by depthScale and as if through a fovDeg lens, and
// moved at render rate by sway (the view's turn, lagged), bob (the player's walk) and kick (its
// rounds). The view_model sync system (slice/view_model.hpp) writes only RenderPose: the motion is
// never saved, hashed or snapshotted, and a lockstep or headless frame draws the rest pose.
struct ViewModel {
    float fovDeg{60.0f};              // the lens the gun is drawn through
    // View-space shrink toward the eye (never further than this of its distance). 0.55 keeps the play
    // scene's gun within the 0.3 m a wall can come to the eye (the capsule's radius) and beyond the
    // depth buffer's clip at about twice the camera's 0.05 m near plane (an OpenGL-style projection
    // under Vulkan's [0, 1] depth clips there).
    float depthScale{0.55f};
    float swayDegPerDeg{0.08f};       // lag per degree the view turns
    float swayMaxDeg{3.0f};
    float swayReturnPerSec{8.0f};     // exponential return rate
    float bobAmplitude{0.012f};       // m
    float bobStride{0.75f};           // m walked a cycle
    float kickBack{0.03f};            // m back a round
    float kickPitchDeg{2.5f};         // up a round
    float kickReturnPerSec{14.0f};    // exponential return rate
};

// The trigger of a weapon that is not a player's: an enemy's AI, a turret, a test pulls it.
struct WeaponTrigger {
    bool pull{false};     // held
    bool reload{false};   // reload now (cleared when acted on)
};

// An enemy: a character (PlayerController, input false) with Health and a Weapon that the enemy
// AI drives, at the start of each combat step: idle until it sees the player (or is hit), then
// chases the place it last saw it, and in range with a clear line attacks in bursts.
struct Enemy {
    float sightRange{30.0f};       // m
    float fovDeg{140.0f};          // the cone it notices the player in while idle
    float attackRange{14.0f};      // m
    float reactionTime{0.35f};     // s from first sight to first round
    float aimErrorDeg{2.5f};       // at most this off the player, each round (seeded)
    uint32_t burst{3};             // rounds a burst
    float burstPause{0.8f};        // s between bursts
    float loseTime{3.0f};          // s it chases without seeing the player before giving up
    float turnRateDegPerSec{360.0f};
};

enum class EnemyMode : uint8_t { Idle, Chase, Attack, Dead };

// What an enemy is doing. The enemy AI writes it (and adds it to an Enemy without one); saved,
// so files, world.hash and snapshots hold the AI exactly.
struct EnemyState {
    EnemyMode mode{EnemyMode::Idle};
    glm::vec3 lastSeen{0.0f};      // where it last saw the player (its feet)
    float lostFor{0.0f};           // s since it last saw the player
    float reaction{0.0f};          // s left before it may fire
    uint32_t burstLeft{0};         // rounds left in this burst
    float pause{0.0f};             // s left of the pause between bursts
    float lastHealth{-1.0f};       // its health at the last step (a fall wakes it); < 0: unknown
    uint32_t shotsSeen{0};         // its weapon's WeaponState.shots at the last step (counts a burst)
};

// The round's rules, one per world. A world with one keeps the round's start (a snapshot through
// the host's WorldSnapshots, at the end of its first combat step) and goes back to it restartDelay
// after the player dies, or when `restart` is pressed; every Enemy dead wins the round.
struct Round {
    float restartDelay{3.0f};       // s from the player's death to the restart
    std::string restart{"restart"}; // action that restarts the round now
};

} // namespace ohao::slice
