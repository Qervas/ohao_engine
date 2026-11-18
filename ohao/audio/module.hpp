#pragma once
// The audio module, "audio": the audio schedule (audio/audio_schedule.hpp: the `audio_schedule` simulate system,
// what the world sounds like as the `audio.*` events), its components (audio/components.hpp) and events
// (audio/events.hpp). Below it, in the same directory: the bank (ohao_audio_bank: audio_bank.hpp, the .audio.json
// reader) and the mixer a window plays the events through (ohao_audio_mixer: audio_system.hpp, reverb_node.hpp). The
// sound sources other modules add (footsteps) are its contributions.
#include "ecs/module.hpp"

namespace ohao::ecs {

[[nodiscard]] const Module& audioModule();
// Its registrations (audio/types.cpp).
void registerAudioTypes(reflect::TypeRegistry& types);

} // namespace ohao::ecs
