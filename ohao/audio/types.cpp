// The audio module's registrations (audio/module.hpp): its components and its events' payloads.
#include "audio/components.hpp"
#include "audio/events.hpp"
#include "audio/module.hpp"

#include "ecs/scene_types.hpp"

namespace ohao::ecs {

// The parts of its registrations, each its own unit (they compile in parallel), in this order.
void registerAudioTypesEvents(reflect::TypeRegistry& r);   // its event payloads (types_events.cpp)

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
    registerAudioTypesEvents(r);
}

} // namespace ohao::ecs
