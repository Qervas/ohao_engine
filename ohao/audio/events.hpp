#pragma once
// The audio module's events (audio/module.hpp): the channels the schedule emits into the World's log (ecs/events.hpp)
// and their payloads.
#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace ohao::ecs {

// What the schedule decided, as events of the World's log (ecs/events.hpp): a host's player plays them
// (runtime/audio_player.hpp), agents read them (events.read). Sounds and buses by the bank's names.
namespace audio_events {
inline constexpr const char* kPlay = "audio.play";              // AudioPlay
inline constexpr const char* kStop = "audio.stop";              // AudioStop
inline constexpr const char* kBus = "audio.bus";                // AudioBus
inline constexpr const char* kSnapshot = "audio.snapshot";      // AudioSnapshot
inline constexpr const char* kZone = "audio.zone";              // AudioZone
inline constexpr const char* kOcclusion = "audio.occlusion";    // AudioOcclusion
inline constexpr const char* kStopWorld = "audio.stop_world";   // AudioStopWorld
} // namespace audio_events

enum class AudioOutcome : uint8_t { Played, Stole, Dropped };

// A voice starts (or, dropped, does not). Its bus, priority and send are its sound's, in the bank.
struct AudioPlay {
    std::string sound;            // the bank's sound
    uint32_t variant{0};          // the device takes it modulo its files
    uint32_t voice{0};
    std::optional<glm::vec3> pos; // where, on the sound's shape; none: at the ear
    float gain{1.0f};             // the bank's volume times the event's scale
    uint32_t walls{0};            // its occlusion at the start: walls (2: 2 or more), dB, low-pass (0: none)
    float occlusionDb{0.0f};
    float lowPassHz{0.0f};
    AudioOutcome outcome{AudioOutcome::Played};
    uint32_t victim{0};           // stole: the voice it took (the last, when it took more)
};
// A voice stops: a steal's 2 ms fade.
struct AudioStop {
    uint32_t voice{0};
};
// A bus's duck: its level from the deepest duck holding it.
struct AudioBus {
    std::string bus;
    float db{0.0f};               // 0: none
    uint32_t blendMs{0};
    bool resync{false};           // resent to bring the device to the state a restore put back
};
// The mix snapshot.
struct AudioSnapshot {
    std::string snapshot;
    uint32_t blendMs{0};
    bool resync{false};
};
// The listener's reverb zone.
struct AudioZone {
    std::string zone;             // the preset ("" with no reverb at all)
    std::string bed;              // its bed sound (may be empty)
    uint32_t blendMs{0};
    bool resync{false};
};
// A held voice's occlusion, re-cast.
struct AudioOcclusion {
    uint32_t voice{0};
    uint32_t walls{0};
    float occlusionDb{0.0f};
    float lowPassHz{0.0f};
    uint32_t blendMs{0};
};
// The world's voices fade: a restore (a round's restart too) put back another moment.
struct AudioStopWorld {
    uint32_t blendMs{0};
};

} // namespace ohao::ecs
