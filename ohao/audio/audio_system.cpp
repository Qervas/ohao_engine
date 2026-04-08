#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio_system.hpp"
#include <algorithm>
#include <vector>
#include <cstdio>

namespace ohao {

AudioSystem::AudioSystem() = default;

AudioSystem::~AudioSystem() {
    shutdown();
}

bool AudioSystem::initialize() {
    if (m_initialized) return true;

    m_engine = new ma_engine();

    ma_engine_config config = ma_engine_config_init();
    config.listenerCount = 1;

    ma_result result = ma_engine_init(&config, m_engine);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "[OHAO Audio] ma_engine_init FAILED: %d\n", result);
        fflush(stderr);
        delete m_engine;
        m_engine = nullptr;
        return false;
    }

    m_initialized = true;
    return true;
}

void AudioSystem::shutdown() {
    if (!m_initialized) return;

    // Stop and free all sounds
    for (auto& [handle, instance] : m_sounds) {
        if (instance.sound) {
            ma_sound_uninit(instance.sound);
            delete instance.sound;
        }
    }
    m_sounds.clear();

    if (m_engine) {
        ma_engine_uninit(m_engine);
        delete m_engine;
        m_engine = nullptr;
    }

    m_initialized = false;
}

void AudioSystem::setListenerPosition(const glm::vec3& pos, const glm::vec3& forward, const glm::vec3& up) {
    if (!m_initialized) return;

    ma_engine_listener_set_position(m_engine, 0, pos.x, pos.y, pos.z);
    ma_engine_listener_set_direction(m_engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(m_engine, 0, up.x, up.y, up.z);
}

SoundHandle AudioSystem::playSound(const std::string& path, SoundCategory category,
                                    bool loop, float volume) {
    if (!m_initialized) return INVALID_SOUND_HANDLE;

    auto* sound = new ma_sound();
    ma_uint32 flags = 0;

    ma_result result = ma_sound_init_from_file(m_engine, path.c_str(), flags, nullptr, nullptr, sound);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "[OHAO Audio] Failed to load '%s' (error %d)\n", path.c_str(), result);
        delete sound;
        return INVALID_SOUND_HANDLE;
    }

    // Non-spatial (2D) sound
    ma_sound_set_spatialization_enabled(sound, MA_FALSE);

    if (loop) {
        ma_sound_set_looping(sound, MA_TRUE);
    }

    SoundHandle handle = nextHandle();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        SoundInstance instance;
        instance.sound = sound;
        instance.category = category;
        instance.baseVolume = volume;
        instance.spatial = false;
        m_sounds[handle] = instance;
        updateSoundVolume(instance);
    }

    ma_sound_start(sound);
    return handle;
}

SoundHandle AudioSystem::playSoundAt(const std::string& path, const glm::vec3& position,
                                      SoundCategory category, bool loop, float volume) {
    if (!m_initialized) return INVALID_SOUND_HANDLE;

    auto* sound = new ma_sound();

    ma_result result = ma_sound_init_from_file(m_engine, path.c_str(), 0, nullptr, nullptr, sound);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "[OHAO Audio] Failed to load '%s' (error %d)\n", path.c_str(), result);
        delete sound;
        return INVALID_SOUND_HANDLE;
    }

    // 3D spatial sound
    ma_sound_set_spatialization_enabled(sound, MA_TRUE);
    ma_sound_set_position(sound, position.x, position.y, position.z);
    ma_sound_set_min_distance(sound, 1.0f);
    ma_sound_set_max_distance(sound, 50.0f);
    ma_sound_set_attenuation_model(sound, ma_attenuation_model_inverse);

    if (loop) {
        ma_sound_set_looping(sound, MA_TRUE);
    }

    SoundHandle handle = nextHandle();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        SoundInstance instance;
        instance.sound = sound;
        instance.category = category;
        instance.baseVolume = volume;
        instance.spatial = true;
        m_sounds[handle] = instance;
        updateSoundVolume(instance);
    }

    ma_sound_start(sound);
    return handle;
}

void AudioSystem::stopSound(SoundHandle handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end() && it->second.sound) {
        ma_sound_stop(it->second.sound);
        ma_sound_uninit(it->second.sound);
        delete it->second.sound;
        m_sounds.erase(it);
    }
}

void AudioSystem::pauseSound(SoundHandle handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end() && it->second.sound) {
        ma_sound_stop(it->second.sound);
    }
}

void AudioSystem::resumeSound(SoundHandle handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end() && it->second.sound) {
        ma_sound_start(it->second.sound);
    }
}

void AudioSystem::setSoundVolume(SoundHandle handle, float volume) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end()) {
        it->second.baseVolume = volume;
        updateSoundVolume(it->second);
    }
}

void AudioSystem::setSoundPosition(SoundHandle handle, const glm::vec3& position) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end() && it->second.sound) {
        ma_sound_set_position(it->second.sound, position.x, position.y, position.z);
    }
}

void AudioSystem::setCategoryVolume(SoundCategory category, float volume) {
    int idx = static_cast<int>(category);
    if (idx < 0 || idx > 2) return;
    m_categoryVolumes[idx] = std::clamp(volume, 0.0f, 1.0f);

    // Update all sounds in this category
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [handle, instance] : m_sounds) {
        if (instance.category == category) {
            updateSoundVolume(instance);
        }
    }
}

float AudioSystem::getCategoryVolume(SoundCategory category) const {
    int idx = static_cast<int>(category);
    if (idx < 0 || idx > 2) return 1.0f;
    return m_categoryVolumes[idx];
}

void AudioSystem::stopCategory(SoundCategory category) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<SoundHandle> toRemove;
    for (auto& [handle, instance] : m_sounds) {
        if (instance.category == category && instance.sound) {
            ma_sound_stop(instance.sound);
            ma_sound_uninit(instance.sound);
            delete instance.sound;
            toRemove.push_back(handle);
        }
    }
    for (auto h : toRemove) {
        m_sounds.erase(h);
    }
}

void AudioSystem::pauseCategory(SoundCategory category) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [handle, instance] : m_sounds) {
        if (instance.category == category && instance.sound) {
            ma_sound_stop(instance.sound);
        }
    }
}

void AudioSystem::resumeCategory(SoundCategory category) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [handle, instance] : m_sounds) {
        if (instance.category == category && instance.sound) {
            ma_sound_start(instance.sound);
        }
    }
}

void AudioSystem::setMasterVolume(float volume) {
    m_masterVolume = std::clamp(volume, 0.0f, 1.0f);

    // Update all sounds
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [handle, instance] : m_sounds) {
        updateSoundVolume(instance);
    }
}

void AudioSystem::stopAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [handle, instance] : m_sounds) {
        if (instance.sound) {
            ma_sound_stop(instance.sound);
            ma_sound_uninit(instance.sound);
            delete instance.sound;
        }
    }
    m_sounds.clear();
}

void AudioSystem::update() {
    if (!m_initialized) return;

    // Cleanup finished (non-looping) sounds
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<SoundHandle> finished;
    for (auto& [handle, instance] : m_sounds) {
        if (instance.sound && !ma_sound_is_looping(instance.sound) && ma_sound_at_end(instance.sound)) {
            finished.push_back(handle);
        }
    }
    for (auto h : finished) {
        auto& instance = m_sounds[h];
        ma_sound_uninit(instance.sound);
        delete instance.sound;
        m_sounds.erase(h);
    }
}

void AudioSystem::updateSoundVolume(SoundInstance& instance) {
    if (!instance.sound) return;
    int catIdx = static_cast<int>(instance.category);
    float finalVolume = instance.baseVolume * m_categoryVolumes[catIdx] * m_masterVolume;
    ma_sound_set_volume(instance.sound, finalVolume);
}

SoundHandle AudioSystem::nextHandle() {
    return m_nextHandle++;
}

} // namespace ohao
