# The core module, "ecs" (ecs/module.hpp): the World, its events, snapshots, scene files, and (until each has its own
# module directory) the systems of physics, animation, the audio schedule and vfx, with their backends below it;
# those read the visual module's components and primitive meshes (ohao_visual: an exception in cmake/ohao_layers.cmake
# until they leave).
ohao_add_module(ecs
    LINKS ohao_core ohao_asset ohao_cook ohao_reflect ohao_physics ohao_anim ohao_vfx glm EnTT::EnTT ohao_unordered_dense)
target_link_libraries(ohao_ecs PUBLIC ohao_visual)
target_link_libraries(ohao_ecs PRIVATE ohao_simdjson)
# The audio schedule reads the world's .audio.json (the bank, plain data: no miniaudio).
target_link_libraries(ohao_ecs PRIVATE ohao_audio_bank)
