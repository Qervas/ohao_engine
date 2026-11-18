#pragma once
// The animation module, "animation": skeletons, clips and animation graphs (animation/skeleton.hpp, anim_graph.hpp,
// gltf_anim.hpp, anim_cache.hpp: its pure core, no World), the systems that step and pose them
// (animation/animation.hpp: the `animation` simulate and `pose` sync systems), their components
// (animation/components.hpp), the clip events (animation/events.hpp) and animation.info. A game's drivers are its
// contributions.
#include "ecs/module.hpp"

namespace ohao::ecs {

[[nodiscard]] const Module& animationModule();
// Its registrations (animation/types.cpp) and methods (animation/methods.cpp).
void registerAnimationTypes(reflect::TypeRegistry& types);
[[nodiscard]] std::span<const Method> animationMethods();

} // namespace ohao::ecs
