# Combat (sub-project 7): test 15 (weapon, health, damage), CPU only.
add_executable(combat_tests combat_tests.cpp)
target_compile_features(combat_tests PRIVATE cxx_std_20)
target_link_libraries(combat_tests PRIVATE ohao_level_world ohao_ecs ohao_physics ohao_test_harness)
target_compile_definitions(combat_tests PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_link_options(combat_tests PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
ohao_add_test(combat_tests LABEL quick COMMAND combat_tests)
