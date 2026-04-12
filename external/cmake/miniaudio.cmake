# miniaudio - Single-header audio library (public domain)
# Provides WAV/MP3/FLAC decoding, mixing, and 3D spatialization
set(MINIAUDIO_LOCAL_DIR "${CMAKE_BINARY_DIR}/_deps/miniaudio-src")

if(EXISTS "${MINIAUDIO_LOCAL_DIR}/miniaudio.h")
    message(STATUS "miniaudio: Using pre-fetched header at ${MINIAUDIO_LOCAL_DIR}")
    set(miniaudio_SOURCE_DIR "${MINIAUDIO_LOCAL_DIR}" CACHE INTERNAL "")
else()
    include(FetchContent)
    FetchContent_Declare(
        miniaudio
        URL https://github.com/mackron/miniaudio/archive/refs/tags/0.11.21.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(miniaudio)
    set(miniaudio_SOURCE_DIR "${miniaudio_SOURCE_DIR}" CACHE INTERNAL "")
endif()

# Create header-only interface target
if(NOT TARGET miniaudio)
    add_library(miniaudio INTERFACE)
    target_include_directories(miniaudio INTERFACE "${miniaudio_SOURCE_DIR}")
endif()
