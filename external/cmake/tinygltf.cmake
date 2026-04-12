# tinygltf - Header-only GLTF/GLB loader
# Check if we already have the header locally (pre-fetched)
set(TINYGLTF_LOCAL_DIR "${CMAKE_BINARY_DIR}/_deps/tinygltf-src")

if(EXISTS "${TINYGLTF_LOCAL_DIR}/tiny_gltf.h")
    message(STATUS "tinygltf: Using pre-fetched header at ${TINYGLTF_LOCAL_DIR}")
    set(tinygltf_SOURCE_DIR "${TINYGLTF_LOCAL_DIR}" CACHE INTERNAL "")
else()
    # Download via FetchContent
    include(FetchContent)
    FetchContent_Declare(
        tinygltf
        URL https://github.com/syoyo/tinygltf/archive/refs/tags/v2.9.3.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(TINYGLTF_HEADER_ONLY ON CACHE INTERNAL "")
    set(TINYGLTF_INSTALL OFF CACHE INTERNAL "")
    set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(tinygltf)
endif()
