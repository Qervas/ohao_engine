#include "runtime/combat_sounds.hpp"

#include "core/log.hpp"

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

SoundPlan planCombatSound(std::string_view name, const ecs::CombatEvent& e, const Listener& listener) {
    SoundPlan plan;
    const bool own = name == "fire" || name == "hurt" || name == "reload" || name == "empty";
    plan.at = own ? listener.position : e.point;
    plan.volume = name == "enemy_fire" ? 0.7f : name.rfind("impact_", 0) == 0 ? 0.55f : 0.85f;
    plan.shape = shapeFor(name == "fire" || name == "enemy_fire" ? SoundKind::Gunfire : SoundKind::Near);
    plan.enemy = name == "enemy_fire" && e.sourceId != 0 && e.sourceId != listener.playerId;
    return plan;
}

bool CombatSounds::initialize(const std::filesystem::path& root, AudioSystem& audio) {
    const std::filesystem::path folder = root / "assets" / "audio" / "weapons";
    std::error_code ec;
    if (std::filesystem::is_directory(folder, ec))
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
            if (entry.is_regular_file(ec) && entry.path().extension() == ".wav")
                m_files[entry.path().stem().string()] = entry.path().string();
    if (!audio.isInitialized()) {
        OHAO_LOG(Warn, audio, "combat sounds: no audio device; combat is silent");
        return false;
    }
    m_audio = &audio;
    OHAO_LOG(Info, audio, "combat sounds: {} sounds", m_files.size());
    return true;
}

void CombatSounds::update(ecs::World& world) {
    if (!m_audio || !m_audio->isInitialized()) return;
    const ecs::CombatSystem* combat = ecs::combatSystem(world);
    if (!combat) return;
    if (!std::exchange(m_started, true)) m_cursor = combat->nextEventSeq();   // nothing from before

    // Heard from the camera; the player's own sounds (its rounds, its hurt, its reload) at the ear.
    const Listener listener = listenerOf(world).value_or(Listener{});
    m_audio->setListenerPosition(listener.position, listener.forward, glm::vec3(0.0f, 1.0f, 0.0f));
    for (;;) {
        uint64_t dropped = 0;
        const auto events = combat->eventsSince(m_cursor, 64, dropped);
        if (events.empty()) break;
        for (const ecs::CombatEvent& e : events) {
            m_cursor = e.seq + 1;
            for (const std::string& name : soundsFor(e, listener.playerId)) {
                auto it = m_files.find(name);
                if (it == m_files.end() && name.rfind("impact_", 0) == 0) it = m_files.find("impact_default");
                if (it == m_files.end()) continue;
                SoundPlan plan = planCombatSound(name, e, listener);
                if (plan.enemy) {   // one ray per enemy round started
                    ++m_rays;
                    plan.shape.occluded = soundOccluded(world, listener, plan.at);
                    m_occluded += plan.shape.occluded;
                }
                if (isValidSoundHandle(m_audio->playSoundAt(it->second, plan.at, SoundCategory::SFX, false, plan.volume, plan.shape)))
                    ++m_played;
            }
        }
    }
}

} // namespace ohao::runtime
