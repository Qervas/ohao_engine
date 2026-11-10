#pragma once
#include "ecs/world.hpp"

namespace ohao { struct FrameResult; }

namespace ohao::ecs {

// Spawns a SceneFramer FrameResult's lights (render/camera/scene_framer.hpp) as
// point-light entities, in list order.
void spawnFramedLights(World& world, const FrameResult& frame);

} // namespace ohao::ecs
