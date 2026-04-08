file(GLOB_RECURSE AUDIO_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE AUDIO_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

add_library(ohao_audio STATIC ${AUDIO_SOURCES} ${AUDIO_HEADERS})

target_include_directories(ohao_audio PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(ohao_audio PUBLIC
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
