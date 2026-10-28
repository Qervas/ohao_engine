# OHAO Audio System Tests
# Tests AudioSystem initialization, volumes, handle safety

add_executable(audio_system_tests
    audio_system_tests.cpp
)
target_compile_features(audio_system_tests PRIVATE cxx_std_20)
target_compile_definitions(audio_system_tests PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_include_directories(audio_system_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/ohao
)

target_link_libraries(audio_system_tests PRIVATE
    ohao_audio
    glm
    ohao_test_harness
)

ohao_add_test(audio_system_tests LABEL quick COMMAND audio_system_tests)

# The schedule (sub-project 11): test 27's zones and graded occlusion in the step (Task 2), test 28
# (Task 3). CPU only; the play scene's level and physics.
add_executable(audio_schedule_tests audio_schedule_tests.cpp)
target_compile_features(audio_schedule_tests PRIVATE cxx_std_20)
target_compile_definitions(audio_schedule_tests PRIVATE OHAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(audio_schedule_tests PRIVATE ohao_level_world ohao_ecs ohao_physics ohao_audio ohao_test_harness)
if(MSVC)
    target_link_options(audio_schedule_tests PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
ohao_add_test(audio_schedule_tests LABEL quick COMMAND audio_schedule_tests)

message(STATUS "Audio system tests configured")
