# OHAO renderer tests

# renderer_test: builds a scene headless and renders it to PNG
add_executable(renderer_test renderer_test.cpp)
target_compile_features(renderer_test PRIVATE cxx_std_20)

target_include_directories(renderer_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ohao
    ${Vulkan_INCLUDE_DIRS}
    ${stb_SOURCE_DIR}
)

if(MSVC)
    target_link_libraries(renderer_test PRIVATE
        ohao_ecs ohao_renderer ohao_gpu_vulkan ohao_scene ohao_physics ohao_audio ohao_core
        ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator)
else()
    target_link_libraries(renderer_test PRIVATE
        -Wl,--start-group
        ohao_ecs ohao_renderer ohao_gpu_vulkan ohao_scene ohao_physics ohao_audio ohao_core
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
# a second consumer don't race two independent copy_directory commands into
# the same destination under a parallel (-j8) build (Task 5). The explicit
# dependency also fixes a pre-existing ordering gap: previously nothing
# guaranteed renderer_test's own copy ran after shaders had been (re)built,
# so building renderer_test alone could copy stale or missing SPVs.
add_dependencies(renderer_test shaders)

message(STATUS "Standalone renderer test configured")

# Instantiate on a real RenderScene vs spawnModel: bit-identical deferred images, mesh
# release, validation clean (sub-project 3, step 7).
add_executable(instantiate_gpu_test instantiate_gpu_test.cpp)
target_compile_features(instantiate_gpu_test PRIVATE cxx_std_20)
target_include_directories(instantiate_gpu_test PRIVATE ${CMAKE_SOURCE_DIR}/ohao ${Vulkan_INCLUDE_DIRS})
target_compile_definitions(instantiate_gpu_test PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(instantiate_gpu_test PRIVATE
    ohao_ecs ohao_renderer ohao_gpu_vulkan ohao_scene ohao_physics ohao_audio ohao_core
    ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator)
if(TARGET Jolt)
    target_link_libraries(instantiate_gpu_test PRIVATE Jolt)
endif()
if(MSVC)
    target_link_options(instantiate_gpu_test PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
add_dependencies(instantiate_gpu_test shaders)

# The scene view and the example scenes on the GPU: Camera/Environment entities and a
# saved-and-loaded scene render bit-identically to the hand-set view (sub-project 3, step 8).
add_executable(scene_view_gpu_test scene_view_gpu_test.cpp)
target_compile_features(scene_view_gpu_test PRIVATE cxx_std_20)
target_include_directories(scene_view_gpu_test PRIVATE ${CMAKE_SOURCE_DIR}/ohao ${CMAKE_SOURCE_DIR}/examples
    ${CMAKE_SOURCE_DIR}/tools ${Vulkan_INCLUDE_DIRS})
target_compile_definitions(scene_view_gpu_test PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(scene_view_gpu_test PRIVATE
    ohao_ecs ohao_renderer ohao_gpu_vulkan ohao_scene ohao_physics ohao_audio ohao_core
    ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator)
if(TARGET Jolt)
    target_link_libraries(scene_view_gpu_test PRIVATE Jolt)
endif()
if(MSVC)
    target_link_options(scene_view_gpu_test PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
add_dependencies(scene_view_gpu_test shaders)

# Snapshot/restore on the GPU through the CLI's stack (api::Session + RenderHost): a
# restored world renders bit-identically and uploads only what changed (sub-project 3, step 9).
add_executable(restore_render_gpu_test restore_render_gpu_test.cpp)
target_compile_features(restore_render_gpu_test PRIVATE cxx_std_20)
target_compile_definitions(restore_render_gpu_test PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    OHAO_BINARY_DIR="${CMAKE_BINARY_DIR}")
target_link_libraries(restore_render_gpu_test PRIVATE ohao_host)
if(MSVC)
    target_link_options(restore_render_gpu_test PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
add_dependencies(restore_render_gpu_test shaders)

# Deferred rendering is bit-identical across fresh renderers and processes, post effects
# included, and the render seed reaches SSAO (sub-project 4, step 1).
add_executable(deferred_determinism_gpu_test deferred_determinism_gpu_test.cpp)
target_compile_features(deferred_determinism_gpu_test PRIVATE cxx_std_20)
target_compile_definitions(deferred_determinism_gpu_test PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    OHAO_BINARY_DIR="${CMAKE_BINARY_DIR}")
target_link_libraries(deferred_determinism_gpu_test PRIVATE ohao_host)
if(MSVC)
    target_link_options(deferred_determinism_gpu_test PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
add_dependencies(deferred_determinism_gpu_test shaders)

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
