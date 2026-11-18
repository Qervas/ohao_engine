# The core module, "ecs" (ecs/module.hpp): the World, its events, snapshots, scene files and the core's systems.
ohao_add_module(ecs LINKS ohao_core ohao_asset ohao_cook ohao_reflect glm EnTT::EnTT ohao_unordered_dense)
target_link_libraries(ohao_ecs PRIVATE ohao_simdjson)
