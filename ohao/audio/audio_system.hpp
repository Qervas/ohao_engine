#pragma once

/**
 * AudioSystem — miniaudio facade: a 3D listener and a mixer of buses (sub-project 11 step 1,
 * docs/specs/2026-10-28-audio-design.md).
 *
 * The buses are a bank's (audio_bank.hpp): each is a sound group feeding a bus node (gain ramped
 * linearly in dB, a low-pass ramped in log frequency, a mute/solo gate, a send to the bank's return
 * bus), nested under the root, which feeds the endpoint. A fresh system has the default bank's
 * buses (master over sfx, music and ambient); loadBank replaces them with a game's and decodes its
 * sounds into memory, so playing a bank sound opens no file. The category API (SoundCategory, file
 * paths) plays into the default bank's buses and stays until the voice schedule replaces its
 * callers (step 3).
 *
 * A mixer without a device (initialize(true)) is rendered by render(): the engine's clock advances
 * only by what is read, so tests and benches render deterministic audio.
 */

#include "audio/audio_bank.hpp"
#include "core/concepts.hpp"

#include <glm/glm.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <compare>
#include <array>
#include <optional>
#include <span>
#include <vector>

// Forward declare miniaudio types to avoid including the massive header here
struct ma_engine;
struct ma_sound;

namespace ohao {
struct AudioLowPass;     // a miniaudio low-pass node (its type is anonymous: wrapped in the .cpp)
struct AudioVoiceSource; // a bank voice's data source: its start delay, its decoded data, its stop fade
}

namespace ohao {

enum class SoundCategory : int {
    SFX = 0,
    Music = 1,
    Ambient = 2,
    Count = 3
};

[[nodiscard]] constexpr int soundCategoryIndex(SoundCategory c) noexcept {
    return static_cast<int>(to_underlying(c));
}

[[nodiscard]] constexpr std::string_view soundCategoryName(SoundCategory c) noexcept {
    switch (c) {
        case SoundCategory::SFX:     return "sfx";
        case SoundCategory::Music:   return "music";
        case SoundCategory::Ambient: return "ambient";
        case SoundCategory::Count:   break;
    }
    return "unknown";
}

[[nodiscard]] constexpr bool isValidSoundCategory(SoundCategory c) noexcept {
    return c == SoundCategory::SFX || c == SoundCategory::Music || c == SoundCategory::Ambient;
}

using SoundHandle = uint32_t;
constexpr SoundHandle INVALID_SOUND_HANDLE = 0;

[[nodiscard]] constexpr bool isValidSoundHandle(SoundHandle h) noexcept {
    return h != INVALID_SOUND_HANDLE;
}

// Custom deleters defined in .cpp (need full miniaudio types)
struct MaEngineDeleter {
    void operator()(ma_engine* engine) const;
};

struct MaSoundDeleter {
    void operator()(ma_sound* sound) const;
};

struct MaLpfNodeDeleter {
    void operator()(AudioLowPass* node) const;
};

struct VoiceSourceDeleter {
    void operator()(AudioVoiceSource* source) const;
};

using MaEnginePtr = std::unique_ptr<ma_engine, MaEngineDeleter>;
using MaSoundPtr = std::unique_ptr<ma_sound, MaSoundDeleter>;
using MaLpfNodePtr = std::unique_ptr<AudioLowPass, MaLpfNodeDeleter>;
using VoiceSourcePtr = std::unique_ptr<AudioVoiceSource, VoiceSourceDeleter>;

// How a positioned sound fades with distance (miniaudio's inverse model: full volume within
// minDistance, minDistance / (minDistance + rolloff * (d - minDistance)) out to maxDistance, then
// held), and whether a wall stands between it and the listener: an occluded sound plays
// kOccludedGainDb quieter through a low-pass at kOccludedLowPassHz (sub-project 9, "Audio quick wins").
struct SoundShape {
    float minDistance{1.0f};
    float maxDistance{50.0f};
    float rolloff{1.0f};
    bool occluded{false};
};
constexpr float kOccludedGainDb = -8.0f;
constexpr double kOccludedLowPassHz = 1200.0;

// What a playing sound was started with (tests and the voice cap read it).
struct VoiceInfo {
    bool playing{false};
    bool spatial{false};
    SoundCategory category{SoundCategory::SFX};
    glm::vec3 position{0.0f};
    SoundShape shape;
    float baseVolume{1.0f};   // as asked
    float volume{1.0f};       // set on the sound: base x occlusion (category and master are its bus's)
    double lowPassHz{0.0};    // 0: none
    uint64_t startFrame{0};   // a bank voice's first frame on the engine clock (asked for, or when it began)
};

// Where and how loud a bank sound plays: at the listener (not spatial), or at a position on a
// distance shape, occluded or not. Graded occlusion (the schedule's, sub-project 11 step 2):
// occlusionLowPassHz above 0 plays the voice that much quieter through that low-pass; else
// shape.occluded is Feel's single grade (kOccludedGainDb, kOccludedLowPassHz).
struct VoicePlacement {
    bool spatial{false};
    glm::vec3 position{0.0f};
    SoundShape shape;
    float volume{1.0f};   // times the bank sound's own volume
    float occlusionDb{0.0f};
    double occlusionLowPassHz{0.0};
    bool loop{false};             // plays its data over and over (an ambience bed) until stopped
    uint32_t fadeInFrames{0};     // rises linearly from silence over these frames (a bed's crossfade)
};

class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;
    AudioSystem(AudioSystem&&) = delete;
    AudioSystem& operator=(AudioSystem&&) = delete;

    // The default playback device. withoutDevice: a mixer nobody reads (sounds start and stay
    // playing, silently), for tests on machines with or without an audio device.
    [[nodiscard]] bool initialize(bool withoutDevice = false);
    void shutdown();
    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

    // Listener (camera)
    void setListenerPosition(const glm::vec3& pos, const glm::vec3& forward, const glm::vec3& up);

    // ---- the bank and its buses (sub-project 11) -------------------------------------------
    // Replaces the buses with the bank's and decodes its sounds (each variant at the engine's rate)
    // from files under `root` (local folder first, audio_bank.hpp). Every playing sound stops. A
    // sound without a file in any folder is kept, with no variants, and warned about once.
    bool loadBank(const AudioBank& bank, const std::filesystem::path& root);
    [[nodiscard]] const AudioBank& bank() const noexcept { return m_bank; }
    [[nodiscard]] uint32_t variantCount(uint16_t sound) const;
    [[nodiscard]] double bankLoadMs() const noexcept { return m_bankLoadMs; }   // the last loadBank's

    // Plays a bank sound's variant (modulo its count) on the sound's bus, at `startFrame` on the
    // engine clock (0 or a past frame: at once): its source reads startFrame - engineFrame() frames
    // of silence first, exact to the frame without a device. Not under the category voice cap: the
    // voice schedule (step 3) decides what plays.
    [[nodiscard]] SoundHandle play(uint16_t sound, uint32_t variant, const VoicePlacement& placement,
                                   uint64_t startFrame = 0);
    // Stops a sound with a linear fade over `fadeFrames` (0: at once). A bank sound's source fades
    // and ends itself, from the next frame it reads.
    void stop(SoundHandle handle, uint32_t fadeFrames);
    // A bank voice's occlusion changed (the schedule's re-cast): its level ramps linearly to the new
    // cut over `rampFrames`, its low-pass moves to `lowPassHz` (0: open) at once.
    void setVoiceOcclusion(SoundHandle handle, float db, double lowPassHz, uint32_t rampFrames);
    // Files opened since the system started (bank loads and the category API): a bank voice opens none.
    [[nodiscard]] uint64_t filesOpened() const noexcept { return m_filesOpened; }

    // A bus's level: its gain in dB (linear in dB over `rampFrames`) and its low-pass (0: none;
    // ramped in log frequency over the same frames). From the next processed frame.
    void setBus(uint8_t bus, float db, double lowPassHz, uint32_t rampFrames);
    // Mute silences a bus's subtree; solo silences every bus that is not a soloed one, its
    // ancestor or its descendant. Both change over 2 ms (96 frames at 48 kHz).
    void setMute(uint8_t bus, bool muted);
    void setSolo(uint8_t bus, bool soloed);
    // How much of a bus goes to the bank's return bus (the reverb), 0..1.
    void setBusSend(uint8_t bus, float level);
    // The reverb on the bank's first return bus (reverb_node.hpp): crossfaded to `preset` over
    // `crossfadeFrames` (0: at once). loadBank sets the bank's defaultZone preset. Nothing without a
    // return bus.
    void setReverb(const ReverbPreset& preset, uint32_t crossfadeFrames);
    [[nodiscard]] bool hasReverb() const noexcept;

    [[nodiscard]] uint64_t engineFrame() const;   // the engine clock, in frames
    [[nodiscard]] uint32_t sampleRate() const;
    [[nodiscard]] uint32_t channels() const;
    // A mixer without a device: renders `frames` interleaved frames into `out` (at least frames x
    // channels floats), advancing the engine clock. Returns the frames rendered (0 with a device).
    uint32_t render(std::span<float> out, uint32_t frames);
    // The mix's cost: the median over the newest 512 periods (the device's data callbacks, or render()
    // calls) of one period's mixing, scaled to 10 ms of audio, in ms; nullopt before the first.
    [[nodiscard]] std::optional<double> mixMs() const;

    // Play sounds — returns INVALID_SOUND_HANDLE on failure (or when the voice cap drops it)
    [[nodiscard]] SoundHandle playSound(std::string_view path,
                                        SoundCategory category = SoundCategory::SFX,
                                        bool loop = false, float volume = 1.0f);
    [[nodiscard]] SoundHandle playSoundAt(std::string_view path, const glm::vec3& position,
                                          SoundCategory category = SoundCategory::SFX,
                                          bool loop = false, float volume = 1.0f,
                                          const SoundShape& shape = {});

    // The voice cap (0: none): at most this many SFX sounds play at once. A new one over the cap
    // takes the place of the quietest at the listener (audibleVolume), or is dropped when it would
    // be the quietest itself; either way one sound is lost and counted in voicesDropped.
    void setVoiceLimit(uint32_t limit) noexcept { m_voiceLimit = limit; }
    [[nodiscard]] uint32_t voiceLimit() const noexcept { return m_voiceLimit; }
    [[nodiscard]] uint64_t voicesDropped() const noexcept { return m_voicesDropped; }
    // SFX sounds playing now.
    [[nodiscard]] std::size_t sfxVoices() const;
    [[nodiscard]] std::optional<VoiceInfo> voice(SoundHandle handle) const;
    // The inverse model's gain at `distance` (miniaudio's own formula).
    [[nodiscard]] static float distanceGain(float distance, const SoundShape& shape) noexcept;
    // How loud a sound of this volume, shape and place is at the listener now: its volume, the
    // occlusion's cut and the distance gain (a sound that is not positioned: its volume).
    [[nodiscard]] float audibleVolume(float volume, const glm::vec3* position, const SoundShape& shape) const noexcept;

    // Convenience aliases
    [[nodiscard]] SoundHandle playSfx(std::string_view path, float volume = 1.0f) {
        return playSound(path, SoundCategory::SFX, false, volume);
    }
    [[nodiscard]] SoundHandle playMusic(std::string_view path, bool loop = true, float volume = 1.0f) {
        return playSound(path, SoundCategory::Music, loop, volume);
    }
    [[nodiscard]] SoundHandle playAmbientAt(std::string_view path, const glm::vec3& position,
                                            bool loop = true, float volume = 1.0f) {
        return playSoundAt(path, position, SoundCategory::Ambient, loop, volume);
    }

    // Sound control
    void stopSound(SoundHandle handle);
    void pauseSound(SoundHandle handle);
    void resumeSound(SoundHandle handle);
    void setSoundVolume(SoundHandle handle, float volume);
    void setSoundPosition(SoundHandle handle, const glm::vec3& position);
    [[nodiscard]] bool isPlaying(SoundHandle handle) const;
    [[nodiscard]] std::size_t activeSoundCount() const;

    // Category control
    void setCategoryVolume(SoundCategory category, float volume);
    [[nodiscard]] float getCategoryVolume(SoundCategory category) const;
    void stopCategory(SoundCategory category);
    void pauseCategory(SoundCategory category);
    void resumeCategory(SoundCategory category);

    // Master control
    void setMasterVolume(float volume);
    [[nodiscard]] float getMasterVolume() const noexcept { return m_masterVolume; }
    void stopAll();

    // Cleanup finished sounds (call each frame)
    void update();

private:
    struct SoundInstance {
        VoiceSourcePtr source;  // a bank sound's data source (destroyed last)
        MaLpfNodePtr lowPass;   // declared before the sound: the sound (its input) goes first
        MaSoundPtr sound;
        SoundCategory category = SoundCategory::SFX;
        float baseVolume = 1.0f;
        bool spatial = false;
        bool bank = false;      // a bank sound (play()): outside the category voice cap
        bool stopping = false;  // fading out by stop(): freed once it has stopped
        glm::vec3 position{0.0f};
        SoundShape shape;
        float occlusionDb{0.0f};     // a graded occlusion's cut (bank voices); 0: shape.occluded's, if any
        double occlusionHz{0.0};     // its low-pass; 0: none, or shape.occluded's
        uint64_t startFrame{0};      // a bank voice's
        int bus{-1};                 // a bank voice's bus
    };
    struct Bus;
    struct BusDeleter {
        void operator()(Bus* bus) const;
    };
    struct Decoded;
    struct DecodedDeleter {
        void operator()(Decoded* d) const;
    };

    void updateSoundVolume(SoundInstance& instance);
    void buildBuses();          // m_bank's buses into the graph (none built yet)
    void destroyBuses();
    void pushGates();           // mute and solo into every bus's gate
    [[nodiscard]] Bus* busFor(SoundCategory category) const;
    [[nodiscard]] Bus* busAt(uint8_t bus) const;
    // Routes an occluded sound (initialized without a group) into `bus` through a low-pass at `hz`.
    void routeOccluded(SoundInstance& instance, Bus* bus, double hz = kOccludedLowPassHz);
    [[nodiscard]] SoundHandle nextHandle();
    // The voice cap for a new SFX sound this loud at the listener: false when it is dropped;
    // otherwise room is made (the quietest playing sound stopped if at the cap). Under m_mutex.
    [[nodiscard]] bool admitVoice(float audible);
    // Starts an initialized sound as a new instance (under the cap already).
    [[nodiscard]] SoundHandle start(SoundInstance instance);
    [[nodiscard]] static float clampVolume(float v) noexcept {
        return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }

    MaEnginePtr m_engine;
    bool m_initialized = false;

    std::unordered_map<SoundHandle, SoundInstance> m_sounds;
    SoundHandle m_nextHandle = 1;
    mutable std::mutex m_mutex;

    float m_masterVolume = 1.0f;
    std::array<float, 3> m_categoryVolumes{{1.0f, 1.0f, 1.0f}}; // SFX, Music, Ambient
    glm::vec3 m_listener{0.0f};
    uint32_t m_voiceLimit{0};
    uint64_t m_voicesDropped{0};

    AudioBank m_bank;
    std::vector<std::unique_ptr<Bus, BusDeleter>> m_buses;   // the bank's order: parents first
    std::array<int, 3> m_categoryBus{{0, 0, 0}};
    // Per bank sound, its decoded variants.
    std::vector<std::vector<std::unique_ptr<Decoded, DecodedDeleter>>> m_decoded;
    double m_bankLoadMs{0.0};
    bool m_withoutDevice{false};
    uint64_t m_filesOpened{0};
};

} // namespace ohao
