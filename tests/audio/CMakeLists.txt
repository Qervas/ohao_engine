# OHAO Audio System Tests
# Tests AudioSystem initialization, volumes, handle safety

add_executable(audio_system_tests
    audio_system_tests.cpp
)
target_compile_features(audio_system_tests PRIVATE cxx_std_20)

target_include_directories(audio_system_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/ohao
)

target_link_libraries(audio_system_tests PRIVATE
    ohao_audio
    glm
    ohao_test_harness
)

ohao_add_test(audio_system_tests LABEL quick COMMAND audio_system_tests)

message(STATUS "Audio system tests configured")
