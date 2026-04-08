# OHAO Audio System Tests
# Tests AudioSystem initialization, volumes, handle safety

add_executable(audio_system_tests
    audio_system_tests.cpp
)

target_include_directories(audio_system_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/ohao
)

target_link_libraries(audio_system_tests PRIVATE
    ohao_audio
    glm
)

# Add test to CTest
add_test(NAME AudioSystemTests COMMAND audio_system_tests)

message(STATUS "Audio system tests configured")
