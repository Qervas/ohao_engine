add_executable(ecs_tests ecs_tests.cpp)
target_compile_features(ecs_tests PRIVATE cxx_std_20)
target_link_libraries(ecs_tests PRIVATE ohao_ecs ohao_level_world ohao_gpu_vulkan ohao_test_harness)
target_compile_definitions(ecs_tests PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")   # the level fixtures
if(MSVC)
    target_link_options(ecs_tests PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
add_dependencies(ecs_tests shaders)   # the render-extract tests make a headless renderer
ohao_add_test(ecs_tests LABEL gpu GPU COMMAND ecs_tests)
