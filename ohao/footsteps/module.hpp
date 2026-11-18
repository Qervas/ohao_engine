#pragma once
// The footsteps module, "footsteps" (one feature, its own module: it depends on audio, animation and physics, and
// none of them knows it): a character's steps and landings sound on the surface it walks (footsteps/footsteps.hpp). It
// adds a sound source to the audio schedule (AudioSchedule::addSource): no system, component or event of its own.
#include "ecs/module.hpp"

namespace ohao::ecs {

[[nodiscard]] const Module& footstepsModule();

} // namespace ohao::ecs
