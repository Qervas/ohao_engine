#pragma once
// Applies a world's Camera and Environment (ecs::resolveSceneView) to a VulkanRenderer
// through its camera and environment setters. Shared by the examples, the headless
// RenderHost and the window's LiveHost. Header-only.
#include "ecs/asset_resolver.hpp"
#include "ecs/scene_view.hpp"
#include "render/renderer/vulkan_renderer.hpp"
#include "render/camera/camera.hpp"

#include <string>
#include <vector>

namespace ohao::runtime {

struct SceneViewApplied {
    bool camera{false};
    bool environment{false};
    std::vector<std::string> warnings;   // e.g. an hdri path the resolver rejects
};

// Leaves the renderer's camera alone when the world has no active Camera. The environment is
// the world's Environment, and a world without one has the default's: the physical sky lit by
// the scene's sun. Removing an hdri unloads it (the renderer reloads what the key says). The camera keeps its aspect ratio (the render target's). This does
// not reset path-tracer accumulation: a host that changes the view between frames calls
// renderer.notifyCameraChanged().
inline SceneViewApplied applySceneView(VulkanRenderer& renderer, const ecs::SceneView& view,
                                       const ecs::AssetResolver& assets) {
    SceneViewApplied applied;
    if (view.camera) {
        const ecs::CameraView& c = *view.camera;
        Camera& cam = renderer.getCamera();
        cam.setPosition(c.position);
        cam.setPerspectiveProjection(c.fovDeg, cam.getAspectRatio(), c.nearPlane, c.farPlane);
        cam.setRotation(c.pitchDeg, c.yawDeg);
        applied.camera = true;
    }
    const ecs::EnvironmentView env = view.environment.value_or(ecs::EnvironmentView{});
    std::string hdri;
    if (env.hdri) {
        const auto path = assets.absolutePath(*env.hdri);
        if (path.empty())
            applied.warnings.push_back("environment hdri '" + env.hdri->path +
                                       "' is not a relative path inside the project; not loaded");
        else
            hdri = path.string();
    }
    renderer.setEnvironmentMap(hdri);
    renderer.setEnvironmentSky(env.sky);
    renderer.setEnvIntensityScale(env.intensity);
    applied.environment = view.environment.has_value();
    return applied;
}

} // namespace ohao::runtime
