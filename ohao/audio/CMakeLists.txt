file(GLOB_RECURSE AUDIO_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE AUDIO_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
# The bank (the .audio.json reader) is plain data with no miniaudio: the ECS's audio schedule reads
# it (ecs/audio_schedule.cpp) without linking the mixer.
list(FILTER AUDIO_SOURCES EXCLUDE REGEX "audio_bank\\.cpp$")
list(FILTER AUDIO_HEADERS EXCLUDE REGEX "audio_bank\\.hpp$")

add_library(ohao_audio_bank STATIC audio_bank.cpp audio_bank.hpp)
target_include_directories(ohao_audio_bank PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_audio_bank PUBLIC cxx_std_20)
target_link_libraries(ohao_audio_bank PUBLIC ohao_core PRIVATE ohao_simdjson)

add_library(ohao_audio STATIC ${AUDIO_SOURCES} ${AUDIO_HEADERS})

target_include_directories(ohao_audio PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/ohao
)

target_compile_features(ohao_audio PUBLIC cxx_std_20)

target_link_libraries(ohao_audio PUBLIC
    ohao_core
    ohao_audio_bank
    miniaudio
    glm
)

# miniaudio needs platform audio backends
if(WIN32)
    # Windows: no extra libs needed (COM auto-initialized by miniaudio)
elseif(APPLE)
    find_library(COREAUDIO_FRAMEWORK CoreAudio)
    find_library(AUDIOUNIT_FRAMEWORK AudioUnit)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
    target_link_libraries(ohao_audio PUBLIC
        ${COREAUDIO_FRAMEWORK}
        ${AUDIOUNIT_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
    )
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(ohao_audio PUBLIC
        Threads::Threads
        dl
        m
    )
endif()
