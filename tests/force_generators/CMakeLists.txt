# OHAO Force Generator Tests
# Unit tests for ForceVolume, Wind, Buoyancy, Spring forces, ForceRegistry, AABB

add_executable(force_generator_tests
    force_generator_tests.cpp
)

target_include_directories(force_generator_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/ohao
)

# ohao_physics contains all force generator and rigid body implementations
target_link_libraries(force_generator_tests PRIVATE
    ohao_physics
    ohao_component
    ohao_actor
    ohao_scene
    ohao_core
    -Wl,--start-group
    ohao_renderer
    ohao_asset
    ${Vulkan_LIBRARIES}
    glm
)

if(TARGET Jolt)
    target_link_libraries(force_generator_tests PRIVATE Jolt)
endif()

add_test(NAME ForceGeneratorTests COMMAND force_generator_tests)

message(STATUS "Force generator tests configured")
