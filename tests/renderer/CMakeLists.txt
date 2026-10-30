# OHAO renderer tests

# renderer_test: builds a scene headless and renders it to PNG
add_executable(renderer_test renderer_test.cpp)
target_compile_features(renderer_test PRIVATE cxx_std_20)

target_include_directories(renderer_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ohao
    ${Vulkan_INCLUDE_DIRS}
    ${stb_SOURCE_DIR}
)
target_compile_definitions(renderer_test PRIVATE OHAO_BINARY_DIR="${CMAKE_BINARY_DIR}")
target_link_libraries(renderer_test PRIVATE ohao_test_harness)

if(MSVC)
    target_link_libraries(renderer_test PRIVATE
        ohao_ecs ohao_renderer ohao_gpu_vulkan ohao_scene ohao_physics ohao_audio ohao_core
        ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator ohao_test_harness)
else()
    target_link_libraries(renderer_test PRIVATE
        -Wl,--start-group
        ohao_ecs ohao_renderer ohao_gpu_vulkan ohao_scene ohao_physics ohao_audio ohao_core
        -Wl,--end-group
        ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator ohao_test_harness)
endif()

if(TARGET Jolt)
    target_link_libraries(renderer_test PRIVATE Jolt)
endif()

# Allow duplicate stbi symbols (stb implementations in the engine libraries and in this test)
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
    ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator ohao_test_harness)
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
    ${Vulkan_LIBRARIES} glm VulkanMemoryAllocator ohao_test_harness)
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

# GPU culling changes what is drawn, never what is seen; its lists are the same every run
# (sub-project 2b, test 6).
add_executable(cull_parity_gpu_test cull_parity_gpu_test.cpp)
target_compile_features(cull_parity_gpu_test PRIVATE cxx_std_20)
target_compile_definitions(cull_parity_gpu_test PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    OHAO_BINARY_DIR="${CMAKE_BINARY_DIR}")
target_link_libraries(cull_parity_gpu_test PRIVATE ohao_host ohao_test_harness)
if(MSVC)
    target_link_options(cull_parity_gpu_test PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
add_dependencies(cull_parity_gpu_test shaders)

# Clustered lighting sums what the full loop sums; 1,000 lights render, with overflow reported;
# the froxel lists are the same every run (sub-project 2b, test 8).
add_executable(light_cluster_gpu_test light_cluster_gpu_test.cpp)
target_compile_features(light_cluster_gpu_test PRIVATE cxx_std_20)
target_include_directories(light_cluster_gpu_test PRIVATE ${CMAKE_SOURCE_DIR}/tools)   # bench/bench_world.hpp
target_compile_definitions(light_cluster_gpu_test PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(light_cluster_gpu_test PRIVATE ohao_host ohao_test_harness)
if(MSVC)
    target_link_options(light_cluster_gpu_test PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
add_dependencies(light_cluster_gpu_test shaders)

# A light's emitter mesh changes only its own pixels: no shadow, no second count of the light.
add_executable(light_emitter_gpu_test light_emitter_gpu_test.cpp)
target_compile_features(light_emitter_gpu_test PRIVATE cxx_std_20)
target_compile_definitions(light_emitter_gpu_test PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(light_emitter_gpu_test PRIVATE ${stb_SOURCE_DIR})   # stb_image: the PNGs back to pixels
target_link_libraries(light_emitter_gpu_test PRIVATE ohao_host ohao_test_harness)
if(MSVC)
    target_link_options(light_emitter_gpu_test PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
add_dependencies(light_emitter_gpu_test shaders)

# Auto exposure (frontier F2 T2): Cornell metered, every light doubled one EV more, captures at a fixed EV.
add_executable(exposure_gpu_test exposure_gpu_test.cpp)
target_compile_features(exposure_gpu_test PRIVATE cxx_std_20)
target_compile_definitions(exposure_gpu_test PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(exposure_gpu_test PRIVATE ohao_host ohao_test_harness)
if(MSVC)
    target_link_options(exposure_gpu_test PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
add_dependencies(exposure_gpu_test shaders)

# GPU skinning (sub-project 10, test 24): the deformed range against a CPU skin, two poses of one
# mesh, the motion vectors' previous positions, RT and the path tracer seeing the pose, refits.
add_executable(skinning_gpu_test skinning_gpu_test.cpp)
target_compile_features(skinning_gpu_test PRIVATE cxx_std_20)
target_compile_definitions(skinning_gpu_test PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    OHAO_BINARY_DIR="${CMAKE_BINARY_DIR}")
target_link_libraries(skinning_gpu_test PRIVATE ohao_host ohao_test_harness)
if(MSVC)
    target_link_options(skinning_gpu_test PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
add_dependencies(skinning_gpu_test shaders)

# Transparency and lit particles (sub-project 12, test 32): the transparent pass's order, lit and
# soft particles, alpha-tested and blended surfaces in RT, captures independent of the frames before.
add_executable(vfx_gpu_test vfx_gpu_test.cpp)
target_compile_features(vfx_gpu_test PRIVATE cxx_std_20)
target_compile_definitions(vfx_gpu_test PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(vfx_gpu_test PRIVATE ${stb_SOURCE_DIR})   # stb_image: the PNGs back to pixels
target_link_libraries(vfx_gpu_test PRIVATE ohao_host ohao_test_harness)
if(MSVC)
    target_link_options(vfx_gpu_test PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
add_dependencies(vfx_gpu_test shaders)

# The small GoogleTest suites (CPU). ohao_gtest_main prints the harness's JSON line.
add_executable(env_cdf_test env_cdf_test.cpp
    ${CMAKE_SOURCE_DIR}/ohao/render/rt/env_cdf.cpp
)
add_executable(env_sun_test env_sun_test.cpp   # the environment's sun, found and painted out
    ${CMAKE_SOURCE_DIR}/ohao/render/rt/env_sun.cpp
)
target_link_libraries(env_sun_test PRIVATE glm)
add_executable(sobol_test sobol_test.cpp   # the path tracer's sampler against its CPU twin
    ${CMAKE_SOURCE_DIR}/ohao/render/rt/sobol_generator.cpp
    ${CMAKE_SOURCE_DIR}/ohao/render/rt/owen_scramble.cpp
)
target_link_libraries(sobol_test PRIVATE ohao_sobol_cpu)
add_executable(denoise_parse_test denoise_parse_test.cpp
    ${CMAKE_SOURCE_DIR}/ohao/render/rt/denoise/denoise_types.cpp
)
target_link_libraries(denoise_parse_test PRIVATE ohao_core)   # parse warnings go to the logger
add_executable(display_test display_test.cpp)   # the display transform: the module against its C++ twin
target_link_libraries(display_test PRIVATE ohao_display_cpu)
foreach(t env_cdf_test env_sun_test sobol_test denoise_parse_test display_test)
    target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/ohao)
    target_link_libraries(${t} PRIVATE ohao_gtest_main)
    if(UNIX)
        target_link_libraries(${t} PRIVATE pthread)
    endif()
    ohao_add_test(${t} LABEL quick COMMAND ${t})
endforeach()

# The GPU suites above: one device each, never two at once.
ohao_add_test(renderer_test LABEL gpu GPU COMMAND renderer_test)
foreach(t instantiate_gpu_test scene_view_gpu_test restore_render_gpu_test deferred_determinism_gpu_test
          cull_parity_gpu_test light_cluster_gpu_test light_emitter_gpu_test skinning_gpu_test vfx_gpu_test
          exposure_gpu_test)
    ohao_add_test(${t} LABEL gpu GPU COMMAND ${t})
endforeach()
