#pragma once
// The animation module's events (animation/module.hpp): the channel the `animation` system emits into the World's log
// (ecs/events.hpp) and its payload.
#include <cstdint>
#include <string>

namespace ohao::ecs {

// A clip event a layer's clip fired in the step (`anim.clip_event`): mag_out, foot_l, body_fall, ...
inline constexpr const char* kClipEventChannel = "anim.clip_event";
struct ClipEvent {
    uint64_t entity{0};     // the animated entity's id
    uint32_t layer{0};      // the graph layer
    std::string clip;       // the clip that fired it
    std::string name;       // the event's name in the clip
};

} // namespace ohao::ecs
