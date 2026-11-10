// The slice's types (step 3i: moved from the engine's ecs/scene_types.cpp and ecs/event_types.cpp, names unchanged):
// its 9 components with their docs, units and limits, and its channels' payloads (TypeRole::Event), into the host's
// scene registry after the engine's.
#include "slice/combat.hpp"
#include "slice/components.hpp"
#include "slice/module.hpp"

#include "game/api.hpp"

namespace ohao::slice {

void registerSliceTypes(reflect::TypeRegistry& r) {
    // ---- components ---------------------------------------------------------------------------
    r.type<Health>("Health").component().host(makeComponentOps<Health>())
        .doc("Hit points. Damage (a weapon's round, combat.damage) takes current down to 0, which is dead: the dead "
             "take no more damage and do not fire.")
        .field("max", &Health::max).doc("Hit points when whole").unitless().min(0.0).max(1000000.0)
        .field("current", &Health::current).doc("Hit points now; 0 is dead").unitless().min(0.0).max(1000000.0);
    r.type<Weapon>("Weapon").component().host(makeComponentOps<Weapon>())
        .doc("A hitscan weapon, fired once per fixed step at most, after physics. A player fires it from its eye along "
             "its look by the world's input (`fire`, `reload`); anything else from its origin along its -Z by its "
             "WeaponTrigger. Each round is one ray within the spread cone; what it hits takes `damage` if it or an "
             "ancestor has Health. Recoil turns a player's view.")
        .field("damage", &Weapon::damage).doc("Hit points a round takes").unitless().min(0.0).max(100000.0)
        .field("fireRate", &Weapon::fireRate).doc("Rounds a second").unit("Hz").min(0.1).max(60.0)
        .field("automatic", &Weapon::automatic).doc("Fires while `fire` is held; false: one round a press")
        .field("magazine", &Weapon::magazine).doc("Rounds a magazine (the reserve is unlimited)").unitless().min(1.0)
            .max(1000.0)
        .field("reloadTime", &Weapon::reloadTime).doc("Time to reload").unit("s").min(0.0).max(60.0)
        .field("range", &Weapon::range).doc("How far a round reaches").unit("m").min(0.1).max(10000.0)
        .field("spreadDeg", &Weapon::spreadDeg).doc("Half angle of the spread cone at rest").unit("deg").min(0.0)
            .max(45.0)
        .field("bloomPerShotDeg", &Weapon::bloomPerShotDeg).doc("Added to the cone by each round").unit("deg")
            .min(0.0).max(45.0)
        .field("bloomMaxDeg", &Weapon::bloomMaxDeg).doc("The most bloom adds to the cone").unit("deg").min(0.0)
            .max(45.0)
        .field("bloomRecoveryDegPerSec", &Weapon::bloomRecoveryDegPerSec)
            .doc("How fast bloom falls once the fire interval passes without a round").unit("deg/s").min(0.0)
            .max(10000.0)
        .field("recoilPitchDeg", &Weapon::recoilPitchDeg).doc("View kick up a round").unit("deg").min(0.0).max(45.0)
        .field("recoilYawDeg", &Weapon::recoilYawDeg).doc("View kick sideways a round, at most, either way (seeded)")
            .unit("deg").min(0.0).max(45.0)
        .field("recoilRecoveryDegPerSec", &Weapon::recoilRecoveryDegPerSec)
            .doc("How fast the vertical kick comes back down once the fire interval passes without a round")
            .unit("deg/s").min(0.0).max(10000.0)
        .field("seed", &Weapon::seed).doc("Seed of the spread's and recoil's random numbers").unitless()
        .field("fire", &Weapon::fire).doc("Action that fires (a player's)").maxLength(64)
        .field("reload", &Weapon::reload).doc("Action that reloads (a player's)").maxLength(64)
        .field("adsTime", &Weapon::adsTime).doc("Time from the hip to the sights (and back); aimed, the cone halves")
            .unit("s").min(0.0).max(10.0)
        .field("aim", &Weapon::aim).doc("Action that aims down the sights while held (a player's)").maxLength(64);
    r.type<WeaponState>("WeaponState").component().host(makeComponentOps<WeaponState>())
        .doc("What a weapon has and is doing. Written by the combat step, which adds it (a full magazine) to a Weapon "
             "without one.")
        .field("ammo", &WeaponState::ammo).doc("Rounds in the magazine").unitless()
        .field("cooldown", &WeaponState::cooldown).doc("Time until the next round may fire").unit("s").min(0.0)
        .field("reloading", &WeaponState::reloading).doc("Reload time left; 0: not reloading").unit("s").min(0.0)
        .field("bloomDeg", &WeaponState::bloomDeg).doc("Added to the spread cone now").unit("deg").min(0.0)
        .field("kickDeg", &WeaponState::kickDeg).doc("Vertical view kick not yet recovered").unit("deg").min(0.0)
        .field("shots", &WeaponState::shots).doc("Rounds fired: the index of the next round's random numbers")
            .unitless()
        .field("triggerHeld", &WeaponState::triggerHeld).doc("The trigger at the last step (one round a press)")
        .field("flash", &WeaponState::flash).doc("Muzzle flash left (the Muzzle's light is on while above 0)")
            .unit("s").min(0.0)
        .field("aim", &WeaponState::aim).doc("0 from the hip, 1 down the sights; the cone narrows linearly to half")
            .unitless().min(0.0).max(1.0);
    r.type<Muzzle>("Muzzle").component().host(makeComponentOps<Muzzle>())
        .doc("A weapon's muzzle flash, on a Light below the weapon's entity: the combat step turns the light to "
             "`intensity` for `time` after each round and to 0 otherwise (a light at 0 has no emitter mesh, so the flash "
             "is the light's mesh while it is on).")
        .field("intensity", &Muzzle::intensity).doc("The Light's intensity while the flash is on").unitless()
            .min(0.0).max(100000.0)
        .field("time", &Muzzle::time).doc("How long the flash stays on after a round").unit("s").min(0.0).max(1.0);
    r.type<ViewModel>("ViewModel").component().host(makeComponentOps<ViewModel>())
        .doc("A first-person gun, on its root below the camera's entity: its subtree is drawn remapped in view space "
             "(toward the eye by depthScale, as if through a fovDeg lens, so it never reaches a wall) and moved at render "
             "rate by sway, bob and kick. Presentation only: the motion is never saved or hashed, and lockstep and "
             "headless frames draw the rest pose.")
        .field("fovDeg", &ViewModel::fovDeg).doc("The lens the gun is drawn through").unit("deg").min(10.0).max(170.0)
        .field("depthScale", &ViewModel::depthScale).doc("Shrink toward the eye in view space").unitless().min(0.01)
            .max(1.0)
        .field("swayDegPerDeg", &ViewModel::swayDegPerDeg).doc("Lag per degree the view turns").unitless().min(0.0)
            .max(1.0)
        .field("swayMaxDeg", &ViewModel::swayMaxDeg).doc("The most the sway lags").unit("deg").min(0.0).max(45.0)
        .field("swayReturnPerSec", &ViewModel::swayReturnPerSec).doc("How fast the sway returns (exponential rate)")
            .unit("Hz").min(0.0).max(1000.0)
        .field("bobAmplitude", &ViewModel::bobAmplitude).doc("Up and down while walking").unit("m").min(0.0).max(0.5)
        .field("bobStride", &ViewModel::bobStride).doc("Walked a bob cycle").unit("m").min(0.01).max(100.0)
        .field("kickBack", &ViewModel::kickBack).doc("Back a round").unit("m").min(0.0).max(1.0)
        .field("kickPitchDeg", &ViewModel::kickPitchDeg).doc("Up a round").unit("deg").min(0.0).max(45.0)
        .field("kickReturnPerSec", &ViewModel::kickReturnPerSec).doc("How fast the kick returns (exponential rate)")
            .unit("Hz").min(0.0).max(1000.0);
    r.type<WeaponTrigger>("WeaponTrigger").component().host(makeComponentOps<WeaponTrigger>())
        .doc("The trigger of a weapon that is not a player's: an AI, a turret or an agent pulls it.")
        .field("pull", &WeaponTrigger::pull).doc("Held")
        .field("reload", &WeaponTrigger::reload).doc("Reload now; cleared when acted on");
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

    // ---- the channels' payloads (slice/combat.hpp's slice_events) -------------------------------
    r.type<ShotEvent>("ShotEvent").event()
        .doc("slice.shot: a round left a weapon")
        .field("shooter", &ShotEvent::shooter).entity().doc("Who fired")
        .field("eye", &ShotEvent::eye).doc("Where the round left from").unit("m")
        .field("dir", &ShotEvent::dir).doc("Its direction, within the cone").unitless()
        .field("coneDeg", &ShotEvent::coneDeg).doc("The cone's half angle").unit("deg")
        .field("round", &ShotEvent::round).doc("The weapon's rounds fired so far, this one included").unitless()
        .field("hit", &ShotEvent::hit).doc("Where it stopped; absent: it hit nothing within range").unit("m").omitDefault();
    r.type<HitEvent>("HitEvent").event()
        .doc("slice.hit: a round met a body")
        .field("shooter", &HitEvent::shooter).entity().doc("Who fired")
        .field("body", &HitEvent::body).entity().doc("The body hit")
        .field("point", &HitEvent::point).doc("Where").unit("m")
        .field("normal", &HitEvent::normal).doc("The surface's normal").unitless()
        .field("dir", &HitEvent::dir).doc("The round's direction").unitless()
        .field("dist", &HitEvent::dist).doc("From the eye").unit("m")
        .field("surface", &HitEvent::surface).doc("The body's Collider.surface (default when it has none)");
    r.type<DamageEvent>("DamageEvent").event()
        .doc("slice.damage: health taken (a round's, or combat.damage's)")
        .field("source", &DamageEvent::source).entity().doc("Who took it")
        .field("target", &DamageEvent::target).entity().doc("The entity with the Health")
        .field("point", &DamageEvent::point).doc("Where").unit("m")
        .field("amount", &DamageEvent::amount).doc("Hit points taken").unitless()
        .field("health", &DamageEvent::health).doc("Hit points after").unitless();
    r.type<DeathEvent>("DeathEvent").event()
        .doc("slice.death: health reached 0")
        .field("killer", &DeathEvent::killer).entity().doc("Who took the last of it")
        .field("target", &DeathEvent::target).entity().doc("The entity with the Health")
        .field("point", &DeathEvent::point).doc("Where the last damage landed").unit("m");
    r.type<ReloadEvent>("ReloadEvent").event()
        .doc("slice.reload (started: ammo the rounds left, 0 from empty) and slice.reloaded (ended: ammo the magazine)")
        .field("holder", &ReloadEvent::holder).entity().doc("The weapon's entity")
        .field("ammo", &ReloadEvent::ammo).doc("Rounds in the magazine").unitless();
    r.type<RoundEvent>("RoundEvent").event()
        .doc("slice.round_start, slice.restart and slice.win: the round started, went back to its start, or was won");
    r.type<ExplodeEvent>("ExplodeEvent").event()
        .doc("slice.explode: a grenade went off")
        .field("thrower", &ExplodeEvent::thrower).entity().doc("Who threw it")
        .field("point", &ExplodeEvent::point).doc("Where").unit("m");
}

} // namespace ohao::slice
