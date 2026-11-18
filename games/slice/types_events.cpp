// The slice's registrations (slice/module.hpp), a part of types.cpp's: its channels' payloads.
#include "slice/combat.hpp"
#include "slice/components.hpp"
#include "slice/module.hpp"
#include "game/api.hpp"

namespace ohao::slice {

void registerSliceTypesEvents(reflect::TypeRegistry& r) {
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
