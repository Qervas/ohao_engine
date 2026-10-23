#pragma once
// The scene view: what a world says about how it is looked at (its Camera) and lit by
// an image (its Environment), resolved into plain values a host applies to its
// renderer (runtime/scene_view_apply.hpp). ohao_ecs knows nothing of the renderer camera.
#include "ecs/components.hpp"
#include "ecs/world.hpp"

#include <optional>

namespace ohao::ecs {

struct CameraView {
    glm::vec3 position{0.0f};
    // The renderer camera's Euler angles, in degrees, from the view direction (the
    // entity's world -Z): pitch = asin(dir.y), yaw = atan2(dir.z, dir.x). Looking down
    // -Z is exactly (0, -90). Computed in double and rounded once, so axis-aligned
    // directions give exact angles.
    float pitchDeg{0.0f};
    float yawDeg{-90.0f};
    float fovDeg{45.0f};
    float nearPlane{0.1f};
    float farPlane{100.0f};
};

struct EnvironmentView {
    std::optional<AssetRef> hdri;
    float intensity{1.0f};
};

struct SceneView {
    std::optional<CameraView> camera;             // none: no Camera, or it is inactive
    std::optional<EnvironmentView> environment;   // none: no Environment
};

// The world's Camera and Environment (each a singleton; with several, the first in
// creation order wins). The camera pose is the entity's world matrix computed from
// Transform and Parent exactly as the transform system does, so it is correct before
// the first update.
[[nodiscard]] SceneView resolveSceneView(const World& world);

// Pitch/yaw (degrees, renderer convention) of a view direction; see CameraView.
[[nodiscard]] glm::vec2 pitchYawDegrees(const glm::vec3& direction);

} // namespace ohao::ecs
