# Recast/Detour v1.6.0 (zlib licence; Mikko Mononen, https://github.com/recastnavigation/recastnavigation): the
# navigation module's navmesh (ohao/navigation; docs/specs/2026-11-18-tidewall-design.md, 3.1). Approved by Frank,
# 2026-11-18. The release's tag zip, its SHA-256 the file's as downloaded 2026-11-19; only populated, never its own
# CMakeLists (no demo, tests or examples), and only what the module uses is compiled: Recast, Detour, DetourTileCache,
# and of DetourCrowd the corridor and the sampled-velocity avoidance (not dtCrowd). Compiled with the engine's machine
# flags (ohao_machine), as the spec's determinism asks: its float results are one build's.
include(FetchContent)
FetchContent_Declare(
    recastnavigation
    URL https://github.com/recastnavigation/recastnavigation/archive/refs/tags/v1.6.0.zip
    URL_HASH SHA256=8b50c62177249554b226514b307e6c529934a62b3926822777bf6abb8906a155
    SOURCE_SUBDIR none   # no CMakeLists there: populated, not added
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(recastnavigation)

set(_rc ${recastnavigation_SOURCE_DIR})
add_library(ohao_recast STATIC
    ${_rc}/Recast/Source/Recast.cpp ${_rc}/Recast/Source/RecastAlloc.cpp ${_rc}/Recast/Source/RecastArea.cpp
    ${_rc}/Recast/Source/RecastAssert.cpp ${_rc}/Recast/Source/RecastContour.cpp ${_rc}/Recast/Source/RecastFilter.cpp
    ${_rc}/Recast/Source/RecastLayers.cpp ${_rc}/Recast/Source/RecastMesh.cpp ${_rc}/Recast/Source/RecastMeshDetail.cpp
    ${_rc}/Recast/Source/RecastRasterization.cpp ${_rc}/Recast/Source/RecastRegion.cpp
    ${_rc}/Detour/Source/DetourAlloc.cpp ${_rc}/Detour/Source/DetourAssert.cpp ${_rc}/Detour/Source/DetourCommon.cpp
    ${_rc}/Detour/Source/DetourNavMesh.cpp ${_rc}/Detour/Source/DetourNavMeshBuilder.cpp
    ${_rc}/Detour/Source/DetourNavMeshQuery.cpp ${_rc}/Detour/Source/DetourNode.cpp
    ${_rc}/DetourTileCache/Source/DetourTileCache.cpp ${_rc}/DetourTileCache/Source/DetourTileCacheBuilder.cpp
    ${_rc}/DetourCrowd/Source/DetourObstacleAvoidance.cpp ${_rc}/DetourCrowd/Source/DetourPathCorridor.cpp)
target_include_directories(ohao_recast SYSTEM PUBLIC ${_rc}/Recast/Include ${_rc}/Detour/Include
    ${_rc}/DetourTileCache/Include ${_rc}/DetourCrowd/Include)
target_link_libraries(ohao_recast PRIVATE ohao_machine)
target_compile_options(ohao_recast PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
set_target_properties(ohao_recast PROPERTIES POSITION_INDEPENDENT_CODE ON)
unset(_rc)
