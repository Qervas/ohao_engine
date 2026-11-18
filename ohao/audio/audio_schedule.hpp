#pragma once
// The audio schedule (sub-project 11; docs/specs/2026-10-28-audio-design.md, "The step: the
// schedule"): what the world sounds like, decided in the step from the step's state, as the `audio.*`
// events of the World's log (ecs/events.hpp) that any host reads (a window's player plays them,
// runtime/audio_player.hpp; agents compare them through events.read). The `audio_schedule` simulate system (the
// audio module's, audio/module.hpp) runs after the writers of the events it reads.
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
//   4. The sounds the step's events name: what the sound sources other modules add ask for first (footsteps, its
//      own module: SoundSource::beforeCues), a game's events by the bank's `events` (their cues, in the order they
//      happened), then animation clip events, each by the first source that takes it (SoundSource::clip), else by
//      animSoundNames; round robin per sound; each placed at the ear or at its point on its curve, then
//      admitted against its bus's cap and the bank's global `voices`: a lower priority is stolen first,
//      then the quieter at the listener, then the older; with no victim the new one is `dropped`.
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
#include "audio/events.hpp"
#include "ecs/events.hpp"
#include "ecs/snapshot_hook.hpp"

#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
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
class SystemContext;
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
// An animation event's no source took: mag_out, mag_in, bolt and body_fall play their own name; nothing else.
[[nodiscard]] std::vector<std::string> animSoundNames(const ClipEvent& e);
// Whose an animated entity's sounds are: the player's when it is the player or below it (the arms).
[[nodiscard]] uint64_t animSoundOwner(const World& world, uint64_t entityId, uint64_t playerId);
// miniaudio's inverse distance model on a shape: full within min, min / (min + rolloff (d - min)) to
// max, then held.
[[nodiscard]] float distanceGain(float distance, const ShapeDesc& shape) noexcept;

// What a sound source is given in the schedule's step: the World, the registry as the schedule reads it, the bank, the
// listener's player, the schedule's helpers, and play(): a request admitted as the schedule's own are, in call order.
class SoundStep {
public:
    World& world;
    const SystemContext& reg;
    const AudioBank& bank;
    uint64_t playerId{0};   // the listener's player (0: none)

    void play(const SoundRequest& request);
    // Whose an animated entity's sounds are (animSoundOwner, through reg).
    [[nodiscard]] uint64_t ownerOf(uint64_t entityId) const;
    // The entity's world matrix at the step (its WorldTransform, else its Transform).
    [[nodiscard]] glm::mat4 worldOf(entt::entity e) const;
    // The character a body belongs to (the entity or an ancestor holding a PlayerController), or null.
    [[nodiscard]] entt::entity characterOf(entt::entity e) const;

private:
    friend class AudioSchedule;
    SoundStep(World& w, const SystemContext& r, const AudioBank& b, uint64_t player, void* impl)
        : world(w), reg(r), bank(b), playerId(player), m_impl(impl) {}
    void* m_impl;
};

// Sounds another module adds to the schedule's step (footsteps, its own module), by name. Its reads join the schedule's
// declaration (World::addAccess) and its readers are the schedule's (the system's name, "audio_schedule").
struct SoundSource {
    std::string name;
    // The step's part 4, before a game's cues.
    std::function<void(SoundStep&)> beforeCues;
    // A clip event's sounds, before animSoundNames: true when it took the event.
    std::function<bool(SoundStep&, const ClipEvent&)> clip;
};

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

    // Sources of sounds, in the order added (a name again replaces it): the audio module's contributions.
    void addSource(SoundSource source);
    void removeSource(std::string_view name);
    [[nodiscard]] std::vector<std::string> sourceNames() const;

    // The director's state a snapshot carries (the module's snapshot hook).
    [[nodiscard]] std::shared_ptr<const void> saveState() const;
    void restoreState(const std::shared_ptr<const void>& state);
    void hashState(StateHash& h) const;   // what saveState keeps (world.hash)

private:
    friend AudioSchedule& installAudio(World& world, AssetResolver& assets);
    friend class SoundStep;
    struct Impl;
    std::unique_ptr<Impl> m;
};

// The World's schedule (null where the audio module is not installed).
[[nodiscard]] AudioSchedule* audioSchedule(World& world);
[[nodiscard]] const AudioSchedule* audioSchedule(const World& world);

} // namespace ohao::ecs
