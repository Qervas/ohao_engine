// The audio module's component storages, instantiated once (ecs/storage.hpp: every other unit calls these).
#include "audio/components.hpp"

OHAO_STORAGE_INSTANCE(ohao::ecs::AudioMix);
OHAO_STORAGE_INSTANCE(ohao::ecs::ReverbZone);
