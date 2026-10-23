# The ohao CLI (sub-project 3, step 9): schema, validate, fmt, apply, query, summary, run,
# doctor and serve over ohao_api, plus ohao_host: the headless render host behind
# render.capture, shared with the GPU tests. The window host is ohao_runtime's LiveHost.
add_library(ohao_host STATIC render_host.cpp render_host.hpp)
target_compile_features(ohao_host PUBLIC cxx_std_20)
target_include_directories(ohao_host PUBLIC ${CMAKE_SOURCE_DIR}/ohao ${CMAKE_SOURCE_DIR}/tools
    ${Vulkan_INCLUDE_DIRS})
target_link_libraries(ohao_host PUBLIC
    ohao_runtime ohao_api ohao_ecs ohao_renderer ohao_gpu_vulkan ohao_scene ohao_physics ohao_audio ohao_core
    ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator)
if(TARGET Jolt)
    target_link_libraries(ohao_host PUBLIC Jolt)
endif()

add_executable(ohao main.cpp doctor.cpp doctor.hpp)
target_compile_features(ohao PRIVATE cxx_std_20)
target_link_libraries(ohao PRIVATE ohao_host ohao_rpc)
if(MSVC)
    target_link_options(ohao PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
add_dependencies(ohao shaders)
