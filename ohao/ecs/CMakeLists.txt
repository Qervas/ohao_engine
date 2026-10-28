file(GLOB_RECURSE ECS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE ECS_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
add_library(ohao_ecs STATIC ${ECS_SOURCES} ${ECS_HEADERS})
target_include_directories(ohao_ecs PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_ecs PUBLIC cxx_std_20)
# ohao_physics: the physics system's bodies (physics_scene.cpp). It links only core, glm and
# Jolt, so there is no cycle back to the ECS.
target_link_libraries(ohao_ecs PUBLIC ohao_core ohao_renderer ohao_scene ohao_reflect ohao_physics ohao_anim glm EnTT::EnTT
    ohao_unordered_dense)
target_link_libraries(ohao_ecs PRIVATE ohao_simdjson)
# The audio schedule reads the world's .audio.json (the bank, plain data: no miniaudio).
target_link_libraries(ohao_ecs PRIVATE ohao_audio_bank)
# The vfx system reads the world's .vfx.json and writes spawn records (sub-project 12); its record
# type names the set's types, so the link is public.
target_link_libraries(ohao_ecs PUBLIC ohao_vfx)
