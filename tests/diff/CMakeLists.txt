# Differentiable renderer tests.
#   diff_unit_tests  -- GoogleTest, CPU logic + bare-instance device queries
#   diff_gpu_probe   -- standalone exe, requires a working device (added in
#                       Stage 0a Task 3)

add_executable(diff_unit_tests diff_unit_tests.cpp)
target_include_directories(diff_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/ohao
    ${Vulkan_INCLUDE_DIRS}
)
target_compile_features(diff_unit_tests PRIVATE cxx_std_20)
target_link_libraries(diff_unit_tests PRIVATE
    ohao_diff gtest gtest_main ${Vulkan_LIBRARIES})
if(UNIX)
    target_link_libraries(diff_unit_tests PRIVATE pthread)
endif()
include(GoogleTest)
gtest_discover_tests(diff_unit_tests)

add_executable(diff_gpu_probe
    diff_gpu_probe.cpp
    gpu_probe_context.cpp
    context/probe_scene.cpp
    context/probes_basic.cpp
    context/probes_wavefront.cpp
    context/probes_scatter.cpp
    context/probes_fused_loop.cpp
    context/probes_replay.cpp
    context/probes_parity.cpp
    context/probes_gradient.cpp
    context/probes_loss.cpp
    probe/oracle_bsdf.cpp
    probe/oracle_integrator.cpp
    probe/fd_harness.cpp
    probe/scene.cpp
    probe/ties.cpp
    probe/checks_foundation.cpp
    probe/checks_wavefront.cpp
    probe/checks_bsdf.cpp
    probe/checks_env.cpp
    probe/checks_nee_film.cpp
    probe/checks_parity.cpp
    probe/checks_replay.cpp
    probe/checks_gradients.cpp
    probe/checks_convergence.cpp
    probe/checks_adjoint_seed.cpp
    probe/checks_loss.cpp
    probe/checks_texture.cpp
)
target_include_directories(diff_gpu_probe PRIVATE
    ${CMAKE_SOURCE_DIR}/ohao
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${Vulkan_INCLUDE_DIRS}
)
target_compile_features(diff_gpu_probe PRIVATE cxx_std_20)
# Stage 0b-1 Task 6: runVisibilityProbe uses ohao::RTAccelerationStructure
# (ohao/render/rt/rt_acceleration_structure.cpp), which lives in the
# ohao_renderer target, not ohao_gpu_vulkan -- link it in.
target_link_libraries(diff_gpu_probe PRIVATE
    ohao_diff ohao_gpu_vulkan ohao_renderer glm VulkanMemoryAllocator ${Vulkan_LIBRARIES})
if(MSVC)
    target_link_options(diff_gpu_probe PRIVATE /FORCE:MULTIPLE)
endif()
# diff_gpu_probe does not copy shader SPVs itself -- the shaders target
# owns that copy (shaders/CMakeLists.txt) once, so that this target and
# renderer_test don't race two independent copy_directory commands into the
# same destination under a parallel (-j8) build (Stage 0b-2a Task 5, the
# shader-copy race). This dependency still matters on its own: without it,
# building diff_gpu_probe alone after editing a .comp file could run before
# the shaders target has (re)compiled and copied its output, leaving a stale
# or missing SPV in place. Stage 0b-1 Task 6 (the visibility probe) added
# visibility_probe.comp to this same target, so staleness here would let it
# validate ray-query behaviour against a shader it never actually built.
#
# Bare "Task N" numbers restart at every stage, so both references above name
# the stage as well: they are three lines apart and mean different stages.
add_dependencies(diff_gpu_probe shaders)
