// The animation module's component storages, instantiated once (ecs/storage.hpp: every other unit calls these).
#include "animation/components.hpp"
#include "animation/animation.hpp"

OHAO_STORAGE_INSTANCE(ohao::ecs::Animator);
OHAO_STORAGE_INSTANCE(ohao::ecs::JointAttach);
OHAO_STORAGE_INSTANCE(ohao::ecs::SkinnedMesh);
OHAO_STORAGE_INSTANCE(ohao::ecs::AnimParams);
OHAO_STORAGE_INSTANCE(ohao::ecs::AnimPose);
