# OHAO Renderer Standalone Tests
# Run the Vulkan renderer WITHOUT Godot — pure C++ verification.

# Standalone renderer test — builds scene, renders to PNG
add_executable(renderer_test renderer_test.cpp)
target_compile_features(renderer_test PRIVATE cxx_std_20)

target_include_directories(renderer_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ohao
    ${Vulkan_INCLUDE_DIRS}
    ${stb_SOURCE_DIR}
)

if(MSVC)
    target_link_libraries(renderer_test PRIVATE
        ohao_renderer ohao_gpu_vulkan ohao_scene ohao_physics ohao_audio ohao_core
        ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator)
else()
    target_link_libraries(renderer_test PRIVATE
        -Wl,--start-group
        ohao_renderer ohao_gpu_vulkan ohao_scene ohao_physics ohao_audio ohao_core
        -Wl,--end-group
        ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator)
endif()

if(TARGET Jolt)
    target_link_libraries(renderer_test PRIVATE Jolt)
endif()

# Allow duplicate stbi symbols (model_gltf.cpp defines STB_IMAGE_IMPLEMENTATION)
if(MSVC)
    target_link_options(renderer_test PRIVATE /FORCE:MULTIPLE)
else()
    target_link_options(renderer_test PRIVATE -Wl,--allow-multiple-definition)
endif()

# renderer_test does not copy shader SPVs itself -- the shaders target owns
# that copy (shaders/CMakeLists.txt) once, so that this target and
# diff_gpu_probe don't race two independent copy_directory commands into
# the same destination under a parallel (-j8) build (Task 5). The explicit
# dependency also fixes a pre-existing ordering gap: previously nothing
# guaranteed renderer_test's own copy ran after shaders had been (re)built,
# so building renderer_test alone could copy stale or missing SPVs.
add_dependencies(renderer_test shaders)

message(STATUS "Standalone renderer test configured")

# Env CDF unit test
add_executable(env_cdf_test env_cdf_test.cpp
    ${CMAKE_SOURCE_DIR}/ohao/render/rt/env_cdf.cpp
)
target_include_directories(env_cdf_test PRIVATE ${CMAKE_SOURCE_DIR}/ohao)
target_link_libraries(env_cdf_test PRIVATE gtest gtest_main)
if(UNIX)
    target_link_libraries(env_cdf_test PRIVATE pthread)
endif()
include(GoogleTest)
gtest_discover_tests(env_cdf_test)

# Sobol + Owen scramble unit tests (Feature 1.2)
add_executable(sobol_test sobol_test.cpp
    ${CMAKE_SOURCE_DIR}/ohao/render/rt/sobol_generator.cpp
    ${CMAKE_SOURCE_DIR}/ohao/render/rt/owen_scramble.cpp
)
target_include_directories(sobol_test PRIVATE ${CMAKE_SOURCE_DIR}/ohao)
target_link_libraries(sobol_test PRIVATE gtest gtest_main)
if(UNIX)
    target_link_libraries(sobol_test PRIVATE pthread)
endif()
include(GoogleTest)
gtest_discover_tests(sobol_test)

# Denoise parse unit test (Denoiser Sub-plan 1)
add_executable(denoise_parse_test denoise_parse_test.cpp
    ${CMAKE_SOURCE_DIR}/ohao/render/rt/denoise/denoise_types.cpp
)
target_include_directories(denoise_parse_test PRIVATE ${CMAKE_SOURCE_DIR}/ohao)
target_link_libraries(denoise_parse_test PRIVATE gtest gtest_main)
if(UNIX)
    target_link_libraries(denoise_parse_test PRIVATE pthread)
endif()
include(GoogleTest)
gtest_discover_tests(denoise_parse_test)
