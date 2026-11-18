// The slice's registrations (slice/module.hpp), a part of types.cpp's: the enemies and the round.
#include "slice/combat.hpp"
#include "slice/components.hpp"
#include "slice/module.hpp"
#include "game/api.hpp"

namespace ohao::slice {

void registerSliceTypesEnemies(reflect::TypeRegistry& r) {
    r.type<Enemy>("Enemy").component().host(makeComponentOps<Enemy>())
        .doc("An enemy: a character (PlayerController with input false) with Health and a Weapon, driven by the enemy "
             "AI at the start of each combat step: idle until it sees the player or is hit, then it chases the place "
             "it last saw the player, and in range with a clear line it attacks in bursts.")
        .field("sightRange", &Enemy::sightRange).doc("How far it sees").unit("m").min(0.0).max(1000.0)
        .field("fovDeg", &Enemy::fovDeg).doc("The cone it notices the player in while idle").unit("deg").min(0.0)
            .max(360.0)
        .field("attackRange", &Enemy::attackRange).doc("How near it comes before it stops and fires").unit("m")
            .min(0.0).max(1000.0)
        .field("reactionTime", &Enemy::reactionTime).doc("From first sight to first round").unit("s").min(0.0)
            .max(60.0)
        .field("aimErrorDeg", &Enemy::aimErrorDeg).doc("At most this off the player, each round (seeded)").unit("deg")
            .min(0.0).max(45.0)
        .field("burst", &Enemy::burst).doc("Rounds a burst").unitless().min(1.0).max(1000.0)
        .field("burstPause", &Enemy::burstPause).doc("Between bursts").unit("s").min(0.0).max(60.0)
        .field("loseTime", &Enemy::loseTime).doc("How long it chases without seeing the player").unit("s").min(0.0)
            .max(600.0)
        .field("turnRateDegPerSec", &Enemy::turnRateDegPerSec).doc("How fast it turns").unit("deg/s").min(0.0)
            .max(100000.0);
    r.enumeration<EnemyMode>("EnemyMode")
        .doc("What an enemy is doing.")
        .value("Idle", EnemyMode::Idle, "Standing; watching its cone")
        .value("Chase", EnemyMode::Chase, "Walking to where it last saw the player")
        .value("Attack", EnemyMode::Attack, "Stopped, turned to the player, firing bursts")
        .value("Dead", EnemyMode::Dead, "Health 0: no character, lying down");
    r.type<EnemyState>("EnemyState").component().host(makeComponentOps<EnemyState>())
        .doc("What an enemy is doing. The enemy AI writes it, and adds it to an Enemy without one.")
        .field("mode", &EnemyState::mode).doc("Idle, Chase, Attack or Dead")
        .field("lastSeen", &EnemyState::lastSeen).doc("Where it last saw the player (its feet)").unit("m")
        .field("lostFor", &EnemyState::lostFor).doc("Since it last saw the player").unit("s").min(0.0)
        .field("reaction", &EnemyState::reaction).doc("Left before it may fire").unit("s").min(0.0)
        .field("burstLeft", &EnemyState::burstLeft).doc("Rounds left in this burst").unitless()
        .field("pause", &EnemyState::pause).doc("Left of the pause between bursts").unit("s").min(0.0)
        .field("lastHealth", &EnemyState::lastHealth).doc("Its health at the last step (a fall wakes it); < 0: unknown")
            .unitless()
        .field("shotsSeen", &EnemyState::shotsSeen).doc("Its weapon's shots at the last step (counts a burst)")
            .unitless();
    r.type<Round>("Round").component().singleton().host(makeComponentOps<Round>())
        .doc("The round's rules (one per world). The world's state at the end of its first combat step is the round's "
             "start; restartDelay after the player dies, or when `restart` is pressed, the world goes back to it. "
             "Every Enemy dead wins the round (it then waits for `restart`).")
        .field("restartDelay", &Round::restartDelay).doc("From the player's death to the restart").unit("s").min(0.0)
            .max(600.0)
        .field("restart", &Round::restart).doc("Action that restarts the round now").maxLength(64);
}

} // namespace ohao::slice
