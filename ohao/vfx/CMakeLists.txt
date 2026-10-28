# ohao_vfx (sub-project 12): the effect set (.vfx.json) and the closed form every particle is drawn
# from. No ECS and no Vulkan: core, glm, and simdjson privately for JSON. The ECS's `vfx` system
# (ecs/vfx.cpp) writes spawn records from it; the renderer evaluates the same closed form on the GPU.
file(GLOB VFX_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB VFX_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
add_library(ohao_vfx STATIC ${VFX_SOURCES} ${VFX_HEADERS})
target_include_directories(ohao_vfx PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_vfx PUBLIC cxx_std_20)
target_link_libraries(ohao_vfx PUBLIC ohao_core glm PRIVATE ohao_simdjson)
