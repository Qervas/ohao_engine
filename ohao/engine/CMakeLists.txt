# The engine's module list and their one install (system design 4.2; engine/install.hpp): every host registers the
# modules' types and installs them (engine::install), again with its renderer once it has one (world_render).
add_library(ohao_engine STATIC install.cpp install.hpp)
target_include_directories(ohao_engine PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_engine PUBLIC cxx_std_20)
target_link_libraries(ohao_engine PUBLIC ohao_ecs ohao_visual ohao_animation ohao_physics ohao_audio ohao_footsteps
    ohao_vfx ohao_level ohao_world_render)
