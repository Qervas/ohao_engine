# OHAO Renderer Pipeline Tests
# Tests all Phase 1-5 AAA renderer features

add_executable(renderer_pipeline_tests
    renderer_pipeline_tests.cpp
)

target_include_directories(renderer_pipeline_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(renderer_pipeline_tests PRIVATE
    -Wl,--start-group
    ohao_renderer
    ohao_scene
    ohao_actor
    ohao_asset
    ohao_component
    ohao_physics
    ohao_ui
    ohao_utils
    -Wl,--end-group
    ${Vulkan_LIBRARIES}
    glm
)

# Add test to CTest
add_test(NAME RendererPipelineTests COMMAND renderer_pipeline_tests)

message(STATUS "Renderer pipeline tests configured")
