# Level collision (sub-project 5, step 4): tests 6 and 7, CPU only.
add_executable(level_physics_tests level_physics_tests.cpp)
target_compile_features(level_physics_tests PRIVATE cxx_std_20)
target_link_libraries(level_physics_tests PRIVATE ohao_level_world ohao_ecs ohao_physics ohao_test_harness)
target_compile_definitions(level_physics_tests PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_link_options(level_physics_tests PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
ohao_add_test(level_physics_tests LABEL quick COMMAND level_physics_tests)
