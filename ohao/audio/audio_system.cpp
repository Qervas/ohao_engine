#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio_system.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cmath>
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

struct AudioLowPass {
    ma_lpf_node node;
};

void MaLpfNodeDeleter::operator()(AudioLowPass* lowPass) const {
    if (!lowPass) return;
    ma_lpf_node_uninit(&lowPass->node, nullptr);
    delete lowPass;
}

AudioSystem::AudioSystem() = default;

AudioSystem::~AudioSystem() {
    shutdown();
}

bool AudioSystem::initialize(bool withoutDevice) {
    if (m_initialized) return true;

    auto engine = MaEnginePtr(new ma_engine());

    ma_engine_config config = ma_engine_config_init();
    config.listenerCount = 1;
    if (withoutDevice) {
        config.noDevice = MA_TRUE;
        config.channels = 2;
        config.sampleRate = 48000;
    }

    const ma_result result = ma_engine_init(&config, engine.get());
    if (result != MA_SUCCESS) {
        OHAO_LOG(Error, audio, "ma_engine_init FAILED: {}", static_cast<int>(result));
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

    m_listener = pos;
    ma_engine_listener_set_position(m_engine.get(), 0, pos.x, pos.y, pos.z);
    ma_engine_listener_set_direction(m_engine.get(), 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(m_engine.get(), 0, up.x, up.y, up.z);
}

SoundHandle AudioSystem::playSound(std::string_view path, SoundCategory category,
                                   bool loop, float volume) {
    if (!m_initialized || !m_engine || path.empty()) return INVALID_SOUND_HANDLE;
    if (!isValidSoundCategory(category)) category = SoundCategory::SFX;
    volume = clampVolume(volume);

    if (category == SoundCategory::SFX) {
        std::lock_guard lock(m_mutex);
        if (!admitVoice(audibleVolume(volume, nullptr, SoundShape{}))) return INVALID_SOUND_HANDLE;
    }

    const std::string pathStr(path);
    auto sound = MaSoundPtr(new ma_sound());
    const ma_uint32 flags = 0;

    const ma_result result = ma_sound_init_from_file(
        m_engine.get(), pathStr.c_str(), flags, nullptr, nullptr, sound.get());
    if (result != MA_SUCCESS) {
        OHAO_LOG(Error, audio, "Failed to load '{}' (error {})", pathStr, static_cast<int>(result));
        return INVALID_SOUND_HANDLE;
    }

    ma_sound_set_spatialization_enabled(sound.get(), MA_FALSE);

    if (loop) {
        ma_sound_set_looping(sound.get(), MA_TRUE);
    }

    SoundInstance instance;
    instance.sound = std::move(sound);
    instance.category = category;
    instance.baseVolume = volume;
    instance.spatial = false;
    return start(std::move(instance));
}

SoundHandle AudioSystem::playSoundAt(std::string_view path, const glm::vec3& position,
                                     SoundCategory category, bool loop, float volume,
                                     const SoundShape& shape) {
    if (!m_initialized || !m_engine || path.empty()) return INVALID_SOUND_HANDLE;
    if (!isValidSoundCategory(category)) category = SoundCategory::SFX;
    volume = clampVolume(volume);

    if (category == SoundCategory::SFX) {
        std::lock_guard lock(m_mutex);
        if (!admitVoice(audibleVolume(volume, &position, shape))) return INVALID_SOUND_HANDLE;
    }

    const std::string pathStr(path);
    auto sound = MaSoundPtr(new ma_sound());

    const ma_result result = ma_sound_init_from_file(
        m_engine.get(), pathStr.c_str(), 0, nullptr, nullptr, sound.get());
    if (result != MA_SUCCESS) {
        OHAO_LOG(Error, audio, "Failed to load '{}' (error {})", pathStr, static_cast<int>(result));
        return INVALID_SOUND_HANDLE;
    }

    ma_sound_set_spatialization_enabled(sound.get(), MA_TRUE);
    ma_sound_set_position(sound.get(), position.x, position.y, position.z);
    ma_sound_set_min_distance(sound.get(), shape.minDistance);
    ma_sound_set_max_distance(sound.get(), shape.maxDistance);
    ma_sound_set_rolloff(sound.get(), shape.rolloff);
    ma_sound_set_attenuation_model(sound.get(), ma_attenuation_model_inverse);

    if (loop) {
        ma_sound_set_looping(sound.get(), MA_TRUE);
    }

    SoundInstance instance;
    // Behind a wall: the sound feeds a low-pass node, which feeds the mix (the cut is in the volume).
    if (shape.occluded) {
        auto lowPass = MaLpfNodePtr(new AudioLowPass());
        const ma_lpf_node_config config = ma_lpf_node_config_init(
            ma_engine_get_channels(m_engine.get()), ma_engine_get_sample_rate(m_engine.get()), kOccludedLowPassHz, 2);
        if (ma_lpf_node_init(ma_engine_get_node_graph(m_engine.get()), &config, nullptr, &lowPass->node) == MA_SUCCESS) {
            ma_node_attach_output_bus(&lowPass->node, 0, ma_engine_get_endpoint(m_engine.get()), 0);
            ma_node_attach_output_bus(sound.get(), 0, &lowPass->node, 0);
            instance.lowPass = std::move(lowPass);
        } else {
            delete lowPass.release();   // never initialized: freed without an uninit
            OHAO_LOG(Warn, audio, "an occluded sound plays without its low-pass (ma_lpf_node_init failed)");
        }
    }
    instance.sound = std::move(sound);
    instance.category = category;
    instance.baseVolume = volume;
    instance.spatial = true;
    instance.position = position;
    instance.shape = shape;
    return start(std::move(instance));
}

SoundHandle AudioSystem::start(SoundInstance instance) {
    const SoundHandle handle = nextHandle();
    ma_sound* raw = instance.sound.get();
    {
        std::lock_guard lock(m_mutex);
        updateSoundVolume(instance);
        m_sounds[handle] = std::move(instance);
    }
    ma_sound_start(raw);
    return handle;
}

float AudioSystem::distanceGain(float distance, const SoundShape& shape) noexcept {
    if (shape.minDistance >= shape.maxDistance) return 1.0f;
    const float d = std::clamp(distance, shape.minDistance, shape.maxDistance);
    return shape.minDistance / (shape.minDistance + shape.rolloff * (d - shape.minDistance));
}

float AudioSystem::audibleVolume(float volume, const glm::vec3* position, const SoundShape& shape) const noexcept {
    if (!position) return volume;
    const float cut = shape.occluded ? std::pow(10.0f, kOccludedGainDb / 20.0f) : 1.0f;
    return volume * cut * distanceGain(glm::length(*position - m_listener), shape);
}

bool AudioSystem::admitVoice(float audible) {
    if (m_voiceLimit == 0) return true;
    // Finished sounds free their voices first.
    std::erase_if(m_sounds, [](auto& entry) {
        const auto& s = entry.second;
        return s.sound && !ma_sound_is_looping(s.sound.get()) && ma_sound_at_end(s.sound.get());
    });
    std::size_t voices = 0;
    auto quietest = m_sounds.end();
    float quietestAudible = 0.0f;
    for (auto it = m_sounds.begin(); it != m_sounds.end(); ++it) {
        const SoundInstance& s = it->second;
        if (s.category != SoundCategory::SFX || !s.sound) continue;
        ++voices;
        const float a = audibleVolume(s.baseVolume, s.spatial ? &s.position : nullptr, s.shape);
        // Ties go to the older sound (the lower handle) being replaced.
        if (quietest == m_sounds.end() || a < quietestAudible || (a == quietestAudible && it->first < quietest->first)) {
            quietest = it;
            quietestAudible = a;
        }
    }
    if (voices < m_voiceLimit) return true;
    ++m_voicesDropped;
    if (quietest == m_sounds.end() || audible <= quietestAudible) return false;   // the new one is the quietest
    ma_sound_stop(quietest->second.sound.get());
    m_sounds.erase(quietest);
    return true;
}

std::size_t AudioSystem::sfxVoices() const {
    std::lock_guard lock(m_mutex);
    std::size_t n = 0;
    for (const auto& [handle, s] : m_sounds)
        n += s.category == SoundCategory::SFX && s.sound && ma_sound_is_playing(s.sound.get()) == MA_TRUE;
    return n;
}

std::optional<VoiceInfo> AudioSystem::voice(SoundHandle handle) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_sounds.find(handle);
    if (it == m_sounds.end() || !it->second.sound) return std::nullopt;
    const SoundInstance& s = it->second;
    VoiceInfo v;
    v.playing = ma_sound_is_playing(s.sound.get()) == MA_TRUE;
    v.spatial = s.spatial;
    v.category = s.category;
    v.position = s.position;
    v.shape = s.shape;
    v.baseVolume = s.baseVolume;
    v.volume = ma_sound_get_volume(s.sound.get());
    v.lowPassHz = s.lowPass ? kOccludedLowPassHz : 0.0;
    return v;
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
    const float occlusion = instance.shape.occluded ? std::pow(10.0f, kOccludedGainDb / 20.0f) : 1.0f;
    const float finalVolume = instance.baseVolume * occlusion *
                              m_categoryVolumes[static_cast<std::size_t>(catIdx)] * m_masterVolume;
    ma_sound_set_volume(instance.sound.get(), finalVolume);
}

SoundHandle AudioSystem::nextHandle() {
    return m_nextHandle++;
}

} // namespace ohao
