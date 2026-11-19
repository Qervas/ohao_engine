// The navigation module's component storages, instantiated once (ecs/storage.hpp: every other unit calls these).
#include "navigation/components.hpp"

OHAO_STORAGE_INSTANCE(ohao::ecs::NavAgent);
OHAO_STORAGE_INSTANCE(ohao::ecs::NavLink);
OHAO_STORAGE_INSTANCE(ohao::ecs::NavObstacle);
OHAO_STORAGE_INSTANCE(ohao::ecs::NavPath);
