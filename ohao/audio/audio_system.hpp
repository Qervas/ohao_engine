#pragma once

/**
 * AudioSystem — miniaudio facade (3D listener, SFX/Music/Ambient buses).
 *
 * Art notes:
 *   - SoundHandle validity helpers + operator bool
 *   - SoundCategory with to_underlying / name helpers
 *   - Result-friendly play APIs stay handle-based (INVALID_SOUND_HANDLE = fail)
 *   - string_view paths; clamp volumes to [0,1]
 */

#include "core/concepts.hpp"

#include <glm/glm.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <mutex>
#include <compare>
#include <array>
#include <optional>

// Forward declare miniaudio types to avoid including the massive header here
struct ma_engine;
struct ma_sound;

namespace ohao {
struct AudioLowPass;   // a miniaudio low-pass node (its type is anonymous: wrapped in the .cpp)
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

using MaEnginePtr = std::unique_ptr<ma_engine, MaEngineDeleter>;
using MaSoundPtr = std::unique_ptr<ma_sound, MaSoundDeleter>;
using MaLpfNodePtr = std::unique_ptr<AudioLowPass, MaLpfNodeDeleter>;

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
    float volume{1.0f};       // set on the sound: base x occlusion x category x master
    double lowPassHz{0.0};    // 0: none
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
        MaLpfNodePtr lowPass;   // declared before the sound: the sound (its input) goes first
        MaSoundPtr sound;
        SoundCategory category = SoundCategory::SFX;
        float baseVolume = 1.0f;
        bool spatial = false;
        glm::vec3 position{0.0f};
        SoundShape shape;
    };

    void updateSoundVolume(SoundInstance& instance);
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
};

} // namespace ohao
