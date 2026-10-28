# VFX (sub-project 12): test 31, the effect set, the closed form and the spawn records the step
# writes (Task 1). CPU only; the play scene's level, physics, combat and animation.
add_executable(vfx_tests vfx_tests.cpp)
target_compile_features(vfx_tests PRIVATE cxx_std_20)
target_compile_definitions(vfx_tests PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(vfx_tests PRIVATE ohao_level_world ohao_ecs ohao_physics ohao_vfx ohao_test_harness)
if(MSVC)
    target_link_options(vfx_tests PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
ohao_add_test(vfx_tests LABEL quick COMMAND vfx_tests)
