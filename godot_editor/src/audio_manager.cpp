#include "audio_manager.h"
#include "scene_sync.h"
#include "audio/audio_system.hpp"
#include "renderer/camera/camera.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

AudioManager::AudioManager() = default;

AudioManager::~AudioManager() {
    shutdown();
}

bool AudioManager::initialize(const std::string& baseSoundsPath) {
    if (m_audioSystem) return true;

    m_basePath = baseSoundsPath;
    m_audioSystem = new ohao::AudioSystem();

    if (!m_audioSystem->initialize()) {
        UtilityFunctions::printerr("[OHAO Audio] Failed to initialize audio system");
        delete m_audioSystem;
        m_audioSystem = nullptr;
        return false;
    }

    UtilityFunctions::print("[OHAO Audio] Audio system initialized (sounds path: ", String(m_basePath.c_str()), ")");
    return true;
}

void AudioManager::shutdown() {
    if (m_audioSystem) {
        m_audioSystem->shutdown();
        delete m_audioSystem;
        m_audioSystem = nullptr;
        UtilityFunctions::print("[OHAO Audio] Audio system shut down");
    }
}

bool AudioManager::isInitialized() const {
    return m_audioSystem && m_audioSystem->isInitialized();
}

void AudioManager::updateListener(ohao::Camera& camera) {
    if (!m_audioSystem) return;

    glm::vec3 pos = camera.getPosition();
    glm::vec3 front = camera.getFront();
    glm::vec3 up = camera.getUp();
    m_audioSystem->setListenerPosition(pos, front, up);
    m_audioSystem->update();
}

uint32_t AudioManager::playSound(const String& path, int category, bool loop, float volume) {
    if (!m_audioSystem) return 0;

    std::string resolved = resolveAudioPath(path);
    return m_audioSystem->playSound(resolved,
        static_cast<ohao::SoundCategory>(category), loop, volume);
}

uint32_t AudioManager::playSoundAt(const String& path, const Vector3& position,
                                    int category, bool loop, float volume) {
    if (!m_audioSystem) return 0;

    std::string resolved = resolveAudioPath(path);
    glm::vec3 pos(position.x, position.y, position.z);
    return m_audioSystem->playSoundAt(resolved, pos,
        static_cast<ohao::SoundCategory>(category), loop, volume);
}

void AudioManager::stopSound(uint32_t handle) {
    if (m_audioSystem) m_audioSystem->stopSound(handle);
}

void AudioManager::pauseSound(uint32_t handle) {
    if (m_audioSystem) m_audioSystem->pauseSound(handle);
}

void AudioManager::resumeSound(uint32_t handle) {
    if (m_audioSystem) m_audioSystem->resumeSound(handle);
}

void AudioManager::setSoundVolume(uint32_t handle, float volume) {
    if (m_audioSystem) m_audioSystem->setSoundVolume(handle, volume);
}

void AudioManager::setSoundPosition(uint32_t handle, const Vector3& position) {
    if (m_audioSystem) {
        m_audioSystem->setSoundPosition(handle, glm::vec3(position.x, position.y, position.z));
    }
}

void AudioManager::setCategoryVolume(int category, float volume) {
    if (m_audioSystem) m_audioSystem->setCategoryVolume(static_cast<ohao::SoundCategory>(category), volume);
}

float AudioManager::getCategoryVolume(int category) const {
    if (m_audioSystem) return m_audioSystem->getCategoryVolume(static_cast<ohao::SoundCategory>(category));
    return 1.0f;
}

void AudioManager::stopCategory(int category) {
    if (m_audioSystem) m_audioSystem->stopCategory(static_cast<ohao::SoundCategory>(category));
}

void AudioManager::pauseCategory(int category) {
    if (m_audioSystem) m_audioSystem->pauseCategory(static_cast<ohao::SoundCategory>(category));
}

void AudioManager::resumeCategory(int category) {
    if (m_audioSystem) m_audioSystem->resumeCategory(static_cast<ohao::SoundCategory>(category));
}

void AudioManager::setMasterVolume(float volume) {
    if (m_audioSystem) m_audioSystem->setMasterVolume(volume);
}

float AudioManager::getMasterVolume() const {
    if (m_audioSystem) return m_audioSystem->getMasterVolume();
    return 1.0f;
}

void AudioManager::stopAll() {
    if (m_audioSystem) m_audioSystem->stopAll();
}

void AudioManager::update() {
    if (m_audioSystem) m_audioSystem->update();
}

std::string AudioManager::resolveAudioPath(const String& path) const {
    // If it's a res:// path, resolve to absolute
    String pathStr = path;
    if (pathStr.begins_with("res://")) {
        return SceneSync::resolveResPath(pathStr);
    }

    // If it's a relative path (e.g., "sfx/gunshot.wav"), prepend base sounds dir
    std::string p = pathStr.utf8().get_data();
    if (!p.empty() && p[0] != '/' && p[0] != '\\' && (p.size() < 2 || p[1] != ':')) {
        return m_basePath + "/" + p;
    }

    return p;
}

} // namespace godot
