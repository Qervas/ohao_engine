# Assimp - Open Asset Import Library (FBX, GLTF, OBJ, and 40+ formats)
include(FetchContent)

set(ASSIMP_LOCAL_DIR "${CMAKE_BINARY_DIR}/_deps/assimp-src")

# Always declare + make available (target must exist for linking)
FetchContent_Declare(
    assimp
    URL https://github.com/assimp/assimp/archive/refs/tags/v5.4.3.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(ASSIMP_BUILD_TESTS OFF CACHE INTERNAL "")
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE INTERNAL "")
set(ASSIMP_BUILD_SAMPLES OFF CACHE INTERNAL "")
set(ASSIMP_INSTALL OFF CACHE INTERNAL "")
set(ASSIMP_NO_EXPORT ON CACHE INTERNAL "")
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE INTERNAL "")
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE INTERNAL "")
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE INTERNAL "")
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE INTERNAL "")
set(ASSIMP_BUILD_COLLADA_IMPORTER ON CACHE INTERNAL "")
FetchContent_MakeAvailable(assimp)
