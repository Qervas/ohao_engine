#pragma once
// The audio schedule (sub-project 11; docs/specs/2026-10-28-audio-design.md, "The step: the
// schedule"): what the world sounds like, decided in the step from the step's state, as the `audio.*`
// events of the World's log (ecs/events.hpp) that any host reads (a window's player plays them,
// runtime/audio_player.hpp; agents compare them through events.read). The `audio_schedule` simulate system
// is the step's last (installAudio, called after installAnimation).
//
// Each step, in this order:
//   1. Any restore since the last step (a round's restart too) writes `stop_world` and brings the device
//      back to the restored mix (`resync` commands).
//   2. The listener's zone: the highest-priority ReverbZone holding the listener at the step (ties: the
//      lowest id), else the bank's defaultZone; a change writes `zone` (the preset, its bed, a 500 ms
//      blend). A zone whose preset the bank lacks is W_AUDIO_PRESET and counts as the default.
//   3. The snapshot the game asks for (requestMixSnapshot; the slice: `dead` while the player is dead,
//      `won` once the room is cleared), else `default`; a change writes `snapshot` with the blend of the
//      one entered (`inMs`), or of the one left (`outMs`) on the way back to `default`.
//   4. The sounds the step's events name: physics footsteps and landings by surface, a game's events by
//      the bank's `events` (their cues, in the order they happened), animation clip events by
//      animSoundNames, round robin per sound; each placed at the ear or at its point on its curve, then
//      admitted against its bus's cap and the bank's global `voices`: a lower priority is stolen first,
//      then the quieter at the listener, then the older; with no victim the new one is `dropped`.
//      Enemies' footsteps come from their clip events (foot_l, foot_r); physics' footsteps stay for the
//      player and for characters whose graph has none.
//   5. Ducks: a `fire` (or any duck's sound) holds its buses down while it holds its voice, the
//      deepest duck on a bus winning; a change of a bus's target writes `bus`.
//   6. Graded occlusion re-casts: a voice holding past 100 ms is re-cast every 100 ms; a change of
//      its walls writes `occlusion` (none; 1 wall -8 dB and 1.2 kHz; 2 or more -14 dB and 700 Hz). At
//      most 16 rays a step: past the cap a re-cast keeps its last answer and a new voice counts as
//      unoccluded, both counted, so the cap changes parameters, never which commands exist.
// The director's state (voices, round robin, ducks, zone, snapshot, its step count) rides in the
// World's snapshot hooks: a restore or a round restart resumes the same schedule, and world.hash covers it. It is
// never saved to a file. Its events are history: a restore writes new ones. Plain data only: no
// miniaudio, no reflection; the bank comes from ohao_audio_bank.
#include "ecs/events.hpp"
#include "ecs/snapshot_hook.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ohao {
struct AudioBank;
struct ShapeDesc;
}

namespace ohao::ecs {

class World;
class AssetResolver;
struct ClipEvent;

constexpr uint32_t kRaysPerStep = 16;         // occlusion rays a step
constexpr uint32_t kRecastMs = 100;           // a holding voice is re-cast this often (in steps of the World's dt)
constexpr uint32_t kZoneBlendMs = 500;        // a zone change's crossfade
constexpr uint32_t kOcclusionBlendMs = 50;    // a re-cast's ramp in the device
constexpr uint32_t kStopWorldMs = 50;         // a restore's fade of the world's voices
constexpr uint32_t kStealFadeFrames = 96;     // 2 ms at 48 kHz: a stolen voice's fade

// Who hears at the step: the input-driven player's head at the step's pose (its body skipped by the
// rays), else the first active camera.
struct AudioListener {
    glm::vec3 position{0.0f};
    entt::entity body{entt::null};
    uint64_t playerId{0};   // the input-driven player's id (0: none): its sounds are its own
};

// How much the walls between the listener and a source take: their count (2 means 2 or more), the
// cut in dB and the low-pass (0: none).
struct Occlusion {
    uint8_t walls{0};
    float db{0.0f};
    double lowPassHz{0.0};
};

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

// The mix snapshot a game asks for (requestMixSnapshot; "default" until one asks).
struct MixRequest {
    std::string snapshot{"default"};
};
// A game asks for a mix snapshot (the slice: `dead` while the player is, `won` once the room is cleared): the
// schedule moves to it at its next step, if the bank has it. The request holds until another.
void requestMixSnapshot(World& world, std::string_view snapshot);

struct AudioScheduleStats {
    uint64_t raysCast{0};     // occlusion rays cast, ever
    uint64_t raysCapped{0};   // casts the per-step cap answered from the last result (or as unoccluded)
    uint32_t voices{0};       // voices holding now
    uint64_t played{0}, stole{0}, dropped{0};   // play commands by outcome, ever
};

struct AudioWarning {
    std::string code;   // W_AUDIO_PRESET, W_AUDIO_BANK
    entt::entity entity{entt::null};
    std::string message;
};

// A voice the schedule holds now (audio.info).
struct ScheduledVoice {
    uint32_t id{0};
    uint64_t seq{0};          // its play event's
    uint16_t sound{0};
    uint8_t bus{0}, priority{0};
    float audible{0.0f};      // its gain at the listener when it started (distance and occlusion)
    uint64_t start{0}, end{0};   // steps
};

// A sound for the schedule to start (what the rules make of an event; tests ask for them directly).
struct SoundRequest {
    std::string sound;          // the bank's name
    uint64_t owner{0};          // whose: the player's own are never occluded
    glm::vec3 at{0.0f};         // where (unused at the ear)
    bool ear{false};            // at the listener, not positioned (also when the bank's shape is "ear")
    float scale{1.0f};          // times the bank's volume
    std::optional<uint32_t> variant;   // none: the sound's next in its round robin
};

[[nodiscard]] std::optional<AudioListener> listenerAtStep(const World& world);
// The walls one ray from the listener toward `source` passes (none without a physics scene).
[[nodiscard]] Occlusion occlusionBetween(const World& world, const AudioListener& listener, glm::vec3 source);
// The preset of the zone holding `listener` ("" in none). A zone fitted to its mesh is known only to
// an installed schedule; without one, only zones with a size count.
[[nodiscard]] std::string listenerZone(const World& world, glm::vec3 listener);

// ---- the rules (Feel's runtime players', sub-projects 6, 7, 9, 10; moved here unchanged) -----------
// A game's events play by the bank's `events` (audio/audio_bank.hpp, EventSound): the cue's channel, its source
// (the listener's player or not) and tag choose the sounds.
// An animation event's: mag_out, mag_in, bolt and body_fall play their own name; foot_l and foot_r a
// footstep ("footstep"; the surface is the character's ground's); nothing else.
[[nodiscard]] std::vector<std::string> animSoundNames(const ClipEvent& e);
// Whose an animated entity's sounds are: the player's when it is the player or below it (the arms).
[[nodiscard]] uint64_t animSoundOwner(const World& world, uint64_t entityId, uint64_t playerId);
// A footstep's or landing's gain by its speed: a walk (4 m/s) 0.65, a sprint (6.5) 0.9; a landing by
// its impact.
[[nodiscard]] float footstepScale(bool landing, float speed) noexcept;
// The bank sound for a footstep on `surface`: "footstep_<surface>", else "footstep_default" ("" if
// the bank has neither).
[[nodiscard]] std::string footstepSound(const AudioBank& bank, std::string_view surface);
// miniaudio's inverse distance model on a shape: full within min, min / (min + rolloff (d - min)) to
// max, then held.
[[nodiscard]] float distanceGain(float distance, const ShapeDesc& shape) noexcept;

class AudioSchedule {
public:
    AudioSchedule(World& world, AssetResolver& assets);
    ~AudioSchedule();
    AudioSchedule(const AudioSchedule&) = delete;
    AudioSchedule& operator=(const AudioSchedule&) = delete;

    // One step (the `audio_schedule` simulate system).
    void step();

    // The bank to use instead of the AudioMix's (tests, benches); an AudioMix change reads its file again.
    void setBank(const AudioBank& bank);
    [[nodiscard]] const AudioBank& bank() const noexcept;
    [[nodiscard]] uint64_t bankVersion() const noexcept;   // changes when the bank does

    [[nodiscard]] uint64_t steps() const noexcept;
    [[nodiscard]] const std::string& zone() const noexcept;       // the listener's zone's preset now
    [[nodiscard]] const std::string& snapshot() const noexcept;   // the snapshot now
    [[nodiscard]] AudioScheduleStats stats() const noexcept;
    [[nodiscard]] float busDuckDb(std::size_t bus) const noexcept;   // a bus's duck level now (dB; 0: none)
    [[nodiscard]] uint32_t snapshotBlendMs() const noexcept;         // the last snapshot command's blend
    [[nodiscard]] double lastStepMs() const noexcept;             // the last step's time in this system
    [[nodiscard]] std::vector<ScheduledVoice> voices() const;     // in id order
    [[nodiscard]] const std::vector<AudioWarning>& warnings() const noexcept;
    // The preset of the zone holding `p` ("" in none), fitted zones included.
    [[nodiscard]] std::string zoneAt(glm::vec3 p) const;

    // The director's state a snapshot carries (installAudio's snapshot hook).
    [[nodiscard]] std::shared_ptr<const void> saveState() const;
    void restoreState(const std::shared_ptr<const void>& state);
    void hashState(StateHash& h) const;   // what saveState keeps (world.hash)

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Installs the `audio_schedule` simulate system (the step's last: ecs::kSimulateOrder) and its snapshot
// hook. Returns the installed one if there is one.
AudioSchedule& installAudio(World& world, AssetResolver& assets);
[[nodiscard]] AudioSchedule* audioSchedule(World& world);
[[nodiscard]] const AudioSchedule* audioSchedule(const World& world);

} // namespace ohao::ecs
