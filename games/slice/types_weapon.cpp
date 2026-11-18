// The slice's registrations (slice/module.hpp), a part of types.cpp's: WeaponState to WeaponTrigger.
#include "slice/combat.hpp"
#include "slice/components.hpp"
#include "slice/module.hpp"
#include "game/api.hpp"

namespace ohao::slice {

void registerSliceTypesWeapon(reflect::TypeRegistry& r) {
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
}

} // namespace ohao::slice
