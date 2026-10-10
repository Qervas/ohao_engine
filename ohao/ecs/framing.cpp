#include "ecs/framing.hpp"
#include "ecs/spawn.hpp"
#include "render/camera/scene_framer.hpp"

namespace ohao::ecs {

void spawnFramedLights(World& world, const FrameResult& frame) {
    for (const auto& l : frame.lights) {
        Light light;
        light.type = LightType::Point;
        light.color = l.color;
        light.intensity = l.intensity;
        light.radius = l.radius;
        Transform t;
        t.position = l.position;
        spawnLight(world, l.name, light, t);
    }
}

} // namespace ohao::ecs
