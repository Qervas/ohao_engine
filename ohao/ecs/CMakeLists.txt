# The core module, "ecs" (ecs/module.hpp): the World, its events, snapshots, scene files, and (until it has its own
# module directory) the vfx system, with its library below it; it reads the visual and physics modules (ohao_visual,
# ohao_physics: exceptions in cmake/ohao_layers.cmake until it leaves).
ohao_add_module(ecs
    LINKS ohao_core ohao_asset ohao_cook ohao_reflect ohao_vfx glm EnTT::EnTT ohao_unordered_dense)
target_link_libraries(ohao_ecs PUBLIC ohao_visual ohao_physics)
target_link_libraries(ohao_ecs PRIVATE ohao_simdjson)
