#pragma once
// The audio player (sub-project 11 step 3; docs/specs/2026-10-28-audio-design.md, "The frame: the
// player"): a window host's bridge from the voice schedule (ecs/audio_schedule.hpp, decided in the
// step) to the device. Once a frame, after the frame's steps, it reads the schedule's `audio.*` events
// since its cursor (the World's log) and drives AudioSystem. Headless hosts have none: the events are what
// agents read (events.read).
//
// The clock: the player anchors a World step s0 to an engine frame f0 and plays step s's commands at
// f0 + (s - s0) x rate x dt + one step (the horizon, OPEN ruling 7; dt the World's), so two steps run in one frame
// sound a step apart. A command whose time has passed starts at once and counts as late. The anchor is
// taken again when the timeline is not running in real time (paused or lockstep: each command at once
// plus the horizon), when the schedule's step goes back (a restore, a round restart), and when the
// anchor drifts more than a step from the device clock.
//
// The commands: play (the bank's preloaded sound, on its bus, at its position on its shape, through its
// occlusion's low-pass; a dropped one plays nothing), stop (a steal's 2 ms fade), bus (a duck: the
// bus's level is the snapshot's plus the deepest duck's), snapshot (every bus's level and low-pass,
// ramped over the blend), zone (the reverb's preset crossfaded, the preset's bed looped on its bus and
// crossfaded over the same blend), occlusion (a voice's level ramped, its low-pass moved), stop_world
// (every voice on the world's buses faded over 3 steps). A bank sound without a file in any folder is
// skipped with one warning per name. Nothing here opens a file after the bank loads.
#include "audio/audio_system.hpp"
#include "ecs/audio_schedule.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace ohao::ecs {
class World;
}

namespace ohao::runtime {

// audio.mix's session overrides (sub-project 11 step 4): per bus a level (replacing the game's), mute and
// solo; a snapshot forced in place of the schedule's. Device only: the schedule never sees them. A bus
// the bank lacks is ignored.
struct MixOverrides {
    struct Bus {
        std::string name;
        std::optional<float> db;
        bool mute{false}, solo{false};
    };
    std::vector<Bus> buses;
    std::string snapshot;   // "": the schedule's
};

// A bus as the device has it now (audio.info's device half).
struct DeviceBus {
    float db{0.0f};          // driven to: the game's (snapshot and duck) or an override's
    bool muted{false}, soloed{false};
    uint32_t voices{0};      // the schedule's voices on it the device is playing
};

class AudioPlayer {
public:
    // Plays through `audio` (the host's; it must outlive this). A bank's sounds are decoded from files
    // under `root` when it is loaded (useBank; update loads the schedule's whenever it changes).
    void initialize(const std::filesystem::path& root, AudioSystem& audio);
    [[nodiscard]] bool ready() const noexcept { return m_audio != nullptr && m_audio->isInitialized(); }

    // Once a frame, after the frame's steps: the schedule's bank (when it changed), the listener from
    // the frame's camera, then the schedule's `audio.*` events since the cursor (the World's log).
    // `realtime`: the timeline runs in real time (false: paused or lockstep). The first call starts at the
    // log's end.
    void update(const ecs::World& world, bool realtime);

    // The bank to play (decoded into the device now; every playing sound stops).
    void useBank(const AudioBank& bank);

    [[nodiscard]] uint64_t late() const noexcept { return m_late; }        // commands started after their time
    [[nodiscard]] uint64_t played() const noexcept { return m_played; }    // voices started
    [[nodiscard]] std::size_t missing() const noexcept { return m_missing.size(); }   // sounds without a file, warned once
    [[nodiscard]] uint64_t anchors() const noexcept { return m_anchors; }  // times the clock was anchored
    [[nodiscard]] std::optional<SoundHandle> handleOf(uint32_t voice) const;
    // The level the game drives a bus to (dB: its snapshot's plus its deepest duck's; 0 for an unknown
    // bus), overrides aside.
    [[nodiscard]] float busLevelDb(std::size_t bus) const noexcept {
        return bus < m_snapshotDb.size() ? m_snapshotDb[bus] + m_duckDb[bus] : 0.0f;
    }

    // audio.mix: replaces the overrides (applied now, over 2 ms, and again whenever a bank loads).
    void setOverrides(MixOverrides mix);
    [[nodiscard]] const MixOverrides& overrides() const noexcept { return m_mix; }
    // Every bus of the bank as the device has it (empty before a bank).
    [[nodiscard]] std::vector<DeviceBus> deviceBuses() const;
    [[nodiscard]] const AudioBank* bank() const noexcept { return m_hasBank ? &m_bank : nullptr; }
    [[nodiscard]] uint32_t horizonFrames() const noexcept;
    [[nodiscard]] uint32_t stepFrames() const noexcept;   // one step of the World's dt
    [[nodiscard]] uint32_t msFrames(uint32_t ms) const noexcept;

private:
    [[nodiscard]] uint32_t rate() const noexcept;
    void anchor(uint64_t step);
    [[nodiscard]] uint64_t startFrameOf(uint64_t step);
    void clock(uint64_t step, bool realtime);   // the anchor, at the World's step
    void play(const ecs::AudioPlay& c, uint64_t step);
    void snapshot(const ecs::AudioSnapshot& c);
    void stopWorld(uint32_t blendFrames);
    void applyBuses(uint32_t rampFrames);
    void applyOverrides(uint32_t rampFrames);   // the overrides into per-bus vectors, levels, mutes, solos
    [[nodiscard]] float levelDb(std::size_t bus) const noexcept;
    [[nodiscard]] double levelHz(std::size_t bus) const noexcept;
    void zone(const ecs::AudioZone& c);

    AudioSystem* m_audio{nullptr};
    std::filesystem::path m_root;
    AudioBank m_bank;
    bool m_hasBank{false};
    uint64_t m_bankVersion{~0ull};

    uint64_t m_cursor{0};
    bool m_started{false};
    bool m_anchored{false};
    uint64_t m_s0{0}, m_f0{0};
    uint64_t m_lastStep{0};
    float m_stepDt{1.0f / 60.0f};   // the World's, at the last update

    std::map<uint32_t, SoundHandle> m_voices;       // schedule voice -> device handle
    std::map<uint32_t, uint8_t> m_voiceBus;         // schedule voice -> its bus
    std::vector<float> m_snapshotDb, m_duckDb;      // per bus: the snapshot's level and the duck's
    std::vector<double> m_snapshotHz;               // per bus: the snapshot's low-pass (0: none)
    std::string m_snapshotName, m_zoneName;         // what the device is in now
    MixOverrides m_mix;
    std::vector<std::optional<float>> m_overrideDb;   // per bus, from m_mix
    std::vector<uint8_t> m_muted, m_soloed;           // per bus, from m_mix
    std::vector<float> m_forcedDb;                    // per bus: the forced snapshot's (empty: none)
    std::vector<double> m_forcedHz;
    SoundHandle m_bed{INVALID_SOUND_HANDLE};
    std::string m_bedName;

    uint64_t m_late{0}, m_played{0}, m_anchors{0};
    std::set<std::string> m_missing;
};

} // namespace ohao::runtime
