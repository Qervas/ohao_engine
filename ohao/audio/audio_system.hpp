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

// Forward declare miniaudio types to avoid including the massive header here
struct ma_engine;
struct ma_sound;

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

using MaEnginePtr = std::unique_ptr<ma_engine, MaEngineDeleter>;
using MaSoundPtr = std::unique_ptr<ma_sound, MaSoundDeleter>;

class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;
    AudioSystem(AudioSystem&&) = delete;
    AudioSystem& operator=(AudioSystem&&) = delete;

    [[nodiscard]] bool initialize();
    void shutdown();
    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

    // Listener (camera)
    void setListenerPosition(const glm::vec3& pos, const glm::vec3& forward, const glm::vec3& up);

    // Play sounds — returns INVALID_SOUND_HANDLE on failure
    [[nodiscard]] SoundHandle playSound(std::string_view path,
                                        SoundCategory category = SoundCategory::SFX,
                                        bool loop = false, float volume = 1.0f);
    [[nodiscard]] SoundHandle playSoundAt(std::string_view path, const glm::vec3& position,
                                          SoundCategory category = SoundCategory::SFX,
                                          bool loop = false, float volume = 1.0f);

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
        MaSoundPtr sound;
        SoundCategory category = SoundCategory::SFX;
        float baseVolume = 1.0f;
        bool spatial = false;
    };

    void updateSoundVolume(SoundInstance& instance);
    [[nodiscard]] SoundHandle nextHandle();
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
};

} // namespace ohao
