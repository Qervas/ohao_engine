# OHAO Engine Tests
# Tests EventBus, CommandHistory, meta/traits, headless renderer and RenderScene

add_executable(engine_tests
    engine_tests.cpp
)

target_include_directories(engine_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/ohao
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_features(engine_tests PRIVATE cxx_std_20)

if(MSVC)
    target_link_options(engine_tests PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()

if(MSVC)
    target_link_libraries(engine_tests PRIVATE
        ohao_runtime ohao_ecs ohao_scene ohao_physics ohao_core ohao_renderer ${Vulkan_LIBRARIES} glm)
else()
    target_link_libraries(engine_tests PRIVATE
        ohao_ecs ohao_scene ohao_physics ohao_core
        -Wl,--start-group ohao_renderer ${Vulkan_LIBRARIES} glm)
endif()

if(TARGET Jolt)
    target_link_libraries(engine_tests PRIVATE Jolt)
endif()
target_link_libraries(engine_tests PRIVATE ohao_test_harness)
add_dependencies(engine_tests shaders)   # it renders headless frames

ohao_add_test(engine_tests LABEL gpu GPU COMMAND engine_tests)

message(STATUS "Engine tests configured")
