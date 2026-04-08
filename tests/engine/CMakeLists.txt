# OHAO Engine Tests
# Tests EventBus, CommandHistory, Scene, and SceneSerializer

add_executable(engine_tests
    engine_tests.cpp
)

target_include_directories(engine_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/ohao
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(engine_tests PRIVATE
    ohao_events
    ohao_commands
    ohao_scene
    ohao_physics
    ohao_component
    ohao_actor
    ohao_ui
    ohao_renderer
    ohao_asset
    ${Vulkan_LIBRARIES}
    glm
)

if(TARGET Jolt)
    target_link_libraries(engine_tests PRIVATE Jolt)
endif()

add_test(NAME EngineTests COMMAND engine_tests)

message(STATUS "Engine tests configured")
