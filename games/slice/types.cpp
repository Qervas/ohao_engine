// The slice's types (step 3i: moved from the engine's ecs/scene_types.cpp and ecs/event_types.cpp, names unchanged):
// its 9 components with their docs, units and limits, and its channels' payloads (TypeRole::Event), into the host's
// scene registry after the engine's.
#include "slice/combat.hpp"
#include "slice/components.hpp"
#include "slice/module.hpp"

#include "game/api.hpp"

namespace ohao::slice {

// The parts of these registrations, each its own unit (they compile in parallel), in this order.
void registerSliceTypesWeapon(reflect::TypeRegistry& r);    // WeaponState to WeaponTrigger (types_weapon.cpp)
void registerSliceTypesEnemies(reflect::TypeRegistry& r);   // the enemies and the round (types_enemies.cpp)
void registerSliceTypesEvents(reflect::TypeRegistry& r);    // its channels' payloads (types_events.cpp)

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
    registerSliceTypesWeapon(r);
    registerSliceTypesEnemies(r);
    registerSliceTypesEvents(r);
}

} // namespace ohao::slice
