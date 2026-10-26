#include "runtime/footsteps.hpp"

#include "core/log.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/scene_view.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ohao::runtime {

namespace {

// "<surface>_<n>.wav": the surface and n, or false.
bool parseName(const std::filesystem::path& p, std::string& surface, unsigned long& n) {
    if (p.extension() != ".wav") return false;
    const std::string stem = p.stem().string();
    const std::size_t cut = stem.rfind('_');
    if (cut == std::string::npos || cut == 0 || cut + 1 == stem.size()) return false;
    const std::string digits = stem.substr(cut + 1);
    if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }) || digits.size() > 6)
        return false;
    surface = stem.substr(0, cut);
    n = std::stoul(digits);
    return true;
}

} // namespace

FootstepSounds::FootstepSounds(const std::vector<std::filesystem::path>& folders) {
    for (const auto& folder : folders) {
        std::error_code ec;
        if (!std::filesystem::is_directory(folder, ec)) continue;
        std::map<std::string, std::vector<std::pair<unsigned long, std::string>>, std::less<>> found;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
            std::string surface;
            unsigned long n = 0;
            if (entry.is_regular_file(ec) && parseName(entry.path(), surface, n))
                found[surface].emplace_back(n, entry.path().string());
        }
        for (auto& [surface, files] : found) {
            if (m_sets.contains(surface)) continue;   // an earlier folder has this surface
            std::sort(files.begin(), files.end());
            Set& set = m_sets[surface];
            for (auto& [n, path] : files) set.files.push_back(std::move(path));
            m_count += set.files.size();
        }
    }
}

std::string FootstepSounds::next(std::string_view surface) {
    auto it = m_sets.find(surface);
    if (it == m_sets.end()) it = m_sets.find(std::string_view("default"));
    if (it == m_sets.end() || it->second.files.empty()) return {};
    Set& set = it->second;
    return set.files[set.next++ % set.files.size()];
}

bool Footsteps::initialize(const std::filesystem::path& root) {
    m_sounds = std::make_unique<FootstepSounds>(std::vector<std::filesystem::path>{
        root / "assets" / "local" / "audio" / "footsteps", root / "assets" / "audio" / "footsteps"});
    if (!m_audio.initialize()) {
        OHAO_LOG(Warn, audio, "footsteps: no audio device; footsteps are silent");
        return false;
    }
    OHAO_LOG(Info, audio, "footsteps: {} sounds", m_sounds->sounds());
    return true;
}

void Footsteps::update(ecs::World& world) {
    if (!m_audio.isInitialized() || !m_sounds) return;
    const ecs::PhysicsScene* physics = ecs::physicsScene(world);
    if (!physics) return;
    if (!std::exchange(m_started, true)) m_cursor = physics->nextEventSeq() - 1;   // nothing from before

    // Heard from the camera: the renderer's pitch and yaw back to a direction.
    if (const auto view = ecs::resolveSceneView(world).camera) {
        const float p = glm::radians(view->pitchDeg), y = glm::radians(view->yawDeg);
        const glm::vec3 forward(std::cos(y) * std::cos(p), std::sin(p), std::sin(y) * std::cos(p));
        m_audio.setListenerPosition(view->position, forward, glm::vec3(0.0f, 1.0f, 0.0f));
    }
    for (;;) {
        uint64_t dropped = 0;
        const auto events = physics->eventsSince(m_cursor, 64, dropped);
        if (events.empty()) break;
        for (const ecs::PhysicsEvent& e : events) {
            m_cursor = e.seq;
            const bool land = e.kind == ecs::PhysicsEvent::Kind::Land;
            if (e.kind != ecs::PhysicsEvent::Kind::Footstep && !land) continue;
            const std::string path = m_sounds->next(e.surface);
            if (path.empty()) continue;
            // Louder with speed: a walk (4 m/s) 0.65, a sprint (6.5) 0.9; a landing by its impact.
            const float volume = land ? std::clamp(e.speed / 6.0f, 0.4f, 1.0f) : std::clamp(0.25f + 0.1f * e.speed, 0.2f, 1.0f);
            if (isValidSoundHandle(m_audio.playSoundAt(path, e.point, SoundCategory::SFX, false, volume))) ++m_played;
        }
    }
    m_audio.update();   // finished sounds go
}

} // namespace ohao::runtime
