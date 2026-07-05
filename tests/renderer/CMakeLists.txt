# OHAO Renderer Standalone Tests
# Run the Vulkan renderer WITHOUT Godot — pure C++ verification.

# Standalone renderer test — builds scene, renders to PNG
add_executable(renderer_test renderer_test.cpp)

target_include_directories(renderer_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ohao
    ${Vulkan_INCLUDE_DIRS}
    ${stb_SOURCE_DIR}
)

target_link_libraries(renderer_test PRIVATE
    # Wrap in group to resolve circular deps between static libs (GCC)
    -Wl,--start-group
    ohao_renderer
    ohao_gpu_vulkan
    ohao_scene
    ohao_physics
    ohao_audio
    ohao_core
    -Wl,--end-group
    ${Vulkan_LIBRARIES}
    glm
    VulkanMemoryAllocator
)

if(TARGET Jolt)
    target_link_libraries(renderer_test PRIVATE Jolt)
endif()

# Allow duplicate stbi symbols (model_gltf.cpp defines STB_IMAGE_IMPLEMENTATION)
if(MSVC)
    target_link_options(renderer_test PRIVATE /FORCE:MULTIPLE)
else()
    target_link_options(renderer_test PRIVATE -Wl,--allow-multiple-definition)
endif()

# Copy shaders next to exe so renderer can find them
add_custom_command(TARGET renderer_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:renderer_test>/bin/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_BINARY_DIR}/shaders"
        "$<TARGET_FILE_DIR:renderer_test>/bin/shaders"
    COMMENT "Copying shader SPVs for standalone renderer test"
)

message(STATUS "Standalone renderer test configured")

# Env CDF unit test
add_executable(env_cdf_test env_cdf_test.cpp
    ${CMAKE_SOURCE_DIR}/ohao/render/rt/env_cdf.cpp
)
target_include_directories(env_cdf_test PRIVATE ${CMAKE_SOURCE_DIR}/ohao)
target_link_libraries(env_cdf_test PRIVATE gtest gtest_main pthread)
include(GoogleTest)
gtest_discover_tests(env_cdf_test)

# Sobol + Owen scramble unit tests (Feature 1.2)
add_executable(sobol_test sobol_test.cpp
    ${CMAKE_SOURCE_DIR}/ohao/render/rt/sobol_generator.cpp
    ${CMAKE_SOURCE_DIR}/ohao/render/rt/owen_scramble.cpp
)
target_include_directories(sobol_test PRIVATE ${CMAKE_SOURCE_DIR}/ohao)
target_link_libraries(sobol_test PRIVATE gtest gtest_main pthread)
include(GoogleTest)
gtest_discover_tests(sobol_test)

# Denoise parse unit test (Denoiser Sub-plan 1)
add_executable(denoise_parse_test denoise_parse_test.cpp
    ${CMAKE_SOURCE_DIR}/ohao/render/rt/denoise/denoise_types.cpp
)
target_include_directories(denoise_parse_test PRIVATE ${CMAKE_SOURCE_DIR}/ohao)
target_link_libraries(denoise_parse_test PRIVATE gtest gtest_main pthread)
include(GoogleTest)
gtest_discover_tests(denoise_parse_test)
