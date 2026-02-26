#pragma once

#include <glm/glm.hpp>
#include <string>
#include <cstdint>

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

// Forward declare
namespace ohao {
    class AudioSystem;
    class Camera;
    enum class SoundCategory : int;
}

namespace godot {

/**
 * AudioManager - 3D positional audio via miniaudio
 *
 * Plain C++ class (like CameraController), owned by OhaoViewport.
 * Wraps ohao::AudioSystem, resolves res:// paths, syncs listener from camera.
 */
class AudioManager {
public:
    AudioManager();
    ~AudioManager();

    bool initialize(const std::string& baseSoundsPath);
    void shutdown();
    bool isInitialized() const;

    // Sync listener to camera (call each frame)
    void updateListener(ohao::Camera& camera);

    // Play sounds (handle res:// path resolution)
    uint32_t playSound(const String& path, int category, bool loop, float volume);
    uint32_t playSoundAt(const String& path, const Vector3& position, int category, bool loop, float volume);

    // Sound control
    void stopSound(uint32_t handle);
    void pauseSound(uint32_t handle);
    void resumeSound(uint32_t handle);
    void setSoundVolume(uint32_t handle, float volume);
    void setSoundPosition(uint32_t handle, const Vector3& position);

    // Category control
    void setCategoryVolume(int category, float volume);
    float getCategoryVolume(int category) const;
    void stopCategory(int category);
    void pauseCategory(int category);
    void resumeCategory(int category);

    // Master control
    void setMasterVolume(float volume);
    float getMasterVolume() const;
    void stopAll();

    // Cleanup (call each frame)
    void update();

private:
    std::string resolveAudioPath(const String& path) const;

    ohao::AudioSystem* m_audioSystem = nullptr;
    std::string m_basePath;
};

} // namespace godot
