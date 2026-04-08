# OHAO Physics Backend Tests
# Tests all 7 Jolt Physics features

add_executable(physics_backend_tests
    physics_backend_tests.cpp
)

target_include_directories(physics_backend_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

# ohao_physics transitively depends on component, actor, UI, scene libs
target_link_libraries(physics_backend_tests PRIVATE
    ohao_physics
    ohao_component
    ohao_actor
    ohao_scene
    ohao_ui
    ohao_renderer
    ohao_asset
    ${Vulkan_LIBRARIES}
    glm
)

# Link Jolt if available
if(TARGET Jolt)
    target_link_libraries(physics_backend_tests PRIVATE Jolt)
endif()

# Add test to CTest
add_test(NAME PhysicsBackendTests COMMAND physics_backend_tests)

message(STATUS "Physics backend tests configured")
