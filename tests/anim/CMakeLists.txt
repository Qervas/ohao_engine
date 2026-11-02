# Animation (sub-project 10): tests 22 (assets, sampling) and 23 (the step), CPU only.
add_executable(anim_tests anim_tests.cpp)
target_compile_features(anim_tests PRIVATE cxx_std_20)
target_link_libraries(anim_tests PRIVATE ohao_anim ohao_ecs ohao_asset ohao_test_harness)
target_compile_definitions(anim_tests PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
    target_link_options(anim_tests PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
ohao_add_test(anim_tests LABEL quick COMMAND anim_tests)
