// The audio module's registrations (audio/module.hpp): its components and its events' payloads.
#include "audio/components.hpp"
#include "audio/events.hpp"
#include "audio/module.hpp"

#include "ecs/scene_types.hpp"

namespace ohao::ecs {

void registerAudioTypes(reflect::TypeRegistry& r) {
    r.type<AudioMix>("AudioMix").component().singleton().host(makeComponentOps<AudioMix>())
        .doc("The world's sound bank and mix (one per world): an .audio.json declaring the buses and their voice caps, "
             "the sounds (files, bus, priority, distance shape, how long each holds its voice), the ducks, the "
             "snapshots and the reverb presets. Without one, the default bank (master over sfx, music, ambient).")
        .field("asset", &AudioMix::asset).doc("Project-relative path of the .audio.json file, '/' separators")
            .asset("audio bank");
    r.type<ReverbZone>("ReverbZone").component().host(makeComponentOps<ReverbZone>())
        .doc("A volume whose reverb preset the listener hears while inside it: a box about the entity, in its frame. "
             "Of the zones holding the listener the highest priority wins, then the lowest id; outside them all, the "
             "bank's defaultZone. A volume, not a body: nothing collides with it.")
        .field("preset", &ReverbZone::preset).doc("A reverb preset of the world's .audio.json (room, outdoor, ...)")
            .maxLength(32)
        .field("priority", &ReverbZone::priority).doc("Higher wins where zones overlap").unitless()
        .field("size", &ReverbZone::size).doc("The box's half extents; zero fits it to the entity's mesh").unit("m").min(0.0);

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
        .field("blendMs", &AudioBus::blendMs).doc("The ramp").unit("ms")
        .field("resync", &AudioBus::resync).doc("Resent to bring the device to what a restore put back").omitDefault();
    r.type<AudioSnapshot>("AudioSnapshot").event()
        .doc("audio.snapshot: the mix snapshot (default, dead, won)")
        .field("snapshot", &AudioSnapshot::snapshot).doc("The snapshot")
        .field("blendMs", &AudioSnapshot::blendMs).doc("The blend").unit("ms")
        .field("resync", &AudioSnapshot::resync).doc("Resent after a restore").omitDefault();
    r.type<AudioZone>("AudioZone").event()
        .doc("audio.zone: the listener's reverb zone")
        .field("zone", &AudioZone::zone).doc("The preset; empty: no reverb at all")
        .field("bed", &AudioZone::bed).doc("Its bed sound").omitDefault()
        .field("blendMs", &AudioZone::blendMs).doc("The crossfade").unit("ms")
        .field("resync", &AudioZone::resync).doc("Resent after a restore").omitDefault();
    r.type<AudioOcclusion>("AudioOcclusion").event()
        .doc("audio.occlusion: a held voice re-cast through different walls")
        .field("voice", &AudioOcclusion::voice).doc("The voice").unitless()
        .field("walls", &AudioOcclusion::walls).doc("Walls now (2: 2 or more)").unitless()
        .field("occlusionDb", &AudioOcclusion::occlusionDb).doc("Their cut").unit("dB")
        .field("lowPassHz", &AudioOcclusion::lowPassHz).doc("Their low-pass; 0: none").unit("Hz")
        .field("blendMs", &AudioOcclusion::blendMs).doc("The ramp").unit("ms");
    r.type<AudioStopWorld>("AudioStopWorld").event()
        .doc("audio.stop_world: the voices on the world's buses fade: a restore (a round's restart too) put back another moment")
        .field("blendMs", &AudioStopWorld::blendMs).doc("The fade").unit("ms");
}

} // namespace ohao::ecs
