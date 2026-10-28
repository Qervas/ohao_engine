#pragma once
// The audio schedule (sub-project 11; docs/specs/2026-10-28-audio-design.md, "The step: the
// schedule"): what the world sounds like, decided in the step from the step's state, as commands in a
// log that any host reads (a window's player plays them, runtime/audio_player.hpp; agents and tests
// compare them). The `audio_schedule` simulate system is the step's last (installAudio, called after
// installAnimation).
//
// Each step, in this order:
//   1. A round restart in the step (or any restore since the last step) writes `stop_world`; a restore
//      also brings the device back to the restored mix (`resync` commands).
//   2. The listener's zone: the highest-priority ReverbZone holding the listener at the step (ties: the
//      lowest id), else the bank's defaultZone; a change writes `zone` (the preset, its bed, a 30-step
//      blend). A zone whose preset the bank lacks is W_AUDIO_PRESET and counts as the default.
//   3. The snapshot from the round: `dead` while the player is dead, `won` once the room is cleared,
//      else `default`; a change writes `snapshot` with the blend of the one entered (`inSteps`), or of
//      the one left (`outSteps`) on the way back to `default`.
//   4. The sounds the step's events name (physics footsteps and landings, combat, animation clip
//      events), by the rules Feel's runtime players had (combatSoundNames, animSoundNames, footsteps by
//      surface, round robin per sound), each placed at the ear or at its point on its curve, then
//      admitted against its bus's cap and the bank's global `voices`: a lower priority is stolen first,
//      then the quieter at the listener, then the older; with no victim the new one is `dropped`.
//      Enemies' footsteps come from their clip events (foot_l, foot_r); physics' footsteps stay for the
//      player and for characters whose graph has none.
//   5. Ducks: a `fire` (or any duck's sound) holds its buses down while it holds its voice, the
//      deepest duck on a bus winning; a change of a bus's target writes `bus`.
//   6. Graded occlusion re-casts: a voice holding past 6 steps is re-cast every 6 steps; a change of
//      its walls writes `occlusion` (none; 1 wall -8 dB and 1.2 kHz; 2 or more -14 dB and 700 Hz). At
//      most 16 rays a step: past the cap a re-cast keeps its last answer and a new voice counts as
//      unoccluded, both counted, so the cap changes parameters, never which commands exist.
// The director's state (voices, round robin, ducks, zone, snapshot, its step count) rides in the
// World's SnapshotHook beside physics': a restore or a round restart resumes the same schedule. It is
// never saved or hashed. The log is not snapshotted: a restore writes new seqs. Plain data only: no
// miniaudio, no reflection; the bank comes from ohao_audio_bank.
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
struct CombatEvent;
struct AnimEvent;
struct PhysicsEvent;

constexpr uint32_t kRaysPerStep = 16;         // occlusion rays a step
constexpr uint32_t kRecastSteps = 6;          // a holding voice is re-cast this often
constexpr uint32_t kZoneBlendSteps = 30;      // a zone change's crossfade
constexpr uint32_t kOcclusionBlendSteps = 3;  // a re-cast's ramp in the device
constexpr uint32_t kStopWorldSteps = 3;       // a restart's fade of the world's voices
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

struct AudioCommand {
    enum class Op : uint8_t { Play, Stop, Bus, Snapshot, Zone, Occlusion, StopWorld };
    enum class Outcome : uint8_t { Played, Stole, Dropped };
    Op op{Op::Play};
    uint64_t seq{0};          // position in the log
    uint64_t step{0};         // the schedule's step it was written in (1 = the first; a restore puts it back)
    // Play: the bank sound and variant (the device takes it modulo its files), its bus and priority;
    // at the ear (not spatial) or at `pos` on the sound's shape; gain: the bank's volume times the
    // event's scale; send: the sound's; occl: its walls at the start; outcome; victim: the voice it
    // took (the last, when it took more). source: whose sound (an entity id; 0: none).
    uint16_t sound{0};
    uint32_t variant{0};
    uint8_t bus{0}, priority{0};
    bool spatial{false};
    glm::vec3 pos{0.0f};
    float gain{1.0f}, send{0.0f};
    Outcome outcome{Outcome::Played};
    uint32_t voice{0};        // Play, Stop, Occlusion: the voice
    uint32_t victim{0};
    uint64_t source{0};
    float db{0.0f};           // Bus: the duck's level (0: none)
    std::string zone, bed;    // Zone: the preset ("" with no reverb at all) and its bed sound (may be empty)
    std::string snapshot;     // Snapshot: its name
    uint32_t blendSteps{0};   // Zone, Snapshot, Bus, Occlusion, StopWorld: the ramp in steps
    Occlusion occl;           // Play, Occlusion
    bool resync{false};       // written to bring the device back to the state a restore put back
};

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

// A voice the schedule holds now (tests, audio.info later).
struct ScheduledVoice {
    uint32_t id{0};
    uint64_t seq{0};          // its play command's
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
[[nodiscard]] const char* audioCommandOpName(AudioCommand::Op op) noexcept;
[[nodiscard]] const char* audioOutcomeName(AudioCommand::Outcome o) noexcept;
// One command as a line, every field but seq (sound and bus by name from `bank`): what tests and the
// `audio` log channel (Debug) compare between hosts.
[[nodiscard]] std::string describeAudioCommand(const AudioCommand& c, const AudioBank& bank);

// ---- the rules (Feel's runtime players', sub-projects 6, 7, 9, 10; moved here unchanged) -----------
// The sounds a combat event names: the player's round "fire", anyone else's "enemy_fire"; a hit
// "impact_<surface>"; damage to the player "hurt"; a death "death"; the player's reload "reload",
// after "empty" when the magazine ran dry. Nothing else.
[[nodiscard]] std::vector<std::string> combatSoundNames(const CombatEvent& e, uint64_t playerId);
// The player's own combat sounds (at the ear): fire, hurt, reload, empty.
[[nodiscard]] bool combatSoundAtEar(std::string_view name) noexcept;
// An animation event's: mag_out, mag_in, bolt and body_fall play their own name; foot_l and foot_r a
// footstep ("footstep"; the surface is the character's ground's); nothing else.
[[nodiscard]] std::vector<std::string> animSoundNames(const AnimEvent& e);
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

    // Starts a sound now through the rules' path (placement, occlusion, caps, ducks); returns the
    // command written (outcome Dropped included), or nothing for a sound the bank lacks.
    std::optional<AudioCommand> play(const SoundRequest& request);

    struct Voice {
        uint32_t id{0};
        Occlusion occl;       // at its start
        bool capped{false};   // over the step's ray cap: counted as unoccluded
    };
    // A tracked voice without a play command (step 2's hook, its tests): its ray and its re-casts.
    Voice trackVoice(glm::vec3 source, uint32_t holdMs, bool occlude);

    // Logs every command written, at Info on the `audio` channel ("schedule: " + describeAudioCommand):
    // hosts turn it on for OHAO_AUDIO_TRACE=1, so check_live compares a window's schedule with a
    // headless host's through log.read.
    void setTrace(bool on) noexcept;

    // The bank to use instead of the AudioMix's (tests, benches); an AudioMix change reads its file again.
    void setBank(const AudioBank& bank);
    [[nodiscard]] const AudioBank& bank() const noexcept;
    [[nodiscard]] uint64_t bankVersion() const noexcept;   // changes when the bank does

    [[nodiscard]] std::span<const AudioCommand> stepCommands() const noexcept;
    // Commands with seq >= since, oldest first, at most max; `dropped`: how many the log of 4,096 no
    // longer holds.
    [[nodiscard]] std::vector<AudioCommand> commandsSince(uint64_t since, std::size_t max, uint64_t& dropped) const;
    [[nodiscard]] uint64_t nextSeq() const noexcept;
    [[nodiscard]] uint64_t steps() const noexcept;
    [[nodiscard]] const std::string& zone() const noexcept;       // the listener's zone's preset now
    [[nodiscard]] const std::string& snapshot() const noexcept;   // the snapshot now
    [[nodiscard]] AudioScheduleStats stats() const noexcept;
    [[nodiscard]] std::vector<ScheduledVoice> voices() const;     // in id order
    [[nodiscard]] const std::vector<AudioWarning>& warnings() const noexcept;
    // The preset of the zone holding `p` ("" in none), fitted zones included.
    [[nodiscard]] std::string zoneAt(glm::vec3 p) const;

    // The director's state a snapshot carries (installAudio chains them into the SnapshotHook).
    [[nodiscard]] std::shared_ptr<const void> saveState() const;
    void restoreState(const std::shared_ptr<const void>& state);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Installs the `audio_schedule` simulate system (after installAnimation, so it is the step's last) and
// chains its state into the World's SnapshotHook (installPhysics' must be there already). Returns the
// installed one if there is one.
AudioSchedule& installAudio(World& world, AssetResolver& assets);
[[nodiscard]] AudioSchedule* audioSchedule(World& world);
[[nodiscard]] const AudioSchedule* audioSchedule(const World& world);
// The latest step's commands (empty without the schedule).
[[nodiscard]] std::span<const AudioCommand> audioCommands(const World& world);

} // namespace ohao::ecs
