#pragma once
// The sound bank and mix of a game, as data (sub-project 11, docs/specs/2026-10-28-audio-design.md,
// "Data: the bank and the mix"): one `.audio.json` per game, named by the saved singleton AudioMix.
// It declares the buses (a tree under one root, each with a voice cap; `return` buses take sends),
// the sounds (files, bus, priority, distance shape, how long a sound holds its voice, gain, reverb
// send), the distance shapes, the ducking rules, the snapshots and the reverb presets. Parsed and
// validated here (every error with its JSON pointer); no miniaudio, no ECS.
//
// Sounds are names; files are presentation. `files` is a pattern under each folder in turn,
// "weapons/fire" or "footsteps/concrete_{n}" ({n} counts from 1 while a file exists; ".wav" unless
// the pattern names an extension). The first folder holding any variant of a sound supplies all of
// them (a local recording folder first, then the committed set). `holdMs` is declared, never read
// from a file, so which file a machine has never changes what the schedule decides (OPEN ruling 13).
#include "core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ohao {

struct BusDesc {
    std::string name;
    std::string parent;     // empty: the root (exactly one)
    uint32_t cap{0};        // voices at once on this bus (0: no cap of its own)
    bool isReturn{false};   // takes sends (the reverb); no sound plays on it directly
};

struct ShapeDesc {
    std::string name;
    float minDistance{1.0f};
    float maxDistance{50.0f};
    float rolloff{1.0f};
};

struct SoundDesc {
    std::string name;
    std::string files;           // the pattern under each folder
    std::string bus;
    uint8_t priority{128};       // 0..255, higher wins a voice
    std::string shape{"ear"};    // "ear": at the listener, not positioned; else a shape's name
    uint32_t holdMs{250};        // how long it holds its voice for the cap
    float volume{1.0f};
    float send{0.0f};            // to the reverb, 0..1
    bool occlude{false};         // gets an occlusion ray (enemies' sounds)
};

struct DuckDesc {
    std::string on;     // the sound that triggers it
    std::string bus;
    float db{0.0f};     // <= 0
    uint32_t attackSteps{1};
    uint32_t releaseSteps{15};
};

struct SnapshotBus {
    float db{0.0f};
    double lowPassHz{0.0};   // 0: none
};

struct SnapshotDesc {
    std::string name;
    std::vector<std::pair<std::string, SnapshotBus>> buses;
    uint32_t inSteps{0};
    uint32_t outSteps{0};
};

struct ReverbDesc {
    std::string name;
    float room{0.5f};
    float damp{0.5f};
    float wet{0.2f};
    std::string bed;   // a looping ambience sound's name; empty: none
};

struct AudioBank {
    std::vector<std::string> folders{"assets/audio"};   // project-relative, searched in order
    std::vector<BusDesc> buses;                         // parents before children
    uint32_t voices{32};                                // all buses together
    std::vector<ShapeDesc> shapes;
    std::vector<SoundDesc> sounds;
    std::vector<DuckDesc> ducks;
    std::vector<SnapshotDesc> snapshots;
    std::vector<ReverbDesc> reverb;
    std::string defaultZone;

    [[nodiscard]] int bus(std::string_view name) const;
    [[nodiscard]] int sound(std::string_view name) const;
    [[nodiscard]] const ShapeDesc* shape(std::string_view name) const;
    [[nodiscard]] int parentOf(int bus) const;   // -1 for the root
};

struct BankError {
    std::string path;   // JSON pointer into the file
    std::string message;
};

[[nodiscard]] Result<AudioBank, std::vector<BankError>> parseAudioBank(std::string_view json);

// The mixer every host starts with, Feel's (sub-project 9): master over sfx (24 voices), music and
// ambient; no sounds. The category API (SoundCategory) plays into these three buses.
[[nodiscard]] AudioBank defaultBank();

// A sound's variant files under `root`: the first of the bank's folders holding any of them.
[[nodiscard]] std::vector<std::filesystem::path> resolveSoundFiles(const AudioBank& bank, const SoundDesc& sound,
                                                                   const std::filesystem::path& root);

// The `.audio.json` format as a JSON Schema (`ohao schema audio-bank`).
[[nodiscard]] std::string audioBankJsonSchema();

} // namespace ohao
