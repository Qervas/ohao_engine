#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio_system.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace ohao {

void MaEngineDeleter::operator()(ma_engine* engine) const {
    if (!engine) return;
    ma_engine_uninit(engine);
    delete engine;
}

void MaSoundDeleter::operator()(ma_sound* sound) const {
    if (!sound) return;
    ma_sound_uninit(sound);
    delete sound;
}

AudioSystem::AudioSystem() = default;

AudioSystem::~AudioSystem() {
    shutdown();
}

bool AudioSystem::initialize() {
    if (m_initialized) return true;

    auto engine = MaEnginePtr(new ma_engine());

    ma_engine_config config = ma_engine_config_init();
    config.listenerCount = 1;

    const ma_result result = ma_engine_init(&config, engine.get());
    if (result != MA_SUCCESS) {
        fprintf(stderr, "[OHAO Audio] ma_engine_init FAILED: %d\n", result);
        fflush(stderr);
        return false;
    }

    m_engine = std::move(engine);
    m_initialized = true;
    return true;
}

void AudioSystem::shutdown() {
    if (!m_initialized) return;

    m_sounds.clear(); // MaSoundDeleter uninits each sound
    m_engine.reset(); // MaEngineDeleter uninits the engine
    m_initialized = false;
}

void AudioSystem::setListenerPosition(const glm::vec3& pos, const glm::vec3& forward, const glm::vec3& up) {
    if (!m_initialized || !m_engine) return;

    ma_engine_listener_set_position(m_engine.get(), 0, pos.x, pos.y, pos.z);
    ma_engine_listener_set_direction(m_engine.get(), 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(m_engine.get(), 0, up.x, up.y, up.z);
}

SoundHandle AudioSystem::playSound(std::string_view path, SoundCategory category,
                                   bool loop, float volume) {
    if (!m_initialized || !m_engine || path.empty()) return INVALID_SOUND_HANDLE;
    if (!isValidSoundCategory(category)) category = SoundCategory::SFX;
    volume = clampVolume(volume);

    const std::string pathStr(path);
    auto sound = MaSoundPtr(new ma_sound());
    const ma_uint32 flags = 0;

    const ma_result result = ma_sound_init_from_file(
        m_engine.get(), pathStr.c_str(), flags, nullptr, nullptr, sound.get());
    if (result != MA_SUCCESS) {
        fprintf(stderr, "[OHAO Audio] Failed to load '%s' (error %d)\n", pathStr.c_str(), result);
        return INVALID_SOUND_HANDLE;
    }

    ma_sound_set_spatialization_enabled(sound.get(), MA_FALSE);

    if (loop) {
        ma_sound_set_looping(sound.get(), MA_TRUE);
    }

    const SoundHandle handle = nextHandle();
    ma_sound* raw = sound.get();

    {
        std::lock_guard lock(m_mutex);
        SoundInstance instance;
        instance.sound = std::move(sound);
        instance.category = category;
        instance.baseVolume = volume;
        instance.spatial = false;
        updateSoundVolume(instance);
        m_sounds[handle] = std::move(instance);
    }

    ma_sound_start(raw);
    return handle;
}

SoundHandle AudioSystem::playSoundAt(std::string_view path, const glm::vec3& position,
                                     SoundCategory category, bool loop, float volume) {
    if (!m_initialized || !m_engine || path.empty()) return INVALID_SOUND_HANDLE;
    if (!isValidSoundCategory(category)) category = SoundCategory::SFX;
    volume = clampVolume(volume);

    const std::string pathStr(path);
    auto sound = MaSoundPtr(new ma_sound());

    const ma_result result = ma_sound_init_from_file(
        m_engine.get(), pathStr.c_str(), 0, nullptr, nullptr, sound.get());
    if (result != MA_SUCCESS) {
        fprintf(stderr, "[OHAO Audio] Failed to load '%s' (error %d)\n", pathStr.c_str(), result);
        return INVALID_SOUND_HANDLE;
    }

    ma_sound_set_spatialization_enabled(sound.get(), MA_TRUE);
    ma_sound_set_position(sound.get(), position.x, position.y, position.z);
    ma_sound_set_min_distance(sound.get(), 1.0f);
    ma_sound_set_max_distance(sound.get(), 50.0f);
    ma_sound_set_attenuation_model(sound.get(), ma_attenuation_model_inverse);

    if (loop) {
        ma_sound_set_looping(sound.get(), MA_TRUE);
    }

    const SoundHandle handle = nextHandle();
    ma_sound* raw = sound.get();

    {
        std::lock_guard lock(m_mutex);
        SoundInstance instance;
        instance.sound = std::move(sound);
        instance.category = category;
        instance.baseVolume = volume;
        instance.spatial = true;
        updateSoundVolume(instance);
        m_sounds[handle] = std::move(instance);
    }

    ma_sound_start(raw);
    return handle;
}

void AudioSystem::stopSound(SoundHandle handle) {
    if (!isValidSoundHandle(handle)) return;
    std::lock_guard lock(m_mutex);
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end() && it->second.sound) {
        ma_sound_stop(it->second.sound.get());
        m_sounds.erase(it);
    }
}

void AudioSystem::pauseSound(SoundHandle handle) {
    if (!isValidSoundHandle(handle)) return;
    std::lock_guard lock(m_mutex);
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end() && it->second.sound) {
        ma_sound_stop(it->second.sound.get());
    }
}

void AudioSystem::resumeSound(SoundHandle handle) {
    if (!isValidSoundHandle(handle)) return;
    std::lock_guard lock(m_mutex);
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end() && it->second.sound) {
        ma_sound_start(it->second.sound.get());
    }
}

void AudioSystem::setSoundVolume(SoundHandle handle, float volume) {
    if (!isValidSoundHandle(handle)) return;
    volume = clampVolume(volume);
    std::lock_guard lock(m_mutex);
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end()) {
        it->second.baseVolume = volume;
        updateSoundVolume(it->second);
    }
}

void AudioSystem::setSoundPosition(SoundHandle handle, const glm::vec3& position) {
    if (!isValidSoundHandle(handle)) return;
    std::lock_guard lock(m_mutex);
    auto it = m_sounds.find(handle);
    if (it != m_sounds.end() && it->second.sound) {
        ma_sound_set_position(it->second.sound.get(), position.x, position.y, position.z);
    }
}

bool AudioSystem::isPlaying(SoundHandle handle) const {
    if (!isValidSoundHandle(handle)) return false;
    std::lock_guard lock(m_mutex);
    auto it = m_sounds.find(handle);
    if (it == m_sounds.end() || !it->second.sound) return false;
    return ma_sound_is_playing(it->second.sound.get()) == MA_TRUE;
}

std::size_t AudioSystem::activeSoundCount() const {
    std::lock_guard lock(m_mutex);
    return m_sounds.size();
}

void AudioSystem::setCategoryVolume(SoundCategory category, float volume) {
    if (!isValidSoundCategory(category)) return;
    const int idx = soundCategoryIndex(category);
    m_categoryVolumes[static_cast<std::size_t>(idx)] = clampVolume(volume);

    std::lock_guard lock(m_mutex);
    for (auto& [handle, instance] : m_sounds) {
        if (instance.category == category) {
            updateSoundVolume(instance);
        }
    }
}

float AudioSystem::getCategoryVolume(SoundCategory category) const {
    if (!isValidSoundCategory(category)) return 1.0f;
    return m_categoryVolumes[static_cast<std::size_t>(soundCategoryIndex(category))];
}

void AudioSystem::stopCategory(SoundCategory category) {
    if (!isValidSoundCategory(category)) return;
    std::lock_guard lock(m_mutex);
    std::erase_if(m_sounds, [&](auto& entry) {
        auto& instance = entry.second;
        if (instance.category == category && instance.sound) {
            ma_sound_stop(instance.sound.get());
            return true;
        }
        return false;
    });
}

void AudioSystem::pauseCategory(SoundCategory category) {
    if (!isValidSoundCategory(category)) return;
    std::lock_guard lock(m_mutex);
    for (auto& [handle, instance] : m_sounds) {
        if (instance.category == category && instance.sound) {
            ma_sound_stop(instance.sound.get());
        }
    }
}

void AudioSystem::resumeCategory(SoundCategory category) {
    if (!isValidSoundCategory(category)) return;
    std::lock_guard lock(m_mutex);
    for (auto& [handle, instance] : m_sounds) {
        if (instance.category == category && instance.sound) {
            ma_sound_start(instance.sound.get());
        }
    }
}

void AudioSystem::setMasterVolume(float volume) {
    m_masterVolume = clampVolume(volume);

    std::lock_guard lock(m_mutex);
    for (auto& [handle, instance] : m_sounds) {
        updateSoundVolume(instance);
    }
}

void AudioSystem::stopAll() {
    std::lock_guard lock(m_mutex);
    for (auto& [handle, instance] : m_sounds) {
        if (instance.sound) {
            ma_sound_stop(instance.sound.get());
        }
    }
    m_sounds.clear();
}

void AudioSystem::update() {
    if (!m_initialized) return;

    std::lock_guard lock(m_mutex);
    std::erase_if(m_sounds, [](auto& entry) {
        auto& instance = entry.second;
        if (instance.sound && !ma_sound_is_looping(instance.sound.get())
            && ma_sound_at_end(instance.sound.get())) {
            return true;
        }
        return false;
    });
}

void AudioSystem::updateSoundVolume(SoundInstance& instance) {
    if (!instance.sound) return;
    const int catIdx = soundCategoryIndex(instance.category);
    const float finalVolume =
        instance.baseVolume * m_categoryVolumes[static_cast<std::size_t>(catIdx)] * m_masterVolume;
    ma_sound_set_volume(instance.sound.get(), finalVolume);
}

SoundHandle AudioSystem::nextHandle() {
    return m_nextHandle++;
}

} // namespace ohao
