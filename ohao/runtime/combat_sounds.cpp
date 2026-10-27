#include "runtime/combat_sounds.hpp"

#include "core/log.hpp"
#include "ecs/scene_view.hpp"

#include <cmath>
#include <utility>

namespace ohao::runtime {

std::vector<std::string> soundsFor(const ecs::CombatEvent& e, uint64_t playerId) {
    using K = ecs::CombatEvent::Kind;
    const bool byPlayer = playerId != 0 && e.sourceId == playerId;
    switch (e.kind) {
    case K::Shot: return {byPlayer ? "fire" : "enemy_fire"};
    case K::Hit: return {"impact_" + (e.surface.empty() ? std::string("default") : e.surface)};
    case K::Damage:
        if (playerId != 0 && e.targetId == playerId) return {"hurt"};
        return {};
    case K::Death: return {"death"};
    case K::Reload:
        if (!byPlayer) return {};
        if (e.amount == 0.0f) return {"empty", "reload"};
        return {"reload"};
    default: return {};
    }
}

bool CombatSounds::initialize(const std::filesystem::path& root) {
    const std::filesystem::path folder = root / "assets" / "audio" / "weapons";
    std::error_code ec;
    if (std::filesystem::is_directory(folder, ec))
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
            if (entry.is_regular_file(ec) && entry.path().extension() == ".wav")
                m_files[entry.path().stem().string()] = entry.path().string();
    if (!m_audio.initialize()) {
        OHAO_LOG(Warn, audio, "combat sounds: no audio device; combat is silent");
        return false;
    }
    OHAO_LOG(Info, audio, "combat sounds: {} sounds", m_files.size());
    return true;
}

void CombatSounds::update(ecs::World& world) {
    if (!m_audio.isInitialized()) return;
    const ecs::CombatSystem* combat = ecs::combatSystem(world);
    if (!combat) return;
    if (!std::exchange(m_started, true)) m_cursor = combat->nextEventSeq();   // nothing from before

    // Heard from the camera; the player's own sounds (its rounds, its hurt, its reload) at the ear.
    glm::vec3 ear(0.0f);
    if (const auto view = ecs::resolveSceneView(world).camera) {
        const float p = glm::radians(view->pitchDeg), y = glm::radians(view->yawDeg);
        const glm::vec3 forward(std::cos(y) * std::cos(p), std::sin(p), std::sin(y) * std::cos(p));
        ear = view->position;
        m_audio.setListenerPosition(view->position, forward, glm::vec3(0.0f, 1.0f, 0.0f));
    }
    uint64_t playerId = 0;
    for (auto [e, pc] : world.registry().view<const ecs::PlayerController>().each()) {
        const uint64_t id = world.idOf(e);
        if (pc.input && (playerId == 0 || id < playerId)) playerId = id;
    }
    for (;;) {
        uint64_t dropped = 0;
        const auto events = combat->eventsSince(m_cursor, 64, dropped);
        if (events.empty()) break;
        for (const ecs::CombatEvent& e : events) {
            m_cursor = e.seq + 1;
            for (const std::string& name : soundsFor(e, playerId)) {
                auto it = m_files.find(name);
                if (it == m_files.end() && name.rfind("impact_", 0) == 0) it = m_files.find("impact_default");
                if (it == m_files.end()) continue;
                const bool own = name == "fire" || name == "hurt" || name == "reload" || name == "empty";
                const glm::vec3 at = own ? ear : e.point;
                const float volume = name == "enemy_fire" ? 0.7f : name.rfind("impact_", 0) == 0 ? 0.55f : 0.85f;
                if (isValidSoundHandle(m_audio.playSoundAt(it->second, at, SoundCategory::SFX, false, volume))) ++m_played;
            }
        }
    }
    m_audio.update();
}

} // namespace ohao::runtime
