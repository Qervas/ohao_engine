#include "footsteps/footsteps.hpp"

#include "animation/animation.hpp"
#include "animation/components.hpp"
#include "animation/events.hpp"
#include "audio/audio_bank.hpp"
#include "audio/audio_schedule.hpp"
#include "ecs/world.hpp"
#include "physics/components.hpp"
#include "physics/events.hpp"
#include "physics/physics_scene.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace ohao::ecs {

float footstepScale(bool landing, float speed) noexcept {
    return landing ? std::clamp(speed / 6.0f, 0.4f, 1.0f) : std::clamp(0.25f + 0.1f * speed, 0.2f, 1.0f);
}

std::string footstepSound(const AudioBank& bank, std::string_view surface) {
    const std::string own = "footstep_" + std::string(surface.empty() ? std::string_view("default") : surface);
    if (bank.sound(own) >= 0) return own;
    if (bank.sound("footstep_default") >= 0) return "footstep_default";
    return {};
}

namespace {

constexpr std::string_view kFeet[] = {"foot_l", "foot_r"};

struct Footsteps {
    EventReader<FootEvent> steps, landings;   // the schedule's readers (its system's name)
    std::map<std::string, bool> footGraphs;   // a graph path: whether its clips fire foot_l or foot_r

    // Whether a character's graph plays its footsteps from clip events.
    bool footFromClips(SoundStep& s, entt::entity character) {
        const auto* animator = s.reg.try_get<Animator>(character);
        if (!animator || animator->graph.path.empty()) return false;
        const auto it = footGraphs.find(animator->graph.path);
        if (it != footGraphs.end()) return it->second;
        const bool has = graphFires(s.world, animator->graph.path, kFeet);
        footGraphs[animator->graph.path] = has;
        return has;
    }

    // Steps and landings in the order physics emitted them (a restart in the step ended what came before).
    void physicsFeet(SoundStep& s) {
        const auto st = steps.read(), lands = landings.read();
        for (std::size_t i = 0, j = 0; i < st.size() || j < lands.size();) {
            const bool land = j < lands.size() && (i == st.size() || lands[j].meta.seq < st[i].meta.seq);
            const FootEvent& e = land ? lands[j++].value : st[i++].value;
            if (!land) {
                const entt::entity who = s.world.findById(e.player);
                if (who != entt::null && footFromClips(s, who)) continue;   // its clip's feet play its steps
            }
            const std::string sound = footstepSound(s.bank, e.surface);
            if (!sound.empty()) s.play({sound, e.player, e.point, false, footstepScale(land, e.speed)});
        }
    }

    // A clip's foot: a step on the character's ground.
    bool clipFoot(SoundStep& s, const ClipEvent& e) {
        if (std::find(std::begin(kFeet), std::end(kFeet), e.name) == std::end(kFeet)) return false;
        const entt::entity who = s.world.findById(e.entity);
        if (who == entt::null) return true;
        const uint64_t owner = s.ownerOf(e.entity);
        const glm::vec3 at = glm::vec3(s.worldOf(who)[3]);
        const entt::entity character = s.characterOf(who);
        const PhysicsScene* physics = physicsScene(s.world);
        const auto info = physics && character != entt::null ? physics->playerInfo(character) : std::nullopt;
        const glm::vec3 v = info ? info->velocity : glm::vec3(0.0f);
        const std::string sound = footstepSound(s.bank, info ? info->surface : std::string());
        if (!sound.empty()) s.play({sound, owner, at, false, footstepScale(false, std::sqrt(v.x * v.x + v.z * v.z))});
        return true;
    }
};

} // namespace

void installFootsteps(World& world) {
    AudioSchedule* schedule = audioSchedule(world);
    if (!schedule) return;
    auto feet = std::make_shared<Footsteps>();
    EventLog& log = world.events();
    feet->steps = log.reader<FootEvent>(physics_events::kFootstep, "audio_schedule");
    feet->landings = log.reader<FootEvent>(physics_events::kLand, "audio_schedule");
    // What it reads in the schedule's step: the characters' animators and their graphs, the players.
    world.addAccess("audio_schedule", types<Animator>(), {}, {"animation", "physics.scene"});
    schedule->addSource({"footsteps", [feet](SoundStep& s) { feet->physicsFeet(s); },
                         [feet](SoundStep& s, const ClipEvent& e) { return feet->clipFoot(s, e); }});
}

} // namespace ohao::ecs
