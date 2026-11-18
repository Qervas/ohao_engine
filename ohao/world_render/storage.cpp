// The world_render module's component storages, instantiated once (ecs/storage.hpp: every other unit calls these).
#include "world_render/components.hpp"
#include "world_render/private/components.hpp"

OHAO_STORAGE_INSTANCE(ohao::ecs::MeshRenderer);
OHAO_STORAGE_INSTANCE(ohao::ecs::InstantiateDirty);
OHAO_STORAGE_INSTANCE(ohao::ecs::RenderSlot);
