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

std::vector<std::string> soundsForAnim(const ecs::AnimEvent& e, uint64_t /*playerId*/) {
    if (e.name == "mag_out" || e.name == "mag_in" || e.name == "bolt" || e.name == "body_fall") return {e.name};
    return {};
}

SoundPlan planAnimSound(std::string_view name, const ecs::AnimEvent& e, glm::vec3 at, const Listener& listener) {
    SoundPlan plan;
    const bool own = listener.playerId != 0 && e.entity == listener.playerId;
    plan.at = own ? listener.position : at;
    plan.volume = name == "body_fall" ? 0.8f : 0.6f;
    plan.shape = shapeFor(SoundKind::Near);
    plan.enemy = !own && e.entity != 0;
    return plan;
}

uint64_t animSoundOwner(const ecs::World& world, uint64_t entityId, uint64_t playerId) {
    if (playerId == 0) return entityId;
    const auto& reg = world.registry();
    entt::entity at = world.findById(entityId);
    for (int depth = 0; depth < 64 && at != entt::null && reg.valid(at); ++depth) {
        if (world.idOf(at) == playerId) return playerId;
        const auto* p = reg.try_get<ecs::Parent>(at);
        at = p ? p->entity : entt::null;
    }
    return entityId;
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
                play(world, it->second, planCombatSound(name, e, listener), listener);
            }
        }
    }

    // Handling sounds from the animation log, where the animated entity is drawn.
    const ecs::AnimationSystem* anim = ecs::animationSystem(world);
    if (!anim) return;
    if (!std::exchange(m_animStarted, true)) m_animCursor = anim->nextEventSeq();
    const auto& reg = world.registry();
    for (;;) {
        uint64_t dropped = 0;
        const auto events = anim->eventsSince(m_animCursor, 64, dropped);
        if (events.empty()) break;
        for (const ecs::AnimEvent& e : events) {
            m_animCursor = e.seq + 1;
            for (const std::string& name : soundsForAnim(e, listener.playerId)) {
                const auto it = m_files.find(name);
                const entt::entity who = world.findById(e.entity);
                if (it == m_files.end() || who == entt::null) continue;
                glm::vec3 at(0.0f);
                if (const auto* rp = reg.try_get<ecs::RenderPose>(who)) at = glm::vec3(rp->world[3]);
                else if (const auto* wt = reg.try_get<ecs::WorldTransform>(who)) at = glm::vec3(wt->world[3]);
                at.y += name == "body_fall" ? 0.2f : 1.1f;   // the floor, or the hands at the chest
                ecs::AnimEvent heard = e;   // the player's arms' events are the player's own
                heard.entity = animSoundOwner(world, e.entity, listener.playerId);
                play(world, it->second, planAnimSound(name, heard, at, listener), listener);
            }
        }
    }
}

void CombatSounds::play(ecs::World& world, const std::string& path, SoundPlan plan, const Listener& listener) {
    if (plan.enemy) {   // one ray per enemy sound started
        ++m_rays;
        plan.shape.occluded = soundOccluded(world, listener, plan.at);
        m_occluded += plan.shape.occluded;
    }
    if (isValidSoundHandle(m_audio->playSoundAt(path, plan.at, SoundCategory::SFX, false, plan.volume, plan.shape)))
        ++m_played;
}

} // namespace ohao::runtime
