#pragma once
// The first-person gun (sub-project 9 spec, "The gun: placement and procedural motion"). An entity
// with a ViewModel, below the entity holding the view's Camera, is drawn with its subtree remapped in
// view space (viewModelRemap: toward the eye by depthScale, as if through a fovDeg lens) and moved at
// render rate by sway (the drawn view's turn, lagged), bob (the player's walk) and kick (its rounds,
// from the combat log). The `view_model` sync system (after `interpolate`, so it sees the look lead's
// drawn view; before `render_extract`) writes only RenderPose. Its motion state is a runtime holder in
// the registry's context: never saved, hashed or snapshotted. A frame without presentation time
// (World::presentationDt 0: lockstep, headless, every capture) draws the rest pose, the remap alone,
// and resets the motion; a round's restart resets it too.
#include "slice/components.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>

namespace ohao::slice {

// The drawn view-space pose of a view-space pose: x and y shrunk by depthScale, z by depthScale times
// tan(fovDeg / 2) / tan(sceneFovDeg / 2). Every point stays within depthScale of its distance from the
// eye, and its projection through sceneFovDeg is the original's through fovDeg.
[[nodiscard]] glm::mat4 viewModelRemap(const glm::mat4& view, float sceneFovDeg, float fovDeg, float depthScale);

// A gun's motion as the last frame left it (degrees and metres, view space).
struct ViewModelMotion {
    float swayYawDeg{0.0f};     // + turns the gun left (it lags a turn right)
    float swayPitchDeg{0.0f};
    float bobPhase{0.0f};       // cycles walked (one per bobStride)
    float bobWeight{0.0f};      // 0 standing or in the air, 1 walking
    float kickBack{0.0f};       // m toward the eye
    float kickPitchDeg{0.0f};   // up
    uint64_t rounds{0};         // the owner's rounds seen (kicks given)
};

// Installs the `view_model` sync system: after `interpolate` (or `transform` without physics), before
// `render_extract` when it is installed. Idempotent. installCombat calls it.
void installViewModel(World& world);
// The motion of a ViewModel entity after the last sync (nullptr if it has none yet).
[[nodiscard]] const ViewModelMotion* viewModelMotion(const World& world, entt::entity gun);

} // namespace ohao::slice
