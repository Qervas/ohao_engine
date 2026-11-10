#pragma once
// Animation in the world (sub-project 10, docs/specs/2026-10-28-animation-design.md, "The step"
// and "The frame").
//
// installAnimation adds:
//   - the `animation` simulate system (after the systems installed before it: physics, combat):
//     for each Animator in id order, its driver (if its graph names one) writes AnimParams from
//     saved state; then each layer in order takes the first transition that holds, advances one
//     step, and emits its clip events (`anim.clip_event`, the World's log: ecs/events.hpp). Nothing in
//     the step reads a pose.
//   - the `pose` sync system (before render_extract, after view_model / interpolate): samples
//     every Animator at (steps - 1 + alpha) into AnimPose (local, model and the skinning palette,
//     the previous drawn frame's kept for motion vectors) and writes each JointAttach child's
//     RenderPose. Presentation: never saved, hashed or seen by a step. A placement (a restore, a
//     first pose) resets the previous palette to the current one.
#include "anim/anim_cache.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/world.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::ecs {

// The drivers' parameter values and events for this step (runtime: rebuilt every step by the
// graph's driver from saved state; a graph without a driver keeps what was set, as tests do).
struct AnimParams {
    std::vector<std::pair<std::string, float>> values;
    std::vector<std::string> events;   // this step's; cleared after the step
    void set(std::string_view name, float value);
};

// The drawn pose (runtime, presentation).
struct AnimPose {
    std::shared_ptr<const anim::BoundGraph> graph;
    std::vector<glm::mat4> model;           // joint matrices in the skeleton's space, this frame
    std::vector<glm::mat4> palette;         // skinning palette in the mesh's bone order, this frame
    std::vector<glm::mat4> prevPalette;     // the previous drawn frame's (= palette after a placement)
    std::vector<int> boneToJoint;           // the mesh's bones to the skeleton's joints
    // Per bone, the bind-pose box of the vertices it holds (any weight); the last entry, the
    // vertices no bone holds. The posed box is the union of each through its palette matrix:
    // a skinned vertex is a weighted mean of its bones' images, so it lies inside.
    std::vector<std::pair<glm::vec3, glm::vec3>> boneBoxes;
    glm::vec3 boundsMin{0.0f}, boundsMax{0.0f};   // this frame's posed local box (the draws' culling box)
    uint64_t frame{0};                      // pose syncs run so far for this entity
};

// A clip event a layer's clip fired in the step (`anim.clip_event`): mag_out, foot_l, body_fall, ...
inline constexpr const char* kClipEventChannel = "anim.clip_event";
struct ClipEvent {
    uint64_t entity{0};     // the animated entity's id
    uint32_t layer{0};      // the graph layer
    std::string clip;       // the clip that fired it
    std::string name;       // the event's name in the clip
};

using AnimDriver = std::function<void(const World&, entt::entity, AnimParams&)>;

class AnimationSystem {
public:
    explicit AnimationSystem(AssetResolver& assets);
    void setEvents(EventLog& log);   // the channel it emits into (installAnimation: the World's)

    // The graph a path names (cached); nullptr with the error logged once per path.
    [[nodiscard]] std::shared_ptr<const anim::BoundGraph> graph(const std::string& path);
    [[nodiscard]] anim::AnimCache& cache() noexcept { return m_cache; }

    void setDriver(std::string name, AnimDriver fn);   // replaces a driver of that name
    void removeDriver(std::string_view name);
    [[nodiscard]] std::vector<std::string> driverNames() const;

    void step(World& world);      // the simulate system
    void pose(World& world);      // the sync system

    [[nodiscard]] double lastStepMs() const noexcept { return m_lastStepMs; }
    [[nodiscard]] double lastPoseMs() const noexcept { return m_lastPoseMs; }

    // An Animator written from outside the step (a load, an edit, a restore): its next pose is a
    // placement (no motion from the previous one).
    void markPlaced(entt::entity e) { m_placed.push_back(e); }

private:
    void poseEntity(World& world, entt::entity e, bool placed);
    void attach(World& world);

    AssetResolver& m_assets;
    anim::AnimCache m_cache;
    std::vector<std::pair<std::string, AnimDriver>> m_drivers;
    std::vector<std::string> m_failed;   // graph paths whose error was logged
    EventWriter<ClipEvent> m_clipEvents;
    double m_lastStepMs{0.0}, m_lastPoseMs{0.0};
    anim::PoseScratch m_scratch;
    anim::LocalPose m_local;
    std::vector<entt::entity> m_placed;
    std::vector<std::pair<uint64_t, entt::entity>> m_order;
    std::unordered_map<entt::entity, glm::mat4> m_drawn;   // JointAttach children drawn last frame
    std::vector<entt::entity> m_seen;
};

// Installs the systems once per World (a second call returns the first); the AssetResolver must
// outlive the World. A game registers its drivers when it installs (the slice: slice/arms_anim.hpp, enemy_anim.hpp).
AnimationSystem& installAnimation(World& world, AssetResolver& assets);
[[nodiscard]] AnimationSystem* animationSystem(World& world);
[[nodiscard]] const AnimationSystem* animationSystem(const World& world);

} // namespace ohao::ecs
