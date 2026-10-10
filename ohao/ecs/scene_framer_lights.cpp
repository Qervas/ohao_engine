// SceneFramer::applyLights lives in ohao_ecs: it spawns entities, and ohao_renderer
// (where the rest of SceneFramer is) must not depend on the ECS.
#include "render/camera/scene_framer.hpp"
#include "ecs/spawn.hpp"

namespace ohao {

void SceneFramer::applyLights(ecs::World& world, const FrameResult& result) {
    for (const auto& l : result.lights) {
        ecs::Light light;
        light.type = ecs::LightType::Point;
        light.color = l.color;
        light.intensity = l.intensity;
        light.radius = l.radius;
        ecs::Transform t;
        t.position = l.position;
        ecs::spawnLight(world, l.name, light, t);
    }
}

} // namespace ohao
