# Jolt Physics - High-performance physics engine (MIT License)
# Used by Godot 4.x, Horizon Forbidden West - battle-tested AAA physics
# https://github.com/jrouwe/JoltPhysics

include(FetchContent)

# Disable Jolt's test/sample targets - we only need the library
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)

# Match our engine settings
set(DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)

# Try local pre-fetched first (same pattern as miniaudio/tinygltf)
# Jolt repo has CMakeLists.txt in Build/ subdirectory, not root
set(JOLT_LOCAL_DIR "${CMAKE_BINARY_DIR}/_deps/joltphysics-src")

if(EXISTS "${JOLT_LOCAL_DIR}/Build/CMakeLists.txt")
    message(STATUS "JoltPhysics: Using pre-fetched source at ${JOLT_LOCAL_DIR}/Build")
    FetchContent_Declare(
        JoltPhysics
        SOURCE_DIR "${JOLT_LOCAL_DIR}/Build"
    )
elseif(EXISTS "${JOLT_LOCAL_DIR}/CMakeLists.txt")
    message(STATUS "JoltPhysics: Using pre-fetched source at ${JOLT_LOCAL_DIR}")
    FetchContent_Declare(
        JoltPhysics
        SOURCE_DIR "${JOLT_LOCAL_DIR}"
    )
else()
    message(STATUS "JoltPhysics: Downloading v5.1.0 via ZIP...")
    FetchContent_Declare(
        JoltPhysics
        URL https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.1.0.zip
        SOURCE_SUBDIR Build
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
endif()

FetchContent_MakeAvailable(JoltPhysics)

# Verify the Jolt target was created
if(TARGET Jolt)
    message(STATUS "JoltPhysics: Jolt target available")
else()
    message(WARNING "JoltPhysics: Jolt target NOT found - physics will use null backend")
endif()
