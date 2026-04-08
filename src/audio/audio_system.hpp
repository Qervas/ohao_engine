#pragma once

#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <cstdint>
#include <mutex>

// Forward declare miniaudio types to avoid including the massive header here
struct ma_engine;
struct ma_sound;

namespace ohao {

enum class SoundCategory : int {
    SFX = 0,
    Music = 1,
    Ambient = 2
};

using SoundHandle = uint32_t;
constexpr SoundHandle INVALID_SOUND_HANDLE = 0;

class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();

    bool initialize();
    void shutdown();
    bool isInitialized() const { return m_initialized; }

    // Listener (camera)
    void setListenerPosition(const glm::vec3& pos, const glm::vec3& forward, const glm::vec3& up);

    // Play sounds
    SoundHandle playSound(const std::string& path, SoundCategory category = SoundCategory::SFX,
                          bool loop = false, float volume = 1.0f);
    SoundHandle playSoundAt(const std::string& path, const glm::vec3& position,
                            SoundCategory category = SoundCategory::SFX,
                            bool loop = false, float volume = 1.0f);

    // Sound control
    void stopSound(SoundHandle handle);
    void pauseSound(SoundHandle handle);
    void resumeSound(SoundHandle handle);
    void setSoundVolume(SoundHandle handle, float volume);
    void setSoundPosition(SoundHandle handle, const glm::vec3& position);

    // Category control
    void setCategoryVolume(SoundCategory category, float volume);
    float getCategoryVolume(SoundCategory category) const;
    void stopCategory(SoundCategory category);
    void pauseCategory(SoundCategory category);
    void resumeCategory(SoundCategory category);

    // Master control
    void setMasterVolume(float volume);
    float getMasterVolume() const { return m_masterVolume; }
    void stopAll();

    // Cleanup finished sounds (call each frame)
    void update();

private:
    struct SoundInstance {
        ma_sound* sound = nullptr;
        SoundCategory category = SoundCategory::SFX;
        float baseVolume = 1.0f;
        bool spatial = false;
    };

    void updateSoundVolume(SoundInstance& instance);
    SoundHandle nextHandle();

    ma_engine* m_engine = nullptr;
    bool m_initialized = false;

    std::unordered_map<SoundHandle, SoundInstance> m_sounds;
    SoundHandle m_nextHandle = 1;
    std::mutex m_mutex;

    float m_masterVolume = 1.0f;
    float m_categoryVolumes[3] = { 1.0f, 1.0f, 1.0f }; // SFX, Music, Ambient
};

} // namespace ohao
