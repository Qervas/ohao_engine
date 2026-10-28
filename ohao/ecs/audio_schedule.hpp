#pragma once
// The audio schedule (sub-project 11; docs/specs/2026-10-28-audio-design.md, "The step: the
// schedule"): what the world sounds like, decided in the step from the step's state, as commands in a
// log that any host reads (a window's player plays them; agents and tests compare them). The
// `audio_schedule` simulate system is the step's last (installAudio, called after installAnimation).
//
// Step 2 builds two of its parts:
//   - Reverb zones. The listener's zone is the highest-priority ReverbZone whose box holds the
//     listener at the step (ties: the lowest id), else the bank's defaultZone; a change writes a
//     `zone` command (the preset, its bed, a 30-step blend). A zone whose preset the world's bank
//     lacks is W_AUDIO_PRESET and counts as the default.
//   - Graded occlusion. One ray from the listener toward a source, through characters (at most 4
//     hops), counting the walls it passes: none; 1 wall -8 dB and 1.2 kHz; 2 or more -14 dB and
//     700 Hz (OPEN ruling 12). A voice that holds on past 6 steps is re-cast every 6 steps and an
//     `occlusion` command written when its grade changes (the device ramps over 3 steps). At most 16
//     rays a step: past the cap a re-cast keeps its last answer and a new voice counts as unoccluded,
//     both counted, so the cap changes parameters, never which commands exist.
// Plays, priorities, ducks, snapshots and the restart are step 3's (trackVoice is the hook its plays
// use). Plain data only: no miniaudio, no reflection; the bank comes from ohao_audio_bank.
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

namespace ohao::ecs {

class World;
class AssetResolver;

constexpr uint32_t kRaysPerStep = 16;         // occlusion rays a step
constexpr uint32_t kRecastSteps = 6;          // a holding voice is re-cast this often
constexpr uint32_t kZoneBlendSteps = 30;      // a zone change's crossfade
constexpr uint32_t kOcclusionBlendSteps = 3;  // a re-cast's ramp in the device

// Who hears at the step: the input-driven player's head at the step's pose (its body skipped by the
// rays), else the first active camera.
struct AudioListener {
    glm::vec3 position{0.0f};
    entt::entity body{entt::null};
};

// How much the walls between the listener and a source take: their count (2 means 2 or more), the
// cut in dB and the low-pass (0: none).
struct Occlusion {
    uint8_t walls{0};
    float db{0.0f};
    double lowPassHz{0.0};
};

struct AudioCommand {
    enum class Op : uint8_t { Zone, Occlusion };
    Op op{Op::Zone};
    uint64_t seq{0};          // position in the log
    uint64_t step{0};         // the schedule's step it was written in (1 = the first)
    std::string zone, bed;    // Zone: the preset ("" with no reverb at all) and its bed (may be empty)
    uint32_t blendSteps{0};   // Zone: the crossfade; Occlusion: the ramp
    uint32_t voice{0};        // Occlusion: the voice re-cast
    Occlusion occl;           // Occlusion: its new grade
};

struct AudioScheduleStats {
    uint64_t raysCast{0};     // occlusion rays cast, ever
    uint64_t raysCapped{0};   // casts the per-step cap answered from the last result (or as unoccluded)
    uint32_t voices{0};       // voices tracked now
};

struct AudioWarning {
    std::string code;   // W_AUDIO_PRESET, W_AUDIO_BANK, W_AUDIO_ZONE
    entt::entity entity{entt::null};
    std::string message;
};

[[nodiscard]] std::optional<AudioListener> listenerAtStep(const World& world);
// The walls one ray from the listener toward `source` passes (none without a physics scene).
[[nodiscard]] Occlusion occlusionBetween(const World& world, const AudioListener& listener, glm::vec3 source);
// The preset of the zone holding `listener` ("" in none). A zone fitted to its mesh is known only to
// an installed schedule; without one, only zones with a size count.
[[nodiscard]] std::string listenerZone(const World& world, glm::vec3 listener);
[[nodiscard]] const char* audioCommandOpName(AudioCommand::Op op) noexcept;   // "zone", "occlusion"

class AudioSchedule {
public:
    AudioSchedule(World& world, AssetResolver& assets);
    ~AudioSchedule();
    AudioSchedule(const AudioSchedule&) = delete;
    AudioSchedule& operator=(const AudioSchedule&) = delete;

    // One step (the `audio_schedule` simulate system).
    void step();

    struct Voice {
        uint32_t id{0};
        Occlusion occl;       // at its start
        bool capped{false};   // over the step's ray cap: counted as unoccluded
    };
    // A sound starting now at `source`, holding its voice `holdMs`; `occlude`: it gets a ray (and
    // re-casts while it holds). Its rays count against the current step's cap.
    Voice trackVoice(glm::vec3 source, uint32_t holdMs, bool occlude);

    [[nodiscard]] std::span<const AudioCommand> stepCommands() const noexcept;
    // Commands with seq >= since, oldest first, at most max; `dropped`: how many the log of 4,096 no
    // longer holds.
    [[nodiscard]] std::vector<AudioCommand> commandsSince(uint64_t since, std::size_t max, uint64_t& dropped) const;
    [[nodiscard]] uint64_t nextSeq() const noexcept;
    [[nodiscard]] uint64_t steps() const noexcept;
    [[nodiscard]] const std::string& zone() const noexcept;   // the listener's zone's preset now
    [[nodiscard]] AudioScheduleStats stats() const noexcept;
    [[nodiscard]] const std::vector<AudioWarning>& warnings() const noexcept;
    // The preset of the zone holding `p` ("" in none), fitted zones included.
    [[nodiscard]] std::string zoneAt(glm::vec3 p) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Installs the `audio_schedule` simulate system (after installAnimation, so it is the step's last).
// Returns the installed one if there is one.
AudioSchedule& installAudio(World& world, AssetResolver& assets);
[[nodiscard]] AudioSchedule* audioSchedule(World& world);
[[nodiscard]] const AudioSchedule* audioSchedule(const World& world);
// The latest step's commands (empty without the schedule).
[[nodiscard]] std::span<const AudioCommand> audioCommands(const World& world);

} // namespace ohao::ecs
