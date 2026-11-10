# The world into the renderer (system design 4.1, step 3a): instantiate (MeshSources into meshes), the render
# extract, the effects' draw, in-memory model spawning, framing and the HUD's list. The world (ohao_ecs) does not
# link the renderer; this does, both.
file(GLOB WORLD_RENDER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
add_library(ohao_world_render STATIC ${WORLD_RENDER_SOURCES})
target_include_directories(ohao_world_render PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_world_render PUBLIC cxx_std_20)
target_link_libraries(ohao_world_render PUBLIC ohao_ecs ohao_renderer ohao_gpu_vulkan)
