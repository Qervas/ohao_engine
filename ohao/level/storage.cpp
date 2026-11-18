// The level module's component storages, instantiated once (ecs/storage.hpp: every other unit calls these).
#include "level/components.hpp"

OHAO_STORAGE_INSTANCE(ohao::ecs::Level);
OHAO_STORAGE_INSTANCE(ohao::ecs::LevelDirty);
OHAO_STORAGE_INSTANCE(ohao::ecs::LevelNode);
OHAO_STORAGE_INSTANCE(ohao::ecs::Socket);
