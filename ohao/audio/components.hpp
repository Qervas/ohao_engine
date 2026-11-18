#pragma once
// The audio module's components (audio/module.hpp): the world's bank and mix (AudioMix) and the reverb zones the
// listener hears (ReverbZone).
#include "reflect/asset_ref.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace ohao::ecs {

using AssetRef = reflect::AssetRef;   // as ecs/components.hpp names it

// The world's sound bank and mix, one per world (sub-project 11): a `.audio.json` naming the buses,
// sounds, ducks, snapshots and reverb presets (audio/audio_bank.hpp). Without one, hosts mix with
// the default bank (master over sfx, music and ambient).
struct AudioMix {
    AssetRef asset;   // an .audio.json, project-relative
};

// A volume whose reverb preset (a name in the world's .audio.json) the listener hears while inside
// it (sub-project 11; audio/audio_schedule.hpp): a box of half extents `size` about the entity, in
// its frame. size 0 fits the box to the entity's mesh (a level's `verb` node). Of the zones holding
// the listener the highest priority wins, then the lowest id; outside them all, the bank's
// defaultZone. A volume, not a body: the physics scene never sees it.
struct ReverbZone {
    std::string preset;
    int32_t priority{0};
    glm::vec3 size{0.0f};
};

} // namespace ohao::ecs
