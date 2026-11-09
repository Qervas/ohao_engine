// The engine's event payloads, reflected (TypeRole::Event): the API writes them (events.read), world.hash hashes the
// next step's, and the catalogue lists them. Each is the payload of the channels its doc names (ecs/events.hpp).
#include "ecs/animation.hpp"
#include "ecs/audio_schedule.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/scene_types.hpp"

namespace ohao::ecs {

void registerEventTypes(reflect::TypeRegistry& r) {
    // ---- physics ------------------------------------------------------------------------------
    r.type<ContactEvent>("ContactEvent").event()
        .doc("physics.contact_begin and physics.contact_end: a body pair began or ended touching (sub-shape contacts "
             "collapsed; a removed body ends its pairs at once)")
        .field("a", &ContactEvent::a).entity().doc("The body with the lower id")
        .field("b", &ContactEvent::b).entity().doc("The other body")
        .field("point", &ContactEvent::point).doc("Where, world; an end repeats its begin's").unit("m")
        .field("normal", &ContactEvent::normal).doc("From a toward b, world").unitless()
        .field("speed", &ContactEvent::speed).doc("The relative normal speed at a begin; 0 at an end").unit("m/s");
    r.type<TriggerEvent>("TriggerEvent").event()
        .doc("physics.trigger_enter and physics.trigger_exit: a body entered or left a trigger (a Collider with trigger)")
        .field("trigger", &TriggerEvent::trigger).entity().doc("The trigger")
        .field("body", &TriggerEvent::body).entity().doc("The body")
        .field("point", &TriggerEvent::point).doc("Where, world; an exit repeats its enter's").unit("m")
        .field("normal", &TriggerEvent::normal).doc("From the trigger toward the body, world").unitless()
        .field("speed", &TriggerEvent::speed).doc("The relative normal speed at an enter; 0 at an exit").unit("m/s");
    r.type<FootEvent>("FootEvent").event()
        .doc("physics.footstep (every 0.75 m walked, 0.95 m sprinted, on the ground) and physics.land (touching the "
             "ground after a fall of more than 0.3 m): a player's")
        .field("player", &FootEvent::player).entity().doc("The player")
        .field("ground", &FootEvent::ground).entity().doc("The ground's entity; none when it has none")
        .field("point", &FootEvent::point).doc("The feet, world").unit("m")
        .field("normal", &FootEvent::normal).doc("The ground's normal, world").unitless()
        .field("speed", &FootEvent::speed).doc("A footstep: the horizontal speed; a landing: the downward speed").unit("m/s")
        .field("surface", &FootEvent::surface).doc("The ground's Collider.surface (default when it has none)");
    r.type<JointBreakEvent>("JointBreakEvent").event()
        .doc("physics.joint_break: a joint broke (its entity gets JointState)")
        .field("joint", &JointBreakEvent::joint).entity().doc("The Joint's entity")
        .field("other", &JointBreakEvent::other).entity().doc("The other body's entity; none: the world")
        .field("point", &JointBreakEvent::point).doc("Its anchor, world").unit("m");

    // ---- animation ----------------------------------------------------------------------------
    r.type<ClipEvent>("ClipEvent").event()
        .doc("anim.clip_event: an event a layer's clip fired in the step (mag_out, mag_in, bolt, body_fall, foot_l, ...)")
        .field("entity", &ClipEvent::entity).entity().doc("The animated entity")
        .field("layer", &ClipEvent::layer).doc("The graph layer").unitless()
        .field("clip", &ClipEvent::clip).doc("The clip that fired it")
        .field("name", &ClipEvent::name).doc("The event's name in the clip");

    // ---- the audio schedule -------------------------------------------------------------------
    r.enumeration<AudioOutcome>("AudioOutcome")
        .doc("What became of a play")
        .value("played", AudioOutcome::Played, "It plays")
        .value("stole", AudioOutcome::Stole, "It plays, after stopping the voices over its bus's or the bank's cap")
        .value("dropped", AudioOutcome::Dropped, "Nothing to take: it does not play");
    r.type<AudioPlay>("AudioPlay").event()
        .doc("audio.play: a voice starts (dropped: it does not), placed, occluded and admitted by the schedule; its "
             "bus, priority and send are its sound's, in the bank")
        .field("sound", &AudioPlay::sound).doc("The bank's sound")
        .field("variant", &AudioPlay::variant).doc("Its variant (the device takes it modulo its files)").unitless().omitDefault()
        .field("voice", &AudioPlay::voice).doc("The voice").unitless()
        .field("pos", &AudioPlay::pos).doc("Where, on the sound's shape; absent: at the ear").unit("m").omitDefault()
        .field("gain", &AudioPlay::gain).doc("The bank's volume times the event's scale").unitless()
        .field("walls", &AudioPlay::walls).doc("Walls between the listener and it at the start (2: 2 or more)").unitless().omitDefault()
        .field("occlusionDb", &AudioPlay::occlusionDb).doc("Their cut").unit("dB").omitDefault()
        .field("lowPassHz", &AudioPlay::lowPassHz).doc("Their low-pass; 0: none").unit("Hz").omitDefault()
        .field("outcome", &AudioPlay::outcome).doc("Played, stole or dropped").omitDefault()
        .field("victim", &AudioPlay::victim).doc("Stole: the voice it took (the last, when it took more)").unitless().omitDefault();
    r.type<AudioStop>("AudioStop").event()
        .doc("audio.stop: a voice stops (a steal's 2 ms fade)")
        .field("voice", &AudioStop::voice).doc("The voice").unitless();
    r.type<AudioBus>("AudioBus").event()
        .doc("audio.bus: a bus's duck, the deepest duck holding it")
        .field("bus", &AudioBus::bus).doc("The bus")
        .field("db", &AudioBus::db).doc("Its level; 0: none").unit("dB")
        .field("blendSteps", &AudioBus::blendSteps).doc("The ramp, in steps").unitless()
        .field("resync", &AudioBus::resync).doc("Resent to bring the device to what a restore put back").omitDefault();
    r.type<AudioSnapshot>("AudioSnapshot").event()
        .doc("audio.snapshot: the mix snapshot (default, dead, won)")
        .field("snapshot", &AudioSnapshot::snapshot).doc("The snapshot")
        .field("blendSteps", &AudioSnapshot::blendSteps).doc("The blend, in steps").unitless()
        .field("resync", &AudioSnapshot::resync).doc("Resent after a restore").omitDefault();
    r.type<AudioZone>("AudioZone").event()
        .doc("audio.zone: the listener's reverb zone")
        .field("zone", &AudioZone::zone).doc("The preset; empty: no reverb at all")
        .field("bed", &AudioZone::bed).doc("Its bed sound").omitDefault()
        .field("blendSteps", &AudioZone::blendSteps).doc("The crossfade, in steps").unitless()
        .field("resync", &AudioZone::resync).doc("Resent after a restore").omitDefault();
    r.type<AudioOcclusion>("AudioOcclusion").event()
        .doc("audio.occlusion: a held voice re-cast through different walls")
        .field("voice", &AudioOcclusion::voice).doc("The voice").unitless()
        .field("walls", &AudioOcclusion::walls).doc("Walls now (2: 2 or more)").unitless()
        .field("occlusionDb", &AudioOcclusion::occlusionDb).doc("Their cut").unit("dB")
        .field("lowPassHz", &AudioOcclusion::lowPassHz).doc("Their low-pass; 0: none").unit("Hz")
        .field("blendSteps", &AudioOcclusion::blendSteps).doc("The ramp, in steps").unitless();
    r.type<AudioStopWorld>("AudioStopWorld").event()
        .doc("audio.stop_world: the voices on the world's buses fade (a round restart, a restore)")
        .field("blendSteps", &AudioStopWorld::blendSteps).doc("The fade, in steps").unitless()
        .field("resync", &AudioStopWorld::resync).doc("A restore's, not a restart's").omitDefault();
}

} // namespace ohao::ecs
